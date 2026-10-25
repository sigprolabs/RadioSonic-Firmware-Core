// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// ============================================================================
// Channel — multi-tap delay / reverb (the in-tree worked example)
// ============================================================================
// "Channel" means a set of delay-and-weight paths per stereo side (L and R
// are configured independently), summed from a shared 1.5 s stereo delay
// ring in PSRAM. This effect demonstrates every optional hook except alias and leds;
// effects in components/rs_student_fx/ follow the same shape.
//
// It is wired the same way as a sandbox effect. This file exports its record,
// g_fx_channel, and an init function, channel_fx_init(), both declared in
// channel_fx.h. The core's dsp_effects_init() registers it with the same
// calls a sandbox effect uses in student_fx.c:
//
//   channel_fx_init();
//   dsp_effects_register(&g_fx_channel);
//
// dsp_effects_init() runs those two lines before rs_user_effects_init(), so
// channel is always index 1, right after bypass. docs/ADDING_EFFECTS.md
// walks through this file hook by hook.

#include "dsp_effects.h"
#include "common.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "channel_fx.h"

static const char *TAG = "channel_fx";

#define CHANNEL_MAX_PATHS 10
#define DELAY_BUF_SEC     1.5f
#define DELAY_SAMPLES     ((int)(DELAY_BUF_SEC * FS_HZ))
#define CH_DELAY_MS_MAX   1000.0f

typedef struct {
    float delay_sec;  // 0.0 – 1.0 s (clamped at set time)
    float weight;     // -1.0 – +1.0
} channel_path_t;

typedef struct {
    uint8_t        num_paths;   // active path count (DSP loop bound)
    channel_path_t paths[CHANNEL_MAX_PATHS];
} channel_cfg_t;

// Staged config (written under dsp_cfg_lock by cmd/load_text/pot_set) and the
// DSP-side snapshot the hot path reads (copied by the snapshot hook).
// Boot default: reverb-like multi-tap (dry + 3 reflections). Path 0 with
// delay_sec=0.0 + weight=1.0 is the dry (direct) signal; the remaining paths
// are silent and freely configurable at runtime.
static channel_cfg_t s_ch_cfg_L = { .num_paths = 4, .paths = {
    { 0.000f, 1.00f },  // dry
    { 0.045f, 0.50f },  // first reflection
    { 0.120f, 0.40f },
    { 0.200f, 0.30f },
}};
static channel_cfg_t s_ch_cfg_R = { .num_paths = 4, .paths = {
    { 0.000f, 1.00f },  // dry
    { 0.048f, 0.50f },  // first reflection (slightly offset from L)
    { 0.128f, 0.40f },
    { 0.215f, 0.30f },
}};
static channel_cfg_t s_chL, s_chR;

// Per-pair mutable state: the PSRAM delay ring. Pair 0 mirrors the pre-TDM
// layout; expansion pairs are allocated at init only when present. A NULL
// buf (failed PSRAM alloc) makes the effect pass through on that pair.
typedef struct {
    float *delay_buf;   // stereo interleaved [L0,R0,L1,R1,...] ring, PSRAM
    int    delay_head;
} channel_pair_t;
static channel_pair_t s_ch_pair[AUDIO_MAX_PAIRS];

// Pot binding config (tap index + sweep range). Deliberately NOT persisted —
// same policy as the pot target itself (settings_store.h).
static int   s_ch_pot_tap   = 1;      // tap 0 is the dry path; sweep tap 1
static float s_ch_pot_lo_ms = 0.0f;
static float s_ch_pot_hi_ms = 250.0f;

static channel_cfg_t *channel_by_char(char ch)
{
    return (ch == 'R' || ch == 'r') ? &s_ch_cfg_R : &s_ch_cfg_L;
}

// Atomic config writers (validate-then-publish; shared by cmd, load_text and
// pot_set). Out-of-range values are clamped and NaN is ignored; the callers
// already reject malformed input where a message helps.
static void channel_set_path(char ch, int idx, float delay_sec, float weight)
{
    channel_cfg_t *cfg = channel_by_char(ch);
    if (idx < 0 || idx >= CHANNEL_MAX_PATHS) return;
    // Every comparison with NaN is false, so the clamps below would store it,
    // and process() would turn a NaN delay into an undefined read index.
    // sscanf accepts "nan" in a load_text line.
    if (isnan(delay_sec) || isnan(weight)) return;
    if (delay_sec < 0.0f) delay_sec = 0.0f;
    if (delay_sec > CH_DELAY_MS_MAX / 1000.0f) delay_sec = CH_DELAY_MS_MAX / 1000.0f;
    if (weight < -1.0f) weight = -1.0f;
    if (weight >  1.0f) weight =  1.0f;
    dsp_cfg_lock();
    cfg->paths[idx].delay_sec = delay_sec;
    cfg->paths[idx].weight    = weight;
    if ((uint8_t)(idx + 1) > cfg->num_paths) cfg->num_paths = (uint8_t)(idx + 1);
    dsp_cfg_unlock();
}

static void channel_set_count(char ch, int n)
{
    channel_cfg_t *cfg = channel_by_char(ch);
    if (n < 0 || n > CHANNEL_MAX_PATHS) return;
    dsp_cfg_lock();
    cfg->num_paths = (uint8_t)n;
    dsp_cfg_unlock();
}

static void channel_clear(char ch)
{
    channel_cfg_t *cfg = channel_by_char(ch);
    dsp_cfg_lock();
    memset(cfg->paths, 0, sizeof(cfg->paths));
    cfg->paths[0].delay_sec = 0.0f;
    cfg->paths[0].weight    = 1.0f;
    cfg->num_paths = 1;
    dsp_cfg_unlock();
}

// ---- hot path ----
static inline void ch_delayed_sample(channel_pair_t *st, int offset_samples,
                                     float *l_out, float *r_out)
{
    int read_idx = st->delay_head - offset_samples;
    if (read_idx < 0) read_idx += DELAY_SAMPLES;
    *l_out = st->delay_buf[read_idx * 2];
    *r_out = st->delay_buf[read_idx * 2 + 1];
}

static void fx_channel(float xL, float xR, float *yL, float *yR)
{
    channel_pair_t *st = &s_ch_pair[dsp_current_pair()];
    if (!st->delay_buf) {          // PSRAM alloc failed at init — pass through
        *yL = xL; *yR = xR;
        return;
    }

    // Write the current sample first so a path with delay_sec=0.0 reads the dry signal.
    st->delay_buf[st->delay_head * 2]     = xL;
    st->delay_buf[st->delay_head * 2 + 1] = xR;

    float tmpL, tmpR;

    float aL = 0.0f;
    for (uint8_t i = 0; i < s_chL.num_paths; i++) {
        int ds = (int)(s_chL.paths[i].delay_sec * FS_HZ);
        if (ds >= DELAY_SAMPLES) ds = DELAY_SAMPLES - 1;
        ch_delayed_sample(st, ds, &tmpL, &tmpR);
        aL += s_chL.paths[i].weight * tmpL;
    }

    float aR = 0.0f;
    for (uint8_t i = 0; i < s_chR.num_paths; i++) {
        int ds = (int)(s_chR.paths[i].delay_sec * FS_HZ);
        if (ds >= DELAY_SAMPLES) ds = DELAY_SAMPLES - 1;
        ch_delayed_sample(st, ds, &tmpL, &tmpR);
        aR += s_chR.paths[i].weight * tmpR;
    }

    if (++st->delay_head >= DELAY_SAMPLES) st->delay_head = 0;

    *yL = aL;
    *yR = aR;
}

static void fx_channel_reset(void)
{
    for (int p = 0; p < AUDIO_MAX_PAIRS; p++) {
        channel_pair_t *st = &s_ch_pair[p];
        if (st->delay_buf) memset(st->delay_buf, 0, DELAY_SAMPLES * 2 * sizeof(float));
        st->delay_head = 0;
    }
}

static void fx_channel_snapshot(void)
{
    dsp_cfg_lock();
    s_chL = s_ch_cfg_L;
    s_chR = s_ch_cfg_R;
    dsp_cfg_unlock();
}

static void fx_channel_print(void)
{
    for (int i = 0; i < CHANNEL_MAX_PATHS; i++) {
        const channel_path_t *pL = &s_ch_cfg_L.paths[i];
        const channel_path_t *pR = &s_ch_cfg_R.paths[i];
        if (pL->weight != 0.0f || pR->weight != 0.0f) {
            printf("  Path%d  L: %.3fs w=%.2f   R: %.3fs w=%.2f\n",
                   i, pL->delay_sec, pL->weight, pR->delay_sec, pR->weight);
        }
    }
}

// ---- console command ----
// channel L|R <idx> <delay_ms> <weight>  |  L|R -n <count>  |  L|R -x
// channel pot [tap [lo_ms hi_ms]]        — the knob's tap + sweep range
// Manual argv parsing — argtable3 misreads negative weights as option flags.
static int fx_channel_cmd(int argc, char **argv)
{
    const char *usage =
        "Usage: channel L|R <idx> <delay_ms> <weight>  |  -n <count>  |  -x\n"
        "       channel pot [tap [lo_ms hi_ms]]\n";
    if (argc < 2) { printf("%s", usage); return 1; }
    char *endp;

    if (strcmp(argv[1], "pot") == 0) {
        if (argc >= 3) {
            long tap = strtol(argv[2], &endp, 10);
            if (*endp || tap < 0 || tap >= CHANNEL_MAX_PATHS) {
                printf("Error: tap must be 0-%d\n", CHANNEL_MAX_PATHS - 1);
                return 1;
            }
            s_ch_pot_tap = (int)tap;
        }
        if (argc >= 5) {
            double lo = strtod(argv[3], &endp);
            if (*endp) { printf("Error: bad lo_ms\n"); return 1; }
            double hi = strtod(argv[4], &endp);
            if (*endp) { printf("Error: bad hi_ms\n"); return 1; }
            // Written so NaN fails it: every comparison with NaN is false.
            if (!(lo >= 0.0 && hi <= CH_DELAY_MS_MAX && lo < hi)) {
                printf("Error: need 0 <= lo_ms < hi_ms <= %.0f\n", (double)CH_DELAY_MS_MAX);
                return 1;
            }
            s_ch_pot_lo_ms = (float)lo;
            s_ch_pot_hi_ms = (float)hi;
        }
        printf("Channel pot: tap %d, %.1f..%.1f ms (select with `pot fx`)\n",
               s_ch_pot_tap, s_ch_pot_lo_ms, s_ch_pot_hi_ms);
        return 0;
    }

    const char *ch = argv[1];
    if (ch[0] != 'L' && ch[0] != 'l' && ch[0] != 'R' && ch[0] != 'r') {
        printf("Error: channel must be L or R\n");
        return 1;
    }
    char CH = (char)(ch[0] & ~0x20);
    channel_cfg_t *cfg = channel_by_char(CH);

    if (argc >= 3 && (strcmp(argv[2], "-x") == 0 || strcmp(argv[2], "--clear") == 0)) {
        channel_clear(CH);
        printf("Channel %c: cleared (dry pass-through)\n", CH);
        return 0;
    }

    if (argc >= 3 && (strcmp(argv[2], "-n") == 0 || strcmp(argv[2], "--count") == 0)) {
        if (argc < 4) { printf("Usage: channel %c -n <count>\n", CH); return 1; }
        long n = strtol(argv[3], &endp, 10);
        if (*endp || n < 0 || n > CHANNEL_MAX_PATHS) {
            printf("Error: count must be 0-%d\n", CHANNEL_MAX_PATHS);
            return 1;
        }
        channel_set_count(CH, (int)n);
        printf("Channel %c: %ld active path(s)\n", CH, n);
        return 0;
    }

    if (argc < 5) { printf("%s", usage); return 1; }

    long idx = strtol(argv[2], &endp, 10);
    if (*endp) { printf("Error: bad idx value\n"); return 1; }
    // Fractional milliseconds are accepted so comb nulls can be placed precisely.
    double delay_ms = strtod(argv[3], &endp);
    if (*endp) { printf("Error: bad delay_ms value\n"); return 1; }
    double weight = strtod(argv[4], &endp);
    if (*endp) { printf("Error: bad weight value\n"); return 1; }

    if (idx < 0 || idx >= CHANNEL_MAX_PATHS) {
        printf("Error: idx must be 0-%d\n", CHANNEL_MAX_PATHS - 1);
        return 1;
    }
    // Range checks written so NaN fails them (strtod accepts "nan").
    if (!(delay_ms >= 0.0 && delay_ms <= CH_DELAY_MS_MAX)) {
        printf("Error: delay_ms must be 0-%.0f\n", (double)CH_DELAY_MS_MAX);
        return 1;
    }
    if (!(weight >= -1.0 && weight <= 1.0)) {
        printf("Error: weight must be -1.0 to +1.0\n");
        return 1;
    }

    channel_set_path(CH, (int)idx, (float)(delay_ms / 1000.0), (float)weight);
    printf("Channel %c[%ld]: %.3fms w=%.3f  (%d active)\n",
           CH, idx, delay_ms, (float)weight, cfg->num_paths);
    return 0;
}

// ---- config serialization ----
// Directive lines, replayable in order (load_text(save_text) == identity):
//   clear L | clear R
//   set <L|R> <idx> <delay_ms> <weight>
//   count <L|R> <n>
static int fx_channel_save_text(char *buf, size_t cap)
{
    channel_cfg_t cl, cr;
    dsp_cfg_lock();
    cl = s_ch_cfg_L;
    cr = s_ch_cfg_R;
    dsp_cfg_unlock();

    int pos = snprintf(buf, cap, "clear L\nclear R\n");
    const channel_cfg_t *cc[2] = { &cl, &cr };
    const char           nm[2] = { 'L', 'R' };
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < cc[c]->num_paths; i++) {
            pos += snprintf(buf + pos, (pos < (int)cap) ? cap - pos : 0,
                            "set %c %d %.4f %.4f\n", nm[c], i,
                            cc[c]->paths[i].delay_sec * 1000.0f,
                            cc[c]->paths[i].weight);
        }
        pos += snprintf(buf + pos, (pos < (int)cap) ? cap - pos : 0,
                        "count %c %d\n", nm[c], cc[c]->num_paths);
    }
    return (pos < (int)cap) ? pos : -1;
}

static bool fx_channel_load_text(const char *txt)
{
    // Each line applies atomically through the clamped writers above, so even
    // a truncated input leaves a sane (if partial) config.
    bool any = false;
    const char *p = txt;
    while (*p) {
        char ch; int idx, n; float ms, w;
        if (sscanf(p, "set %c %d %f %f", &ch, &idx, &ms, &w) == 4) {
            channel_set_path(ch, idx, ms / 1000.0f, w);
            any = true;
        } else if (sscanf(p, "count %c %d", &ch, &n) == 2) {
            channel_set_count(ch, n);
            any = true;
        } else if (sscanf(p, "clear %c", &ch) == 1) {
            channel_clear(ch);
            any = true;
        }   // unknown/blank lines are skipped (comments, forward-compat)
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return any;
}

// ---- pot binding ----
static void fx_channel_pot_set(float x01)
{
    float ms = s_ch_pot_lo_ms + (s_ch_pot_hi_ms - s_ch_pot_lo_ms) * x01;
    // The delay engine indexes whole samples — skip republishing when the
    // quantized value hasn't moved (one config publish per audible step).
    int samples = (int)(ms * (float)FS_HZ / 1000.0f);
    float wL, wR;
    dsp_cfg_lock();
    int cur = (int)(s_ch_cfg_L.paths[s_ch_pot_tap].delay_sec * (float)FS_HZ);
    wL = s_ch_cfg_L.paths[s_ch_pot_tap].weight;   // knob moves the time only —
    wR = s_ch_cfg_R.paths[s_ch_pot_tap].weight;   // each side keeps its weight
    dsp_cfg_unlock();
    if (samples == cur) return;
    channel_set_path('L', s_ch_pot_tap, ms / 1000.0f, wL);
    channel_set_path('R', s_ch_pot_tap, ms / 1000.0f, wR);
}

// ---- init ----
// dsp_effects_init() calls this before it registers g_fx_channel, once the
// boot geometry is final, so dsp_pair_count() is the real pair count.
void channel_fx_init(void)
{
    const size_t delay_bytes = DELAY_SAMPLES * 2 * sizeof(float);

    // Channel delay rings, one per codec pair, all PSRAM (internal RAM is the
    // board's scarcest resource). A failed alloc mutes only the
    // channel effect on that pair (pass-through).
    int pairs = dsp_pair_count();
    for (int p = 0; p < pairs; p++) {
        s_ch_pair[p].delay_buf =
            (float *)heap_caps_malloc(delay_bytes, MALLOC_CAP_SPIRAM);
        if (s_ch_pair[p].delay_buf) {
            memset(s_ch_pair[p].delay_buf, 0, delay_bytes);
        } else {
            ESP_LOGE(TAG, "PSRAM delay-ring alloc failed for pair %d", p);
            // The pair still runs (other effects have their own state); only
            // fx_channel passes through on it.
        }
    }
    ESP_LOGI(TAG, "channel delay rings: %d pair(s) x %u B PSRAM",
             pairs, (unsigned)delay_bytes);
}

// ---- record (dsp_effects_init registers it) ----
// Channel's web card (EMBED_TXTFILES in main/CMakeLists.txt, NUL-terminated):
// the worked example of an effect-supplied panel.
extern const char _binary_channel_panel_html_start[];

const dsp_effect_t g_fx_channel = {
    .name = "channel", .label = "Channel (L/R multi-tap)", .led_rgb = 0x00FF00,  // green
    .process   = fx_channel,
    .reset     = fx_channel_reset,
    .print     = fx_channel_print,
    .snapshot  = fx_channel_snapshot,
    .cmd       = fx_channel_cmd,
    .save_text = fx_channel_save_text,
    .load_text = fx_channel_load_text,
    .pot_label = "channel delay",
    .pot_set   = fx_channel_pot_set,
    .web_panel = _binary_channel_panel_html_start,
};
