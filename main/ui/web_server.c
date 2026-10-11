// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// web_server.c — the HTTP control panel.
//
// One job: serve the embedded web UI page and its REST-ish /api/* endpoints,
// plus the hard-won heap-gated start/retry machinery the httpd task stack
// needs on this internal-RAM-starved board. WiFi itself (NVS credentials,
// STA/AP fallback, brownout mitigation, light-sleep suspend/resume, the
// `wifi` CLI command) lives in wifi_manager.c, which drives this module's
// lifecycle through web_server_start()/web_server_stop(); web_server never
// calls back into wifi_manager except the read-only status getter (for
// /api/status and the ready log) and the credential-save helpers behind
// /api/wifi/*.

#include "web_server.h"
#include "wifi_manager.h"
#include "common.h"
#include "audio_pipeline.h"
#include "audio_dsp.h"
#include "dsp_effects.h"
#include "audio_codec.h"
#include "audio_store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>

static const char *TAG = "web";

static httpd_handle_t s_srv = NULL;
static bool s_started = false;   // httpd started at least once (sticky; see web_server.h)

bool web_server_started(void) { return s_started; }

// ==========================================================
// HTTP body / form helpers
// ==========================================================

// Read entire request body into a heap buffer. Caller must free(). Max = max_len bytes.
static char *read_body(httpd_req_t *req, int max_len)
{
    int len = req->content_len;
    if (len <= 0 || len > max_len) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = '\0';
    return buf;
}

static void url_decode(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '+') {
            *d++ = ' '; s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) &&
                   isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *d++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}

// Find value for key in a URL-encoded body string.
static int form_val(const char *body, const char *key, char *out, int out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = body;
    while ((p = strstr(p, search)) != NULL) {
        if (p == body || *(p - 1) == '&') break;
        p++;
    }
    if (!p) return 0;
    p += strlen(search);
    const char *end = strchr(p, '&');
    int vlen = end ? (int)(end - p) : (int)strlen(p);
    if (vlen >= out_len) vlen = out_len - 1;
    memcpy(out, p, vlen);
    out[vlen] = '\0';
    url_decode(out);
    return 1;
}

// ==========================================================
// Embedded web UI
// ==========================================================
// The page lives in web_page.html (same directory) and is embedded at build
// time via EMBED_TXTFILES in main/CMakeLists.txt — real HTML/CSS/JS a browser
// and an editor can work with, instead of the hand-escaped C string literal
// it used to be. The mode buttons are built client-side from /api/modes, so
// out-of-tree effects appear without touching the page.
extern const char _binary_web_page_html_start[];   // NUL-terminated (TXTFILES)


// ==========================================================
// HTTP handlers
// ==========================================================

static bool sd_name_ok(const char *name);   // shared SD filename validation (defined below)

static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, _binary_web_page_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req)
{
    const char *mode_str = dsp_current()->name;
    const char *in_str = (g_state.input_mode == INPUT_MIC) ? "mic" : "diff";

    bool recording = audio_store_is_recording();
    bool playing   = audio_store_is_playing();

    // Effect detail is NOT in the status JSON — the page fetches the current
    // effect's config from the generic GET /api/fx/<name> when it needs it.
    // The one generic live value is the meter (analyzer effects publish it).
    float    m_val, m_conf;
    uint32_t m_seq;
    dsp_meter_read(&m_val, &m_conf, &m_seq);

    // WiFi state belongs to wifi_manager — the one read-only call back in.
    bool wifi_disabled, wifi_is_ap;
    char wifi_ip[24];
    wifi_manager_get_wifi_status(&wifi_disabled, &wifi_is_ap, wifi_ip, sizeof(wifi_ip));

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"mode\":\"%s\","
        "\"input\":\"%s\","
        "\"gain\":%d,"
        "\"gain_out\":%d,"
        "\"sd\":\"%s\","
        "\"meter\":{\"value\":%.4f,\"conf\":%.3f,\"seq\":%lu},"
        "\"recording\":%d,"
        "\"playing\":%d,"
        "\"wifi\":\"%s %s\""
        "}",
        mode_str, in_str, g_state.input_gain_db, g_state.output_gain_db,
        g_state.sd_card ? "mounted" : "not mounted",
        m_val, m_conf, (unsigned long)m_seq,
        recording ? 1 : 0,
        playing   ? 1 : 0,
        wifi_is_ap ? "AP:" : "STA:", wifi_ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, buf);
}

// Registry listing for the web UI: the page builds its Mode buttons AND its
// effect cards from this at load, so a newly registered effect appears with
// no page edits, as it does in the CLI hint and on the panel buttons. Per effect: "config" = has a text config (generic card
// over /api/fx/<name>), "panel" = supplies its own card (/api/fx/<name>/panel).
// Sent one chunk per effect, so the registry size never overflows a buffer.

// Copy s into out as JSON string content (quotes, backslashes and control
// characters escaped). Effect names and labels come from out-of-tree code, so
// one stray quote must not break the whole list. Returns false if out is too
// small.
static bool json_escape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[7];
        int n;
        if (c == '"' || c == '\\') n = snprintf(esc, sizeof(esc), "\\%c", c);
        else if (c < 0x20)         n = snprintf(esc, sizeof(esc), "\\u%04x", c);
        else { esc[0] = (char)c; n = 1; }
        if (o + (size_t)n >= cap) return false;
        memcpy(out + o, esc, (size_t)n);
        o += (size_t)n;
    }
    out[o] = '\0';
    return true;
}

static esp_err_t h_modes(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (httpd_resp_send_chunk(req, "[", 1) != ESP_OK) return ESP_FAIL;
    char name[40], label[96], group[40], item[256];
    bool first = true;
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *fx = dsp_effect_at(i);
        if (!json_escape(fx->name, name, sizeof(name)) ||
            !json_escape(fx->label ? fx->label : fx->name, label, sizeof(label)) ||
            !json_escape(dsp_effect_group(fx), group, sizeof(group))) {
            continue;   // absurdly long name/label/group: skip it, keep the JSON valid
        }
        // "led" is the color LED 3 actually shows (dsp_effect_led: bypass
        // #000000, unset led_rgb dim white).
        int n = snprintf(item, sizeof(item),
                         "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"%s\","
                         "\"led\":\"#%06lX\",\"config\":%s,\"panel\":%s}",
                         first ? "" : ",", name, label, group,
                         (unsigned long)dsp_effect_led(fx),
                         (fx->save_text && fx->load_text) ? "true" : "false",
                         fx->web_panel ? "true" : "false");
        if (n < 0 || n >= (int)sizeof(item)) continue;
        if (httpd_resp_send_chunk(req, item, n) != ESP_OK) return ESP_FAIL;
        first = false;
    }
    if (httpd_resp_send_chunk(req, "]", 1) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_mode(httpd_req_t *req)
{
    char *body = read_body(req, 64);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char mode[16] = {0};
    form_val(body, "mode", mode, sizeof(mode));
    free(body);

    int idx = dsp_find(mode);   // accepts names and effect aliases
    if (idx < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown mode"); return ESP_FAIL; }
    dsp_set(idx);

    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t h_input(httpd_req_t *req)
{
    char *body = read_body(req, 64);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char inp[16] = {0};
    form_val(body, "input", inp, sizeof(inp));
    free(body);

    if      (strcmp(inp, "mic")  == 0) { g_state.input_mode = INPUT_MIC;       g_state.input_gain_db = 20; }
    else if (strcmp(inp, "diff") == 0) { g_state.input_mode = INPUT_LINE_DIFF; g_state.input_gain_db = 0;  }
    else { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown input"); return ESP_FAIL; }

    codec_apply_settings();
    return httpd_resp_sendstr(req, "OK");
}

// Form fields: gain=<0..47> (input MICPGA) and/or gain_out=<-6..29> (HP/LO
// driver). Either may be sent alone.
static esp_err_t h_gain(httpd_req_t *req)
{
    char *body = read_body(req, 48);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char val[8] = {0}, val_out[8] = {0};
    form_val(body, "gain", val, sizeof(val));
    form_val(body, "gain_out", val_out, sizeof(val_out));
    free(body);

    if (val[0]) {
        int db = atoi(val);
        if (db < 0 || db > 47) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Gain 0-47"); return ESP_FAIL; }
        g_state.input_gain_db = db;
        codec_apply_input_gain();
    }
    if (val_out[0]) {
        int db = atoi(val_out);
        if (db < -6 || db > 29) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Output gain -6..29"); return ESP_FAIL; }
        g_state.output_gain_db = db;
        codec_apply_output_gain();
    }
    return httpd_resp_sendstr(req, "OK");
}

// ==========================================================
// Generic effect config endpoints — /api/fx/<name>
// ==========================================================
// The web API reaches effect parameters ONLY through the registry's
// save_text/load_text hooks (dsp_effects.h), the same one text format the
// settings store persists:
//   GET  /api/fx/<name>        -> the effect's current config as text
//   POST /api/fx/<name>        <- directive lines / coefficient lines to apply
//   GET  /api/fx/<name>/panel  -> the effect's own web card (its web_panel
//                                 HTML fragment); 404 means "use the
//                                 generic card"
// An out-of-tree effect gets all of these for free; the firmware carries no
// per-effect endpoint.

// Resolve "/api/fx/<name>" or "/api/fx/<name>/panel" (query string ignored).
// *panel reports whether the /panel suffix was present.
static const dsp_effect_t *fx_from_uri(httpd_req_t *req, bool *panel)
{
    const char *p = req->uri + strlen("/api/fx/");
    char clean[16] = {0};
    size_t i = 0;
    while (i < sizeof(clean) - 1 && p[i] && p[i] != '?' && p[i] != '/') {
        clean[i] = p[i];
        i++;
    }
    *panel = (strncmp(p + i, "/panel", 6) == 0) && (p[i + 6] == '\0' || p[i + 6] == '?');
    int idx = dsp_find(clean);
    return (idx >= 0) ? dsp_effect_at(idx) : NULL;
}

static esp_err_t h_fx_get(httpd_req_t *req)
{
    bool panel;
    const dsp_effect_t *fx = fx_from_uri(req, &panel);
    if (panel) {
        if (!fx || !fx->web_panel) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No panel for this effect");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_sendstr(req, fx->web_panel);   // flash-resident, NUL-terminated
    }
    if (!fx || !fx->save_text) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such configurable effect");
        return ESP_FAIL;
    }
    char *buf = malloc(8 * 1024);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    int n = fx->save_text(buf, 8 * 1024);
    if (n < 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t h_fx_post(httpd_req_t *req)
{
    bool panel;
    const dsp_effect_t *fx = fx_from_uri(req, &panel);
    if (panel || !fx || !fx->load_text) {   // the panel is read-only
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such configurable effect");
        return ESP_FAIL;
    }
    char *body = read_body(req, 16 * 1024);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
        return ESP_FAIL;
    }
    bool ok = fx->load_text(body);
    free(body);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Config rejected");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t h_upload_sd(httpd_req_t *req)
{
    if (!g_state.sd_card) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No SD card mounted");
        return ESP_FAIL;
    }

    // Filename from query: ?name=foo.wav
    char filename[128] = "/sdcard/upload.bin";
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char name[64] = {0};
        if (httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK) {
            url_decode(name);
            if (!sd_name_ok(name)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "Invalid name (no path separators, quotes, or leading dots)");
                return ESP_FAIL;
            }
            snprintf(filename, sizeof(filename), "/sdcard/%s", name);
        }
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        char errmsg[80];
        snprintf(errmsg, sizeof(errmsg), "Cannot create file: %s (errno %d)", strerror(errno), errno);
        ESP_LOGE(TAG, "%s — path: %s", errmsg, filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errmsg);
        return ESP_FAIL;
    }

    char *buf = malloc(16 * 1024);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int remaining = req->content_len;
    int total = 0;
    bool io_err = false;
    while (remaining > 0) {
        int chunk = remaining < (16 * 1024) ? remaining : (16 * 1024);
        int n = httpd_req_recv(req, buf, chunk);
        if (n <= 0) break;
        if ((int)fwrite(buf, 1, n, f) != n) { io_err = true; break; }  // card full / write error
        remaining -= n;
        total += n;
    }
    free(buf);
    if (fclose(f) != 0) io_err = true;   // buffered tail can still fail here

    // A truncated upload must not report success — remove the partial file
    // so the client can't mistake it for the real one.
    if (io_err || remaining > 0) {
        remove(filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            io_err ? "SD write failed (card full?)" : "Upload interrupted");
        return ESP_FAIL;
    }

    char resp[256];
    snprintf(resp, sizeof(resp), "OK: %d bytes -> %s", total, filename);
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t h_upload_flash(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    // EXTERNAL source: the audio hot path must not submit into the ring while
    // the upload feeds it, or live audio interleaves into the sample.
    esp_err_t err = audio_store_record_start(AUDIO_BACKEND_FLASH, NULL,
                                             FS_HZ, AUDIO_BITS_PER_SAMPLE, AUDIO_CHANNELS,
                                             AUDIO_REC_SRC_EXTERNAL);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Flash sample record start failed (recording, live capture, or flash playback active?)");
        return ESP_FAIL;
    }

    char buf[512];
    int remaining = total;
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, chunk);
        if (n <= 0) break;
        audio_store_record_submit(buf, n);
        remaining -= n;
    }
    audio_store_record_stop();

    char resp[64];
    snprintf(resp, sizeof(resp), "OK: %d bytes written to flash sample", total);
    return httpd_resp_sendstr(req, resp);
}

// Shared SD filename validation for the upload/download handlers. Files live
// flat in /sdcard, so any path separator (or '..' by implication) is refused
// outright rather than stripped — a visibly complete rule beats a clever one.
// Quotes, backslashes, and control characters are refused too so accepted
// names can be embedded verbatim in the /api/files JSON and the
// Content-Disposition header.
static bool sd_name_ok(const char *name)
{
    if (!name || !name[0] || name[0] == '.') return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || (unsigned char)*p < 0x20)
            return false;
    }
    return true;
}

static esp_err_t h_list_files(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!g_state.sd_card) {
        return httpd_resp_sendstr(req, "[]");
    }

    DIR *dir = opendir("/sdcard");
    if (!dir) {
        return httpd_resp_sendstr(req, "[]");
    }

    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[264];  /* "/sdcard/" (8) + NAME_MAX (255) + '\0' */
        snprintf(path, sizeof(path), "/sdcard/%s", ent->d_name);
        struct stat st;
        long size = (stat(path, &st) == 0) ? (long)st.st_size : -1;

        // Names can arrive via a PC card reader with characters the upload
        // sanitiser refuses — escape quotes/backslashes so one odd filename
        // can't break the JSON for the whole listing.
        char jname[128];
        int j = 0;
        for (const char *p = ent->d_name; *p && j < (int)sizeof(jname) - 2; p++) {
            if (*p == '"' || *p == '\\') jname[j++] = '\\';
            jname[j++] = *p;
        }
        jname[j] = '\0';

        char entry[192];
        int n = snprintf(entry, sizeof(entry),
            "%s{\"name\":\"%s\",\"size\":%ld}",
            first ? "" : ",", jname, size);
        httpd_resp_send_chunk(req, entry, n);
        first = false;
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Smallest free-stack watermark ever seen in the SD download handler, in words
// (0 = never exercised). See the note where it is written.
static UBaseType_t s_httpd_stack_hwm_words = 0;

// Unused headroom at the deepest point of the download path, in bytes; 0 until
// a download has actually run.
size_t web_server_httpd_stack_headroom(void)
{
    return (size_t)s_httpd_stack_hwm_words * sizeof(StackType_t);
}

static esp_err_t h_download_sd(httpd_req_t *req)
{
    if (!g_state.sd_card) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No SD card mounted");
        return ESP_FAIL;
    }

    char query[128];
    char name[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
        url_decode(name);
    }
    if (!name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    if (!sd_name_ok(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
        return ESP_FAIL;
    }

    char path[160];
    snprintf(path, sizeof(path), "/sdcard/%s", name);

    struct stat st;
    if (stat(path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char cd[128];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    // No Content-Length here: the body goes out via httpd_resp_send_chunk,
    // i.e. Transfer-Encoding: chunked, and RFC 7230 forbids sending both
    // (clients ignore the length on a chunked response anyway).

    char *buf = malloc(4 * 1024);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }

    size_t n_read;
    while ((n_read = fread(buf, 1, 4 * 1024, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n_read) != ESP_OK) break;
    }
    free(buf);
    fclose(f);

    // Record how close this path came to the bottom of the httpd stack. This is
    // the handler the internal-RAM ledger (docs/ARCHITECTURE.md) says
    // HTTPD_STACK_SIZE is sized for, so it is the measurement that justifies
    // (or forbids) shrinking that stack to fit the budget. Stored only —
    // emitting a status line here is exactly what corrupted transfers twice
    // before (rule 4 of the resource-change checklist); read via `status`.
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    if (s_httpd_stack_hwm_words == 0 || hwm < s_httpd_stack_hwm_words) {
        s_httpd_stack_hwm_words = hwm;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Restart helper — delays briefly so HTTP response can be sent first
static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t h_wifi_sta(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char ssid[33] = {0}, pass[65] = {0};
    form_val(body, "ssid", ssid, sizeof(ssid));
    form_val(body, "pass", pass, sizeof(pass));
    free(body);

    if (!ssid[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID empty"); return ESP_FAIL; }
    wifi_manager_save_sta_creds(ssid, pass);
    httpd_resp_sendstr(req, "OK: credentials saved, restarting...");
    // Task creation can fail under the internal-heap pressure this project
    // documents as its primary failure mode — the promised restart must still
    // happen, so fall back to restarting from here (response is already queued).
    if (xTaskCreatePinnedToCore(restart_task, "rst", 2048, NULL, 5, NULL, 0) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t h_wifi_ap(httpd_req_t *req)
{
    wifi_manager_save_force_ap();
    httpd_resp_sendstr(req, "OK: AP mode saved, restarting...");
    if (xTaskCreatePinnedToCore(restart_task, "rst", 2048, NULL, 5, NULL, 0) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1200));   // let the response flush (see h_wifi_sta)
        esp_restart();
    }
    return ESP_OK;
}

// ==========================================================
// Flash WAV streaming
// ==========================================================

static esp_err_t flash_wav_chunk(const void *data, size_t len, void *ctx)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, (const char *)data, (ssize_t)len);
}

static esp_err_t h_flash_wav(httpd_req_t *req)
{
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = audio_store_flash_write_wav(flash_wav_chunk, req);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No sample in flash");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stream error");
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ==========================================================
// Record / Playback handlers
// ==========================================================

static esp_err_t h_rec_start(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char be[8] = {0}, path[128] = {0}, tap[8] = {0};
    form_val(body, "backend", be,   sizeof(be));
    form_val(body, "path",    path, sizeof(path));
    form_val(body, "tap",     tap,  sizeof(tap));
    free(body);

    // Optional tap selector: post = filtered DSP output, pre = raw input.
    if (tap[0]) audio_store_set_record_tap(strcmp(tap, "post") == 0 ? AUDIO_TAP_POST : AUDIO_TAP_PRE);

    audio_backend_t backend = (strcmp(be, "sd") == 0) ? AUDIO_BACKEND_SD : AUDIO_BACKEND_FLASH;
    // Tap-aware format (TDM: pre = all mic channels at slot width, post =
    // stereo mixdown). record_start would override a mismatch anyway.
    uint16_t rec_ch   = (audio_store_get_record_tap() == AUDIO_TAP_PRE)
                          ? (uint16_t)g_geom.rx_ch : 2;
    uint16_t rec_bits = (uint16_t)g_geom.slot_bits;
    esp_err_t err = audio_store_record_start(backend, path[0] ? path : NULL,
                                             FS_HZ, rec_bits, rec_ch,
                                             AUDIO_REC_SRC_AUDIO);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Already recording or playing flash");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Record start failed");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "OK: recording started");
}

static void rec_stop_task(void *arg)
{
    audio_store_record_stop();  // blocks until writer task finishes (erase+write for flash)
    vTaskDelete(NULL);
}

static esp_err_t h_rec_stop(httpd_req_t *req)
{
    if (!audio_store_is_recording()) {
        return httpd_resp_sendstr(req, "OK: not recording");
    }
    // Fire and forget — the blocking erase+write runs in a background task.
    // The 2-second status poll will reflect recording=false when done.
    // If task creation fails (internal-heap pressure), stop synchronously —
    // blocking this handler for the drain beats never stopping the recorder.
    if (xTaskCreatePinnedToCore(rec_stop_task, "rec_stop_web", 3072, NULL, 5, NULL, 0) != pdPASS) {
        audio_store_record_stop();
        return httpd_resp_sendstr(req, "OK: saved");
    }
    return httpd_resp_sendstr(req, "OK: saving...");
}

static esp_err_t h_play_start(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char be[8] = {0}, path[128] = {0}, loop_s[4] = {0};
    form_val(body, "backend", be,     sizeof(be));
    form_val(body, "path",    path,   sizeof(path));
    form_val(body, "loop",    loop_s, sizeof(loop_s));
    free(body);

    audio_backend_t backend = (strcmp(be, "sd") == 0) ? AUDIO_BACKEND_SD : AUDIO_BACKEND_FLASH;
    bool loop = (loop_s[0] == '1');
    esp_err_t err = audio_store_play_start(backend, path[0] ? path : NULL, loop);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No sample found in flash");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Play start failed");
        return ESP_FAIL;
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "OK: playing [%s]%s", be, loop ? " (loop)" : "");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t h_play_stop(httpd_req_t *req)
{
    audio_store_play_stop();
    return httpd_resp_sendstr(req, "OK: stopped");
}

// ==========================================================
// Live capture stream — chunked WAV of the post/pre-DSP signal
// ==========================================================

// The live stream is unbounded, so it runs on its OWN task via the async-handler
// API. If it ran inline it would occupy the single httpd worker for the whole
// stream (up to send_wait_timeout), blacking out the rest of the web UI.
static void rec_live_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    uint8_t *buf = malloc(8 * 1024);
    if (buf) {
        httpd_resp_set_type(req, "audio/wav");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");

        audio_store_live_open();

        uint8_t hdr[44];
        audio_store_live_wav_header(hdr);
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));

        while (err == ESP_OK) {
            size_t n = audio_store_live_read(buf, 8 * 1024, 500);
            if (n == 0) {
                if (!audio_store_live_active()) break;   // closed elsewhere
                continue;                                 // brief gap — keep streaming
            }
            err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n);  // != OK on disconnect
        }

        audio_store_live_close();
        // Splice tripwire (issue #12 class): live has no finalize step, so
        // report upstream I2S loss when the stream ends.
        uint32_t ovf = audio_store_live_overruns();
        if (ovf > 0)
            ESP_LOGW(TAG, "%u I2S RX overrun(s) during live stream — audio has splice gaps",
                     (unsigned)ovf);
        httpd_resp_send_chunk(req, NULL, 0);  // best-effort terminator
        free(buf);
    }

    httpd_req_async_handler_complete(req);    // release the request/socket
    vTaskDelete(NULL);
}

static esp_err_t h_rec_live(httpd_req_t *req)
{
    if (audio_store_live_active()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Live capture already in use");
        return ESP_FAIL;
    }

    // Optional ?tap=pre|post selects the captured signal before arming.
    char q[40], tap[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "tap", tap, sizeof(tap));
    if (tap[0]) audio_store_set_record_tap(strcmp(tap, "post") == 0 ? AUDIO_TAP_POST : AUDIO_TAP_PRE);

    // Hand the request to a dedicated task so the httpd worker is freed at once.
    httpd_req_t *areq = NULL;
    if (httpd_req_async_handler_begin(req, &areq) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async begin failed");
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(rec_live_task, "rec_live", 4096, areq, 5, NULL, 0) != pdPASS) {
        httpd_req_async_handler_complete(areq);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;   // worker returns immediately; streaming continues on rec_live_task
}

// ==========================================================
// HTTP server start
// ==========================================================

#define URI(m, u, h) { .uri = (u), .method = (m), .handler = (h), .user_ctx = NULL }

static const httpd_uri_t s_uris[] = {
    URI(HTTP_GET,  "/",                   h_root),
    URI(HTTP_GET,  "/favicon.ico",        h_favicon),
    URI(HTTP_GET,  "/api/status",         h_status),
    URI(HTTP_GET,  "/api/modes",          h_modes),
    URI(HTTP_POST, "/api/mode",           h_mode),
    URI(HTTP_POST, "/api/input",          h_input),
    URI(HTTP_POST, "/api/gain",           h_gain),
    URI(HTTP_GET,  "/api/fx/*",           h_fx_get),
    URI(HTTP_POST, "/api/fx/*",           h_fx_post),
    URI(HTTP_GET,  "/api/flash/wav",       h_flash_wav),
    URI(HTTP_POST, "/api/rec/start",       h_rec_start),
    URI(HTTP_POST, "/api/rec/stop",        h_rec_stop),
    URI(HTTP_GET,  "/api/rec/live",        h_rec_live),
    URI(HTTP_POST, "/api/play/start",      h_play_start),
    URI(HTTP_POST, "/api/play/stop",       h_play_stop),
    URI(HTTP_GET,  "/api/files",           h_list_files),
    URI(HTTP_GET,  "/api/download/sd*",   h_download_sd),
    URI(HTTP_POST, "/api/upload/sd*",     h_upload_sd),
    URI(HTTP_POST, "/api/upload/flash",   h_upload_flash),
    URI(HTTP_POST, "/api/wifi/sta",       h_wifi_sta),
    URI(HTTP_POST, "/api/wifi/ap",        h_wifi_ap),
};

// httpd_start needs a contiguous HTTPD_STACK_SIZE internal block for its task
// stack right at the WiFi bring-up heap peak. Two hard-won facts (2026-07-08,
// measured; see "Internal RAM budget" in docs/ARCHITECTURE.md):
//   1. The task-create failure path (ESP_ERR_HTTPD_TASK) is the one path inside
//      httpd_start that logs nothing.
//   2. Through IDF v5.5.4 that same path LEAKS the bound port-80 listen
//      socket, so every later attempt fails `listen (112)` EADDRINUSE until
//      reboot. v5.5.5 closes the sockets there; the gate stays because the
//      firmware still builds on older v5.5.x.
// Therefore: GATE the attempt on the largest free internal block and retry the
// gate on a one-shot timer — never enter httpd_start unless the stack
// allocation can succeed. Without a retry the web UI is dead until reboot
// (wifi_manager's wifi_task has exited by then).
// 8 KB, cut from 16 KB on 2026-09-06 against the measurement this file was
// already taking: uxTaskGetStackHighWaterMark at the end of the SD download
// handler (the deepest path the ledger says the stack is sized for) reported
// 12,352 B of 16,384 STILL UNUSED, i.e. a 4,032 B peak. 8 KB keeps ~2x margin
// on the measured peak and halves the contiguous internal block this task
// needs. Re-read `status` ("stack headroom") after any new deep handler.
#define HTTPD_STACK_SIZE      8192

size_t web_server_httpd_stack_size(void) { return HTTPD_STACK_SIZE; }
#define HTTPD_START_MARGIN    4096            // server structs + fragmentation slack
#define HTTPD_START_RETRIES   5
#define HTTPD_RETRY_DELAY_US  (3 * 1000 * 1000ULL)
static esp_timer_handle_t s_httpd_retry_tmr = NULL;
static int s_httpd_attempts = 0;

// TRIED AND REVERTED 2026-07-27 — reserving HTTPD_STACK_SIZE+MARGIN at the top
// of app_main (to guarantee a contiguous block survived to httpd_start) BOOT
// LOOPED the board. Internal free at wifi-up is only ~19-27 KB total, so
// holding 20 KB from the start starves the allocations boot itself needs and
// something ESP_ERROR_CHECKs into a panic. There is no 20 KB of slack to
// reserve: with TDM active, httpd's stack genuinely does not fit any more.
// Do not retry this without first reclaiming internal RAM elsewhere.
static void httpd_retry_cb(void *arg) { (void)arg; web_server_start(); }

// Count the attempt and arm the retry timer (or give up after the budget).
static void httpd_retry_later(const char *why)
{
    s_httpd_attempts++;
    ESP_LOGE(TAG, "httpd start attempt %d/%d: %s (internal free=%u largest=%u)",
             s_httpd_attempts, HTTPD_START_RETRIES, why,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (s_httpd_attempts >= HTTPD_START_RETRIES) {
        ESP_LOGE(TAG, "HTTP server failed %d attempts — web UI unavailable until reboot",
                 HTTPD_START_RETRIES);
        return;
    }
    if (!s_httpd_retry_tmr) {
        const esp_timer_create_args_t args = {
            .callback = httpd_retry_cb,
            .name     = "httpd_retry",
        };
        if (esp_timer_create(&args, &s_httpd_retry_tmr) != ESP_OK) {
            ESP_LOGE(TAG, "httpd retry timer create failed — web UI unavailable");
            return;
        }
    }
    esp_timer_start_once(s_httpd_retry_tmr, HTTPD_RETRY_DELAY_US);
}

void web_server_start(void)
{
    if (s_srv) return;   // already running (resume after a late retry, etc.)

    // (The "wifi-up" heap checkpoint lives in wifi_manager.c, at the actual
    // esp_wifi_start() — it used to be here, which logged it once per httpd
    // retry and made the boot log claim five WiFi bring-ups.)

    // The gate (fact 2 above): entering httpd_start without room for its stack
    // poisons port 80 for the rest of the boot.
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < HTTPD_STACK_SIZE + HTTPD_START_MARGIN) {
        httpd_retry_later("gated: largest internal block too small for httpd stack");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size          = HTTPD_STACK_SIZE;
    cfg.core_id             = 0;      // keep off core 1 (reserved for audio)
    cfg.recv_wait_timeout   = 300;
    cfg.send_wait_timeout   = 300;
    cfg.max_open_sockets    = 4;
    // Sized from the table itself so a new URI can never silently fail to
    // register (the drift-guard philosophy — see the CDC FIFO _Static_assert).
    cfg.max_uri_handlers    = (uint16_t)(sizeof(s_uris) / sizeof(s_uris[0]));
    cfg.uri_match_fn        = httpd_uri_match_wildcard;
    cfg.lru_purge_enable    = true;   // required for async handler (/api/rec/live)

    esp_err_t err = httpd_start(&s_srv, &cfg);
    if (err != ESP_OK) {
        httpd_retry_later(esp_err_to_name(err));
        return;
    }
    s_httpd_attempts = 0;
    for (int i = 0; i < (int)(sizeof(s_uris) / sizeof(s_uris[0])); i++) {
        esp_err_t rerr = httpd_register_uri_handler(s_srv, &s_uris[i]);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "URI register failed for %s: %s",
                     s_uris[i].uri, esp_err_to_name(rerr));
        }
    }
    s_started = true;   // sticky "httpd came up" — see web_server_started()
    heap_checkpoint("httpd-up");
    {
        bool dis, is_ap;
        char ip[24];
        wifi_manager_get_wifi_status(&dis, &is_ap, ip, sizeof(ip));
        ESP_LOGI(TAG, "HTTP server ready — Web UI: http://%s", ip);
    }
}

// ==========================================================
// Stop (light-sleep suspend, `wifi off`)
// ==========================================================

// Retry timer first: a pending httpd_retry_cb firing after the caller tears
// WiFi down would call httpd_start on a dead stack (wifi_manager stops its
// reconnect timer for the same reason before calling here).
void web_server_stop(void)
{
    if (s_httpd_retry_tmr) esp_timer_stop(s_httpd_retry_tmr);
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
    s_httpd_attempts = 0;   // fresh retry budget for the next start —
                            // boot-time exhaustion doesn't carry over to resume
}
