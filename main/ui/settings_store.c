// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "settings_store.h"
#include "common.h"
#include "audio_codec.h"
#include "audio_dsp.h"
#include "dsp_effects.h"
#include "audio_store.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "settings";

#define SETTINGS_NS    "rs_settings"
#define SETTINGS_MAGIC 0x52535332u   // "RSS2" — bump on layout change
                                     // (RSS1 was the pre-framework fixed
                                     // struct; old blobs read as stale)

// One saved snapshot: the framework fields the CORE owns, followed by one
// text section per effect — "[name]\n" + that effect's save_text output.
// Effects serialize THEMSELVES (dsp_effect_t save_text/load_text); the store
// knows nothing about any effect's parameters, so an out-of-tree effect
// persists exactly like a shipped one, and a section whose effect isn't
// registered in this build is skipped with a warning instead of garbling.
// The active effect is stored by NAME — registry indexes shift, names don't.
typedef struct {
    uint32_t magic;
    char     effect[16];        // dsp_effect_t.name
    uint8_t  input_mode;        // input_mode_t
    int16_t  input_gain_db;
    int16_t  output_gain_db;
    uint32_t text_len;          // bytes of section text following (no NUL)
    char     text[];
} settings_blob_t;

// Layout tripwire: any edit that changes the header must land on this line —
// update the expected size AND bump SETTINGS_MAGIC in the same commit.
_Static_assert(sizeof(settings_blob_t) == 32,
               "settings_blob_t layout changed — bump SETTINGS_MAGIC");

// Text budget per snapshot. The largest effect config serializes to ~7 KB; the
// NVS partition is 24 KB, so two full worst-case slots still fit (with the
// WiFi keys) but leave little slack — settings_save reports a failed
// nvs_set_blob cleanly if the partition ever fills.
#define SETTINGS_TEXT_CAP (12 * 1024)

static bool slot_valid(const char *slot)
{
    return slot && (strcmp(slot, SETTINGS_SLOT_USER) == 0 ||
                    strcmp(slot, SETTINGS_SLOT_DEFAULT) == 0);
}

esp_err_t settings_save(const char *slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    // NVS commits erase 4 KB flash sectors — the same both-core cache stall
    // as issue #12. Saving during a capture or live stream would splice the
    // audio, so refuse; the take matters more than the save.
    if (audio_store_capture_active()) {
        ESP_LOGW(TAG, "save refused: capture/live active — a flash erase would "
                      "splice the audio; stop it first");
        return ESP_ERR_INVALID_STATE;
    }

    settings_blob_t *b = heap_caps_malloc(sizeof(*b) + SETTINGS_TEXT_CAP,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) return ESP_ERR_NO_MEM;
    memset(b, 0, sizeof(*b));

    b->magic = SETTINGS_MAGIC;
    strlcpy(b->effect, dsp_current()->name, sizeof(b->effect));
    b->input_mode     = (uint8_t)g_state.input_mode;
    b->input_gain_db  = (int16_t)g_state.input_gain_db;
    b->output_gain_db = (int16_t)g_state.output_gain_db;

    // One "[name]" section per effect that serializes itself. Each hook
    // copies its config under dsp_cfg_lock as needed — the store just
    // concatenates.
    size_t pos = 0;
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *fx = dsp_effect_at(i);
        if (!fx->save_text) continue;
        size_t hdr = pos;
        int n = snprintf(b->text + pos, SETTINGS_TEXT_CAP - pos, "[%s]\n", fx->name);
        if (n < 0 || pos + n >= SETTINGS_TEXT_CAP) { pos = hdr; break; }
        pos += n;
        n = fx->save_text(b->text + pos, SETTINGS_TEXT_CAP - pos);
        if (n < 0) {
            ESP_LOGW(TAG, "'%s' config too large — section dropped", fx->name);
            pos = hdr;
            continue;
        }
        pos += n;
        if (pos > 0 && b->text[pos - 1] != '\n' && pos < SETTINGS_TEXT_CAP - 1) {
            b->text[pos++] = '\n';
        }
    }
    b->text_len = (uint32_t)pos;
    size_t size = sizeof(*b) + pos;

    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, slot, b, size);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    free(b);

    if (err == ESP_OK) ESP_LOGI(TAG, "saved '%s' (%u bytes)", slot, (unsigned)size);
    else               ESP_LOGE(TAG, "save '%s' failed: %s", slot, esp_err_to_name(err));
    return err;
}

// Read a slot into a malloc'd blob (caller frees). Validates magic + size and
// guarantees the text is NUL-terminated.
static esp_err_t settings_read(const char *slot, settings_blob_t **out)
{
    *out = NULL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;   // namespace absent = nothing ever saved

    size_t size = 0;
    err = nvs_get_blob(h, slot, NULL, &size);
    if (err != ESP_OK || size < sizeof(settings_blob_t) ||
        size > sizeof(settings_blob_t) + SETTINGS_TEXT_CAP) {
        nvs_close(h);
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }

    settings_blob_t *b = heap_caps_malloc(size + 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { nvs_close(h); return ESP_ERR_NO_MEM; }
    err = nvs_get_blob(h, slot, b, &size);
    nvs_close(h);

    if (err == ESP_OK &&
        (b->magic != SETTINGS_MAGIC ||
         size != sizeof(settings_blob_t) + b->text_len)) {
        err = ESP_ERR_INVALID_CRC;   // stale layout — treat as unreadable
    }
    if (err != ESP_OK) { free(b); return err; }
    b->text[b->text_len] = '\0';     // the +1 above guarantees room
    *out = b;
    return ESP_OK;
}

// Walk "[name]\n<body>" sections and hand each body to that effect's
// load_text hook. The blob is a trust boundary, but the store's own exposure
// is only the section framing — parameter validation belongs to each hook.
static void settings_apply_sections(char *text)
{
    char *p = text;
    char  cur[16] = { 0 };
    char *body = NULL;

    for (;;) {
        bool at_end   = (*p == '\0');
        bool at_hdr   = (!at_end && *p == '[' && (p == text || p[-1] == '\n'));
        if (at_end || at_hdr) {
            if (cur[0] && body) {              // dispatch the finished section
                char saved = *p;
                *p = '\0';
                int idx = dsp_find(cur);
                const dsp_effect_t *fx = (idx >= 0) ? dsp_effect_at(idx) : NULL;
                if (fx && fx->load_text) {
                    if (!fx->load_text(body)) {
                        ESP_LOGW(TAG, "'%s' section rejected", cur);
                    }
                } else {
                    ESP_LOGW(TAG, "saved section '%s' has no effect in this "
                                  "build — skipped", cur);
                }
                *p = saved;
            }
            cur[0] = '\0';
            body   = NULL;
            if (at_end) break;

            char *e  = strchr(p, ']');
            char *nl = strchr(p, '\n');
            if (e && (!nl || e < nl)) {
                size_t n = (size_t)(e - (p + 1));
                if (n >= sizeof(cur)) n = sizeof(cur) - 1;
                memcpy(cur, p + 1, n);
                cur[n] = '\0';
                body = nl ? nl + 1 : e + 1;
            }
            p = nl ? nl + 1 : p + 1;
            continue;
        }
        p++;
    }
}

static void settings_apply(settings_blob_t *b)
{
    // Codec: input routing + both gains (clamped to the AIC3204 ranges —
    // the blob is a trust boundary).
    // Anything but mic is line in — including a saved 1 (the removed
    // single-ended mode), so older slots still load.
    g_state.input_mode     = (b->input_mode == INPUT_MIC) ? INPUT_MIC : INPUT_LINE_DIFF;
    int ig = b->input_gain_db, og = b->output_gain_db;
    g_state.input_gain_db  = (ig < CODEC_INPUT_GAIN_MIN_DB)  ? CODEC_INPUT_GAIN_MIN_DB
                           : (ig > CODEC_INPUT_GAIN_MAX_DB)  ? CODEC_INPUT_GAIN_MAX_DB  : ig;
    g_state.output_gain_db = (og < CODEC_OUTPUT_GAIN_MIN_DB) ? CODEC_OUTPUT_GAIN_MIN_DB
                           : (og > CODEC_OUTPUT_GAIN_MAX_DB) ? CODEC_OUTPUT_GAIN_MAX_DB : og;
    codec_apply_settings();

    // Per-effect configs, each through its own load_text hook.
    settings_apply_sections(b->text);

    int idx = dsp_find(b->effect);
    if (idx >= 0) dsp_set(idx);
    else ESP_LOGW(TAG, "saved effect '%s' no longer exists — keeping current", b->effect);
    // No call-back into control_buttons here (that was an include cycle): the
    // button task lazily re-syncs its cursor when it notices an external
    // effect change (resync_if_external_change in control_buttons.c).
}

esp_err_t settings_load(const char *slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    settings_blob_t *b;
    esp_err_t err = settings_read(slot, &b);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load '%s': %s", slot, esp_err_to_name(err));
        return err;
    }
    settings_apply(b);
    ESP_LOGI(TAG, "loaded '%s': %s, %s in %d dB, out %d dB, %u B effect config",
             slot, b->effect,
             (b->input_mode == INPUT_MIC) ? "mic" : "line",
             (int)b->input_gain_db, (int)b->output_gain_db,
             (unsigned)b->text_len);
    free(b);
    return ESP_OK;
}

// ==========================================================
// Boot-time slot-width override (TDM)
// ==========================================================
// Standalone NVS u8 key — NOT part of settings_blob_t, because it must be
// readable BEFORE codec/DSP init (app_main geometry selection) and must
// survive blob-layout bumps. 0 = auto (32-bit up to 2 codecs; 16-bit at 3,
// forced by the S3's 128-bit TDM frame cap).
#define SLOT_WIDTH_KEY "slot_w"

uint8_t settings_get_slot_width_early(void)
{
    // Idempotent NVS bring-up (same dance as settings_store_init — whichever
    // runs first does the real init).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return 0;

    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(h, SLOT_WIDTH_KEY, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return (v == 16 || v == 32) ? v : 0;
}

static esp_err_t slot_width_write(uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, SLOT_WIDTH_KEY, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_store_init(void)
{
    // NVS may not be up yet (web_server_init runs later and does the same
    // dance) — both "already initialized" and first-init are fine.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    // Diagnostic builds may have adopted a running codec (no init) to keep a
    // live fault intact — applying a saved default would re-route it.
    if (codec_was_adopted()) {
        ESP_LOGW(TAG, "codec adopted (diag build) — saved default NOT applied");
        return ESP_OK;
    }
    // A missing, stale, or unreadable default slot must never block boot —
    // settings_load already logged the reason; compiled-in settings apply.
    if (settings_load(SETTINGS_SLOT_DEFAULT) != ESP_OK) {
        ESP_LOGI(TAG, "no saved default — using compiled-in settings");
    }
    return ESP_OK;
}

// ==========================================================
// `settings` console command
// ==========================================================
static void show_slot(const char *slot)
{
    settings_blob_t *b;
    if (settings_read(slot, &b) != ESP_OK) {
        printf("  %-8s (empty)\n", slot);
        return;
    }
    int idx = dsp_find(b->effect);
    printf("  %-8s %s, %s, gain in %d dB / out %d dB, sections:",
           slot,
           (idx >= 0) ? dsp_effect_at(idx)->label : b->effect,
           (b->input_mode == INPUT_MIC) ? "mic" : "line (diff)",
           (int)b->input_gain_db, (int)b->output_gain_db);
    // List the "[name]" sections so a glance shows what the slot carries.
    bool any = false;
    for (char *p = b->text; *p; ) {
        if (*p == '[' && (p == b->text || p[-1] == '\n')) {
            char *e = strchr(p, ']');
            if (e) { printf(" %.*s", (int)(e - p - 1), p + 1); any = true; }
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    printf("%s\n", any ? "" : " (none)");
    free(b);
}

// NVS commits erase flash sectors — the same both-core cache stall that
// splices an active capture (see settings_save). One shared refusal string
// for every verb that would write flash.
static const char *s_refuse_capture =
    "Refused: recording/live capture active — flash writes would splice the audio. Stop it first.\n";

static int cmd_settings(int argc, char **argv)
{
    const char *usage =
        "Usage: settings save     — save current settings (btn5 press)\n"
        "       settings load     — load saved settings (shift+btn5)\n"
        "       settings default  — save current as power-on default (btn5 hold)\n"
        "       settings show     — show both saved slots\n"
        "       settings clear    — erase both slots (compiled defaults on next boot)\n"
        "       settings slotwidth <auto|16|32> — TDM slot width (effective after reboot)\n";
    if (argc < 2) { printf("%s", usage); return 1; }

    if (strcmp(argv[1], "slotwidth") == 0) {
        uint8_t cur = settings_get_slot_width_early();
        if (argc < 3) {
            printf("slotwidth = %s (active geometry: %d-bit%s)\n",
                   cur == 0 ? "auto" : (cur == 16 ? "16" : "32"),
                   g_geom.slot_bits, g_geom.tdm ? " TDM" : " std I2S");
            return 0;
        }
        uint8_t v;
        if      (strcmp(argv[2], "auto") == 0) v = 0;
        else if (strcmp(argv[2], "16")   == 0) v = 16;
        else if (strcmp(argv[2], "32")   == 0) v = 32;
        else { printf("Usage: settings slotwidth <auto|16|32>\n"); return 1; }
        if (audio_store_capture_active()) {   // NVS commit erases flash — splice hazard
            printf("%s", s_refuse_capture);
            return 1;
        }
        if (slot_width_write(v) != ESP_OK) { printf("NVS write failed\n"); return 1; }
        printf("slotwidth = %s — takes effect after reboot%s\n",
               v == 0 ? "auto" : (v == 16 ? "16" : "32"),
               v == 32 ? " (3 codecs still force 16 — S3 TDM frame cap)" : "");
        return 0;
    }

    if (strcmp(argv[1], "save") == 0) {
        esp_err_t err = settings_save(SETTINGS_SLOT_USER);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("%s", s_refuse_capture);
            return 1;
        }
        if (err != ESP_OK) return 1;
        printf("Settings saved.\n");
    } else if (strcmp(argv[1], "load") == 0) {
        esp_err_t err = settings_load(SETTINGS_SLOT_USER);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = settings_load(SETTINGS_SLOT_DEFAULT);   // same fallback as shift+btn5
        if (err == ESP_ERR_NVS_NOT_FOUND) { printf("Nothing saved yet.\n"); return 1; }
        if (err != ESP_OK) return 1;
        printf("Settings loaded.\n");
    } else if (strcmp(argv[1], "default") == 0) {
        esp_err_t err = settings_save(SETTINGS_SLOT_DEFAULT);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("%s", s_refuse_capture);
            return 1;
        }
        if (err != ESP_OK) return 1;
        printf("Saved as power-on default.\n");
    } else if (strcmp(argv[1], "show") == 0) {
        printf("--- Saved Settings ---\n");
        show_slot(SETTINGS_SLOT_USER);
        show_slot(SETTINGS_SLOT_DEFAULT);
    } else if (strcmp(argv[1], "clear") == 0) {
        if (audio_store_capture_active()) {   // erases flash — same splice hazard as save
            printf("%s", s_refuse_capture);
            return 1;
        }
        nvs_handle_t h;
        if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
            // Erase only the two snapshot slots — NOT the whole namespace:
            // the TDM slot-width key lives here too and is contractually
            // independent of the snapshots (it must survive blob-layout
            // bumps and, per the help text, `clear` as well).
            nvs_erase_key(h, SETTINGS_SLOT_USER);      // may be absent — fine
            nvs_erase_key(h, SETTINGS_SLOT_DEFAULT);
            nvs_commit(h);
            nvs_close(h);
        }
        printf("Saved settings cleared.\n");
    } else {
        printf("%s", usage);
        return 1;
    }
    return 0;
}

void register_settings_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "settings",
        .help    = "Save/load settings: settings save | load | default | show | clear | slotwidth <auto|16|32>",
        .func    = &cmd_settings,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
