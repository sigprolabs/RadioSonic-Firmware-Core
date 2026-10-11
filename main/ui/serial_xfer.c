// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// USB serial file transfer ("xfer" console command) — see serial_xfer.h and the
// wire spec in docs/SERIAL_XFER.md.
//
// Transport facts that shape this code:
//   * The console is an esp_console REPL over TinyUSB CDC0. During a command
//     handler, linenoise is NOT reading stdin, so post-command bytes the host
//     sends sit in the CDC RX FIFO — we drain them directly with
//     tinyusb_cdcacm_read() (driver-level, no line-ending mangling, with an
//     idle timeout so a dead host can't wedge the REPL).
//   * ALL bulk output (base64 "D:" lines, raw payloads, tail CRC/END lines)
//     goes through the flow-controlled xfer_write_all — never printf, whose
//     console path is allowed to drop under TX pressure (that bug shipped
//     twice; lossless since 2026-07-06). base64 keeps `get` ASCII-safe and
//     the "D:" tag lets the host ignore stray ESP_LOG lines.
//   * Integrity is standard CRC-32 (zlib) so the host can use zlib.crc32 / a
//     stock table impl with no surprises.

#include "serial_xfer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb_cdc_acm.h"          // console + raw binary transfer share CDC0

#include "audio_store.h"
#include "wav_util.h"                 // wav_header_t (sizes the live-stream header)
#include "sd_storage.h"               // MOUNT_POINT — one owner for the SD path
#include "common.h"

static const char *TAG = "xfer";

#define XFER_MOUNT       MOUNT_POINT
#define XFER_CHUNK       3072u                          // raw bytes per D: line
#define XFER_B64CAP      (4u * ((XFER_CHUNK + 2u) / 3u) + 1u)  // 4097
#define XFER_RX_IDLE_MS  30000                          // abort upload if host goes silent

// Raw-binary CDC0 writer (defined in the fast-transfer section below); used by the
// live stream as well as the `xfer speed` probe.
static size_t xfer_write_all(const uint8_t *p, size_t n, uint32_t *crc);

// ---------------------------------------------------------------------------
// CRC-32 (zlib): poly 0xEDB88320, init/xorout 0xFFFFFFFF. crc32_update(0, ...)
// chained gives the same result as Python zlib.crc32(data, prev).
// ---------------------------------------------------------------------------

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// base64 (standard alphabet, with '=' padding)
// ---------------------------------------------------------------------------

static const char B64E[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// out must be >= 4*ceil(n/3)+1. Returns NUL-terminated length.
static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = B64E[(v >> 18) & 63]; out[o++] = B64E[(v >> 12) & 63];
        out[o++] = B64E[(v >> 6) & 63];  out[o++] = B64E[v & 63];
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64E[(v >> 18) & 63]; out[o++] = B64E[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[o++] = B64E[(v >> 18) & 63]; out[o++] = B64E[(v >> 12) & 63];
        out[o++] = B64E[(v >> 6) & 63];  out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decode slen chars of s into out (cap bytes). Skips non-alphabet chars, stops
// at '='. Returns bytes decoded, or -1 on overflow.
static int b64_decode(const char *s, size_t slen, uint8_t *out, size_t cap)
{
    size_t o = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        char c = s[i];
        if (c == '=') break;
        int v = b64_val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (int)o;
}

// ---------------------------------------------------------------------------
// Download / live TX — base64 "D:" line sink
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  raw[XFER_CHUNK];
    size_t   fill;
    char    *line;     // >= XFER_B64CAP
    uint32_t crc;      // running zlib CRC (only when do_crc)
    bool     do_crc;
    bool     stalled;  // host stopped draining — abort, don't grind out the file
} b64out_t;

static void b64out_emit(b64out_t *o)
{
    if (o->fill == 0 || o->stalled) return;
    if (o->do_crc) o->crc = crc32_update(o->crc, o->raw, o->fill);
    size_t ln = b64_encode(o->raw, o->fill, o->line);
    // Flow-controlled writer, NOT printf: D: lines used to ride the console's
    // drop-under-pressure path, losing wire bytes whenever the host drained
    // slower than the encoder produced (~60% loss on a ~350 KB/s host — Dan's
    // repro, 2026-07-06). CRC caught it, but transfers could never complete.
    // A short write means the host stopped draining entirely (stall timeout in
    // xfer_write_all) — latch it so the caller aborts instead of spending the
    // stall timeout again on every remaining chunk of the file.
    if (xfer_write_all((const uint8_t *)"D:", 2, NULL) < 2 ||
        xfer_write_all((const uint8_t *)o->line, ln, NULL) < ln ||
        xfer_write_all((const uint8_t *)"\n", 1, NULL) < 1) {
        o->stalled = true;
    }
    o->fill = 0;
}

static void b64out_push(b64out_t *o, const uint8_t *data, size_t len)
{
    while (len) {
        size_t take = XFER_CHUNK - o->fill;
        if (take > len) take = len;
        memcpy(o->raw + o->fill, data, take);
        o->fill += take; data += take; len -= take;
        if (o->fill == XFER_CHUNK) b64out_emit(o);
        if (o->stalled) return;
    }
}

// Callback adapter for audio_store_flash_write_wav. Propagates a TX stall as
// an error so the partition read loop aborts instead of streaming into a void.
static esp_err_t flash_b64_cb(const void *data, size_t len, void *ctx)
{
    b64out_t *o = (b64out_t *)ctx;
    b64out_push(o, (const uint8_t *)data, len);
    return o->stalled ? ESP_FAIL : ESP_OK;
}

// ---------------------------------------------------------------------------
// Log mute/restore for binary transfers
// ---------------------------------------------------------------------------
// Restores the PRIOR level rather than hardcoding INFO, so a transfer can't
// permanently clobber a verbosity the user set.
static esp_log_level_t s_saved_log_level = ESP_LOG_INFO;

static void xfer_logs_mute(void)
{
    s_saved_log_level = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
}

static void xfer_logs_restore(void)
{
    esp_log_level_set("*", s_saved_log_level);
}

// ---------------------------------------------------------------------------
// Filename validation
// ---------------------------------------------------------------------------
// Guarantee: the returned name contains no path separators, no leading dot,
// and no quotes/control characters — files live flat in the SD root, so
// anything path-like is refused outright rather than stripped (a visibly
// complete rule; same policy as the web upload/download handlers). Legal
// names are documented in docs/SERIAL_XFER.md.
static const char *sanitize(const char *in, char *out, size_t cap)
{
    if (!in || !in[0] || in[0] == '.') return NULL;
    for (const char *p = in; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || (unsigned char)*p < 0x20)
            return NULL;
    }
    strncpy(out, in, cap - 1);
    out[cap - 1] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

static int xfer_list(void)
{
    if (!g_state.sd_card) { printf("XFER ERR=nocard\n"); return 1; }
    DIR *d = opendir(XFER_MOUNT);
    if (!d) { printf("XFER ERR=opendir\n"); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[300];
        snprintf(p, sizeof(p), XFER_MOUNT "/%s", e->d_name);
        struct stat st;
        long sz = (stat(p, &st) == 0) ? (long)st.st_size : -1;
        printf("F:%s %ld\n", e->d_name, sz);
    }
    closedir(d);
    printf("XFER END\n");
    return 0;
}

static b64out_t *b64out_new(bool do_crc)
{
    b64out_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->line = malloc(XFER_B64CAP);
    if (!o->line) { free(o); return NULL; }
    o->do_crc = do_crc;
    o->crc = 0;
    return o;
}

static void b64out_free(b64out_t *o)
{
    if (o) { free(o->line); free(o); }
}

// Shared open preamble for the SD download verbs: card check, name
// validation, stat, open. Owns the error-line taxonomy (nocard/name/
// notfound/open) — returns NULL with the error already printed.
static FILE *xfer_open_sd_read(const char *name, char *safe, size_t safecap,
                               long *size_out)
{
    if (!g_state.sd_card) { printf("XFER ERR=nocard\n"); return NULL; }
    if (!sanitize(name, safe, safecap)) { printf("XFER ERR=name\n"); return NULL; }
    char path[200];
    snprintf(path, sizeof(path), XFER_MOUNT "/%s", safe);

    struct stat st;
    if (stat(path, &st) != 0) { printf("XFER ERR=notfound\n"); return NULL; }
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("XFER ERR=open\n"); return NULL; }
    if (size_out) *size_out = (long)st.st_size;
    return fp;
}

static int xfer_get_sd(const char *name)
{
    char safe[160];
    long fsize = 0;
    FILE *fp = xfer_open_sd_read(name, safe, sizeof(safe), &fsize);
    if (!fp) return 1;

    b64out_t *o = b64out_new(true);
    uint8_t  *rb = malloc(XFER_CHUNK);
    if (!o || !rb) { free(rb); b64out_free(o); fclose(fp); printf("XFER ERR=oom\n"); return 1; }

    printf("XFER BEGIN name=%s size=%ld\n", safe, fsize);
    fflush(stdout);
    size_t n;
    while ((n = fread(rb, 1, XFER_CHUNK, fp)) > 0) {
        b64out_push(o, rb, n);
        if (o->stalled) break;
    }
    bool rderr = (!o->stalled && ferror(fp));   // mid-file SD read error
    b64out_emit(o);
    // Tail lines also via the lossless writer — the FIFO is saturated right
    // after the stream, exactly when printf drops (the getraw tail bug).
    // A truncated transfer must say so (ERR), never CRC+END-as-if-complete:
    // the host would otherwise read the length mismatch as link corruption.
    char tail[48];
    int tn;
    bool failed = o->stalled || rderr;
    if (failed) {
        tn = snprintf(tail, sizeof(tail), "XFER ERR=%s\n",
                      o->stalled ? "stall" : "read");
    } else {
        tn = snprintf(tail, sizeof(tail), "XFER CRC=%08lX\nXFER END\n",
                      (unsigned long)o->crc);
    }
    xfer_write_all((const uint8_t *)tail, (size_t)tn, NULL);

    free(rb); b64out_free(o); fclose(fp);
    return failed ? 1 : 0;
}

// Raw-binary SD download (device → host). `get` is lossless too (its D: lines
// go through the same flow-controlled writer since 2026-07-06) — getraw's
// advantage is purely throughput: no 33% base64 expansion and no per-line
// framing, so ~900 KB/s payload vs ~515 KB/s for `get`.
// Wire contract (docs/SERIAL_XFER.md):
//   host →  xfer getraw sd:<name>\n
//   dev  ←  XFER RAWBEGIN name=<name> size=<N>\n    (text line, arrives CRLF)
//   dev  ←  <N raw bytes>                           (verbatim file contents)
//   dev  ←  \nXFER CRC=<hex8>\n XFER END\n          (text resync + integrity)
// Logs are muted before the RAWBEGIN line (same rule as xfer_live) so nothing
// can interleave with the payload; framing lines go out while the link is
// otherwise idle, where the console path does not drop.
static int xfer_getraw_sd(const char *name)
{
    char safe[160];
    long fsize = 0;
    FILE *fp = xfer_open_sd_read(name, safe, sizeof(safe), &fsize);
    if (!fp) return 1;

    const size_t CK = 16384;
    uint8_t *rb = malloc(CK);
    if (!rb) { fclose(fp); printf("XFER ERR=oom\n"); return 1; }

    xfer_logs_mute();
    printf("XFER RAWBEGIN name=%s size=%ld\n", safe, fsize);
    fflush(stdout);

    uint32_t crc = 0;
    bool stalled = false;
    size_t n;
    while ((n = fread(rb, 1, CK, fp)) > 0) {
        if (xfer_write_all(rb, n, &crc) < n) { stalled = true; break; }
    }
    bool rderr = (!stalled && ferror(fp));

    fclose(fp);
    free(rb);
    xfer_logs_restore();
    // Trailing lines MUST also go through the lossless writer: right after the
    // payload the TX FIFO is still saturated, which is exactly when the console
    // printf path drops output (hardware-observed: CRC/END vanished, host timed
    // out after a byte-perfect 256 MiB payload). Raw writes skip \n->\r\n
    // translation; hosts must accept bare-\n line endings here.
    char tail[48];
    int tn;
    if (stalled || rderr) {
        tn = snprintf(tail, sizeof(tail), "\nXFER ERR=%s\n", stalled ? "stall" : "read");
        xfer_write_all((const uint8_t *)tail, (size_t)tn, NULL);
        return 1;
    }
    tn = snprintf(tail, sizeof(tail), "\nXFER CRC=%08lX\nXFER END\n", (unsigned long)crc);
    xfer_write_all((const uint8_t *)tail, (size_t)tn, NULL);
    return 0;
}

static int xfer_get_flash(void)
{
    size_t total = audio_store_flash_wav_size();
    if (total == 0) { printf("XFER ERR=nosample\n"); return 1; }

    b64out_t *o = b64out_new(true);
    if (!o) { printf("XFER ERR=oom\n"); return 1; }

    printf("XFER BEGIN name=flash.wav size=%u\n", (unsigned)total);
    fflush(stdout);
    esp_err_t err = audio_store_flash_write_wav(flash_b64_cb, o);
    char tail[48];
    int tn;
    if (err == ESP_OK) {
        b64out_emit(o);
    }
    if (err == ESP_OK && !o->stalled) {
        tn = snprintf(tail, sizeof(tail), "XFER CRC=%08lX\nXFER END\n",
                      (unsigned long)o->crc);
    } else {
        tn = snprintf(tail, sizeof(tail), "XFER ERR=%s\n",
                      o->stalled ? "stall" : "read");
        err = ESP_FAIL;
    }
    // Lossless tail (FIFO is saturated right after the stream; printf drops there).
    xfer_write_all((const uint8_t *)tail, (size_t)tn, NULL);
    b64out_free(o);
    return (err == ESP_OK) ? 0 : 1;
}

// Upload sinks
static esp_err_t sink_sd(const uint8_t *d, size_t n, void *ctx)
{
    return (fwrite(d, 1, n, (FILE *)ctx) == n) ? ESP_OK : ESP_FAIL;
}
static esp_err_t sink_flash(const uint8_t *d, size_t n, void *ctx)
{
    (void)ctx;
    audio_store_record_submit(d, n);   // streams to flash via the recorder ring
    return ESP_OK;
}

typedef esp_err_t (*xfer_sink_fn)(const uint8_t *, size_t, void *);

// Receive base64 "D:" lines from the host until "XFER DONE"; verify size + CRC.
// Returns 0 on success; sets *err to a short reason on failure.
static int xfer_receive(xfer_sink_fn sink, void *ctx,
                        size_t expect_size, uint32_t expect_crc, const char **err)
{
    char    *line = malloc(XFER_B64CAP + 8);
    uint8_t *raw  = malloc(XFER_CHUNK + 8);
    if (!line || !raw) { free(line); free(raw); *err = "oom"; return 1; }

    size_t   li = 0, got = 0;
    uint32_t crc = 0;
    bool     done = false, ok = true;
    *err = "timeout";
    int64_t deadline = esp_timer_get_time() + (int64_t)XFER_RX_IDLE_MS * 1000;

    // Drain the CDC0 RX FIFO directly (console task is inside this command
    // handler, so linenoise is not competing for it — same pattern as `live`).
    // Poll cadence: the CDC RX ring is only 512 B (CONFIG_TINYUSB_CDC_RX_BUFSIZE),
    // so the empty-poll delay caps throughput at ring/delay — keep it at one
    // 1 ms tick (~500 KB/s ceiling), not 10 ms (~50 KB/s).
    uint8_t chunk[128];
    while (ok && !done) {
        size_t rx = 0;
        tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, chunk, sizeof(chunk), &rx);
        if (rx == 0) {
            if (esp_timer_get_time() > deadline) { ok = false; *err = "timeout"; }
            else vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        deadline = esp_timer_get_time() + (int64_t)XFER_RX_IDLE_MS * 1000;

        for (size_t bi = 0; bi < rx && ok && !done; bi++) {
            uint8_t c = chunk[bi];
            if (c == '\r') continue;
            if (c != '\n') {
                if (li < XFER_B64CAP + 7) line[li++] = (char)c;   // overflow → CRC will fail
                continue;
            }
            line[li] = '\0';
            if (li >= 2 && line[0] == 'D' && line[1] == ':') {
                int n = b64_decode(line + 2, li - 2, raw, XFER_CHUNK + 8);
                if (n < 0) { ok = false; *err = "b64"; }
                else {
                    crc = crc32_update(crc, raw, (size_t)n);
                    got += (size_t)n;
                    if (sink(raw, (size_t)n, ctx) != ESP_OK) { ok = false; *err = "write"; }
                }
            } else if (strcmp(line, "XFER DONE") == 0) {
                done = true;
            }
            // any other line: ignore (host/log noise)
            li = 0;
        }
    }

    // On an aborted upload the host may still be mid-stream; drain and discard
    // until the line goes idle so leftover base64 doesn't land in the REPL as
    // phantom "commands". Bounded so a runaway host can't hold the console.
    if (!done) {
        int64_t idle_since = esp_timer_get_time();
        int64_t give_up    = idle_since + 5000000;   // 5 s hard cap
        while (esp_timer_get_time() < give_up) {
            size_t rx = 0;
            tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, chunk, sizeof(chunk), &rx);
            if (rx) idle_since = esp_timer_get_time();
            else if (esp_timer_get_time() - idle_since > 300000) break;  // 300 ms quiet
            else vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    free(line); free(raw);
    if (!ok || !done) return 1;
    if (got != expect_size) { *err = "size"; return 1; }
    if (crc != expect_crc)  { *err = "crc";  return 1; }
    return 0;
}

static int xfer_put_sd(const char *name, size_t size, uint32_t crc)
{
    if (!g_state.sd_card) { printf("XFER ERR=nocard\n"); return 1; }
    char safe[160];
    if (!sanitize(name, safe, sizeof(safe))) { printf("XFER ERR=name\n"); return 1; }
    char path[200];
    snprintf(path, sizeof(path), XFER_MOUNT "/%s", safe);

    FILE *fp = fopen(path, "wb");
    if (!fp) { printf("XFER ERR=open\n"); return 1; }

    printf("XFER READY\n"); fflush(stdout);
    const char *err = "?";
    int rc = xfer_receive(sink_sd, fp, size, crc, &err);
    fclose(fp);
    if (rc != 0) { remove(path); printf("XFER ERR=%s\n", err); return 1; }
    printf("XFER OK\n");
    ESP_LOGI(TAG, "uploaded %u bytes -> %s", (unsigned)size, path);
    return 0;
}

static int xfer_put_flash(size_t size, uint32_t crc)
{
    // EXTERNAL source: the audio hot path must not submit into the ring while
    // the upload feeds it, or live audio interleaves into the sample.
    esp_err_t e = audio_store_record_start(AUDIO_BACKEND_FLASH, NULL, FS_HZ, 32, 2,
                                           AUDIO_REC_SRC_EXTERNAL);
    if (e != ESP_OK) { printf("XFER ERR=busy\n"); return 1; }

    printf("XFER READY\n"); fflush(stdout);
    const char *err = "?";
    int rc = xfer_receive(sink_flash, NULL, size, crc, &err);
    audio_store_record_stop();   // drains the ring + writes flash metadata
    if (rc != 0) { printf("XFER ERR=%s\n", err); return 1; }
    printf("XFER OK\n");
    ESP_LOGI(TAG, "uploaded %u bytes -> flash sample", (unsigned)size);
    return 0;
}

// Live audio stream over CDC0 as RAW BINARY (no base64, NO text preamble). The audio
// source is the audio_store LIVE ring (audio_store_live_read — the same best-effort
// consumer /api/rec/live uses). Transport is the proven raw-CDC writer xfer_write_all
// (~900 KB/s; real-time 48k/stereo/32-bit PCM is 375 KB/s, ~2.4x margin).
//
// Wire contract (deliberately preamble-free to avoid a text/binary boundary race in the
// first USB chunk): after the command echo, the response IS the stream — it starts with
// the 44-byte WAV header (which carries sr/ch/bits) and continues as raw PCM on the SAME
// port. The host flips to binary the moment it sends `xfer live` and reads the format
// from those 44 bytes. Stop = host sends ANY byte (or closes the port → write stalls);
// the device then prints a text `XFER END` + prompt so the host can resync to text mode.
// The only text the device can emit instead of the stream is an error line (`XFER ERR=`),
// so the host keys "streaming" on the `RIFF` magic.
static int xfer_live(const char *taparg)
{
    // Reject a mistyped tap up front — silently streaming the CURRENT tap on
    // 'xfer live psot' would hand the host the wrong signal with no diagnostic.
    if (taparg && strcmp(taparg, "pre") != 0 && strcmp(taparg, "post") != 0) {
        printf("XFER ERR=args\n");
        return 1;
    }

    // Mute logs FIRST — before any call that might ESP_LOG (audio_store_set_record_tap /
    // audio_store_live_open do), so not a single log line can leak ahead of the WAV header
    // and break the pure-binary contract. printf (XFER ERR / XFER END) is stdout, not a log,
    // so it is unaffected by this.
    xfer_logs_mute();

    // CRLF landmine (Dan, 2026-07-06): a host that terminates the command with
    // \r\n leaves the LF in the RX FIFO after the console consumes through the
    // CR — and that orphan byte instantly trips the any-byte-stops abort below.
    // Drain leftover line bytes before arming the stop check.
    {
        uint8_t junk[16]; size_t jn;
        do { jn = 0; tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, junk, sizeof(junk), &jn); } while (jn > 0);
    }

    if (taparg) {
        if      (strcmp(taparg, "post") == 0) audio_store_set_record_tap(AUDIO_TAP_POST);
        else if (strcmp(taparg, "pre")  == 0) audio_store_set_record_tap(AUDIO_TAP_PRE);
    }

    uint8_t *rb = malloc(XFER_CHUNK);
    if (!rb) { xfer_logs_restore(); printf("XFER ERR=oom\n"); return 1; }

    // A failed open must NOT be followed by the WAV header — the spec tells
    // hosts to key "streaming started" on the RIFF magic, so emitting it here
    // would leave them waiting on a stream that never comes.
    if (audio_store_live_open() != ESP_OK) {
        free(rb);
        xfer_logs_restore();
        printf("XFER ERR=busy\n");
        return 1;
    }

    // The stream starts directly with the WAV header (no status line) so the host has a
    // clean binary boundary: command echo, then `RIFF....`.
    uint8_t hdr[sizeof(wav_header_t)];
    audio_store_live_wav_header(hdr);
    xfer_write_all(hdr, sizeof(hdr), NULL);

    for (;;) {
        uint8_t abort_c; size_t rx = 0;
        tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, &abort_c, 1, &rx);
        if (rx > 0) break;                                   // any byte from host stops
        size_t n = audio_store_live_read(rb, XFER_CHUNK, 200);
        if (n && xfer_write_all(rb, n, NULL) < n) break;     // host stopped draining (stall)
    }

    audio_store_live_close();
    xfer_logs_restore();   // restore logging
    printf("\nXFER END\n");                 // text resync marker after the host-requested stop
    // Splice tripwire (issue #12 class): live has no finalize step, so report
    // upstream I2S loss here. AFTER the END marker — hosts key resync on that
    // line and must already tolerate console text following it.
    uint32_t ovf = audio_store_live_overruns();
    if (ovf > 0)
        printf("WARNING: %u I2S RX overrun(s) during live stream — audio has splice gaps\n",
               (unsigned)ovf);
    fflush(stdout);

    free(rb);
    return 0;
}

// ---------------------------------------------------------------------------
// Raw-binary fast path on CDC0 — the SAME port as the console. Safe without
// locking because the console task is parked inside this command handler for
// the whole transfer and logs are muted, so nothing else writes the endpoint.
// (A separate CDC1 channel existed briefly during bring-up; it's gone —
// CDC_COUNT=1 — and everything, framing and payload, rides CDC0 now.)
// ---------------------------------------------------------------------------

// Fast transfer reuses the console CDC (CDC0) in raw-binary mode: during a transfer
// the console loop is parked in esp_console_run (no prompt/linenoise writes) and we
// suppress logs, so CDC0 carries only the raw payload. Binary goes out via direct
// tinyusb_cdcacm_write_queue (NOT printf — that path does \n->\r\n translation).
#define XFER_CDC_BIN     TINYUSB_CDC_ACM_0
#define XFER_TX_STALL_US (5LL * 1000 * 1000)   // give up if host stops draining 5s

// The flow-controlled writer parks one FreeRTOS tick per FIFO-full retry, so the
// TX FIFO size gates throughput (bytes-per-tick). A drifted local sdkconfig once
// shipped 512 B here and throttled live below real-time. Refuse to build with a
// FIFO too small to sustain 375 KB/s at any supported tick rate: sdkconfig is
// untracked, so this assert — not the defaults file — is the enforceable contract.
_Static_assert(CONFIG_TINYUSB_CDC_TX_BUFSIZE >= 4096,
               "CDC TX FIFO too small for xfer/live throughput — sync sdkconfig "
               "with sdkconfig.defaults (CONFIG_TINYUSB_CDC_TX_BUFSIZE=8192)");

// Write all n bytes to CDC0 (queue + non-blocking flush, retry on FIFO-full). The
// write rate is gated by how fast the host drains the IN endpoint, so timing this
// measures real end-to-end throughput. Returns bytes actually sent (< n on stall).
static size_t xfer_write_all(const uint8_t *p, size_t n, uint32_t *crc)
{
    if (crc) *crc = crc32_update(*crc, p, n);
    size_t off = 0;
    int64_t stall_deadline = esp_timer_get_time() + XFER_TX_STALL_US;
    while (off < n) {
        size_t q = tinyusb_cdcacm_write_queue(XFER_CDC_BIN, p + off, n - off);
        if (q == 0) {
            tinyusb_cdcacm_write_flush(XFER_CDC_BIN, 0);   // nudge the EP
            if (esp_timer_get_time() > stall_deadline) break;
            vTaskDelay(1);
            continue;
        }
        off += q;
        stall_deadline = esp_timer_get_time() + XFER_TX_STALL_US;
    }
    tinyusb_cdcacm_write_flush(XFER_CDC_BIN, 0);
    return off;
}

// Raw throughput probe: blast `bytes` of a counter pattern out the console port
// itself (the "raw binary follows on THIS port" line below says so on the
// wire). The host drains the same port it sent the command on; the byte at
// offset i is (i & 0xFF) for a sanity check.
static int xfer_speed(size_t bytes)
{
    const size_t CK = 8192;
    uint8_t *buf = malloc(CK);
    if (!buf) { printf("XFER ERR=oom\n"); return 1; }
    for (size_t i = 0; i < CK; i++) buf[i] = (uint8_t)i;

    printf("XFER SPEED start bytes=%u (raw binary follows on THIS port)\n", (unsigned)bytes);
    fflush(stdout);
    xfer_logs_mute();   // keep logs off the binary stream

    int64_t t0 = esp_timer_get_time();
    size_t sent = 0, stalled = 0;
    while (sent < bytes) {
        size_t chunk = bytes - sent;
        if (chunk > CK) chunk = CK;
        size_t w = xfer_write_all(buf, chunk, NULL);
        sent += w;
        if (w < chunk) { stalled = 1; break; }
    }
    int64_t dt = esp_timer_get_time() - t0;

    xfer_logs_restore();   // restore
    double kbs = (dt > 0) ? ((double)sent / 1024.0) / ((double)dt / 1e6) : 0.0;
    printf("\nXFER SPEED done bytes=%u ms=%lld rate=%.1f KB/s%s\n",
           (unsigned)sent, (long long)(dt / 1000), kbs,
           stalled ? "  (STALLED — host stopped reading)" : "");
    fflush(stdout);
    free(buf);
    return 0;
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static int cmd_xfer(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: xfer <list|get|getraw|put|live|speed> ...\n");
        printf("  xfer list\n");
        printf("  xfer get    <sd:NAME|flash>\n");
        printf("  xfer getraw <sd:NAME>         (raw binary, ~900 KB/s)\n");
        printf("  xfer put    <sd:NAME|flash> <size> <crc32hex>\n");
        printf("  xfer live [pre|post]\n");
        printf("  xfer speed [bytes]            (raw USB throughput probe, default 1 MB)\n");
        return 1;
    }

    const char *op = argv[1];

    if (strcmp(op, "list") == 0) return xfer_list();

    if (strcmp(op, "get") == 0) {
        if (argc < 3) { printf("XFER ERR=args\n"); return 1; }
        const char *dst = argv[2];
        if (strncmp(dst, "sd:", 3) == 0) return xfer_get_sd(dst + 3);
        if (strcmp(dst, "flash") == 0)   return xfer_get_flash();
        printf("XFER ERR=dest\n"); return 1;
    }

    if (strcmp(op, "getraw") == 0) {
        if (argc < 3) { printf("XFER ERR=args\n"); return 1; }
        if (strncmp(argv[2], "sd:", 3) == 0) return xfer_getraw_sd(argv[2] + 3);
        printf("XFER ERR=dest\n"); return 1;
    }

    if (strcmp(op, "put") == 0) {
        if (argc < 5) { printf("XFER ERR=args\n"); return 1; }
        const char *dst = argv[2];
        size_t   size = (size_t)strtoul(argv[3], NULL, 10);
        uint32_t crc  = (uint32_t)strtoul(argv[4], NULL, 16);
        if (strncmp(dst, "sd:", 3) == 0) return xfer_put_sd(dst + 3, size, crc);
        if (strcmp(dst, "flash") == 0)   return xfer_put_flash(size, crc);
        printf("XFER ERR=dest\n"); return 1;
    }

    if (strcmp(op, "live") == 0) return xfer_live(argc >= 3 ? argv[2] : NULL);

    if (strcmp(op, "speed") == 0) {
        size_t bytes = (argc >= 3) ? (size_t)strtoul(argv[2], NULL, 10) : 1048576u;
        return xfer_speed(bytes);
    }

    printf("XFER ERR=op\n");
    return 1;
}

void register_serial_xfer_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "xfer",
        .help     = "USB serial file transfer (Web Serial): list | get <sd:NAME|flash> | "
                    "getraw <sd:NAME> | put <sd:NAME|flash> <size> <crc32hex> | "
                    "live [pre|post] | speed [bytes]",
        .func     = &cmd_xfer,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
