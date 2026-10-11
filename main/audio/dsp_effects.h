// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ==========================================================
// DSP effect registry — the framework contract
// ==========================================================
// Each DSP effect is a self-contained registry entry. The framework
// (audio_dsp.c) owns int<->float conversion, the L/R unpack/repack, the
// sample loop, and output clipping; an effect implements the math for one
// stereo sample plus its own state — and, through the OPTIONAL hooks below,
// its own console verbs, web/settings serialization, LED meter, and pot
// binding. Every control surface in the firmware (CLI, web API + page,
// `status`, LEDs, front-panel buttons, the analog pot, settings persistence)
// reaches effects ONLY through this registry: an out-of-tree effect is a
// first-class citizen by construction, because in-tree effects have no other
// door either.
//
// To ADD AN EFFECT: see docs/ADDING_EFFECTS.md. Out-of-tree effects live in
// components/rs_student_fx/ (the sandbox) and reach the core only through
// the dsp_effect_t record below.

typedef struct {
    const char   *name;     // CLI/web key, e.g. "channel" (max 15 chars)
    const char   *label;    // human label for `status`
    // Front-panel family: effects with the same group string form one entry
    // in the Feature-button cycle, and Feature pressed with BOOT (shift) held
    // steps between them (LED 4 shows the position). NULL = the effect is a
    // group of its own.
    const char   *group;
    // Mode LED (LED 3) color while this effect runs, 0xRRGGBB at full scale;
    // led_status.c only applies the strip's brightness. 0 = unset (dim white).
    // The designer picks it: choose one that tells the effect apart.
    uint32_t      led_rgb;
    void (*process)(float xL, float xR, float *yL, float *yR);  // one stereo sample
    void (*reset)(void);    // clear state on flush; may be NULL

    // ---- Optional control-surface hooks (any may be NULL) ------------------
    void (*print)(void);    // extra `status` detail (printf)
    // Per-block coherent-config copy. Runs in the DSP task once per block for
    // the CURRENT effect, before the sample loop. Copy your staged config
    // (written by cmd/load_text under dsp_cfg_lock) into the snapshot your
    // process() reads, so a block never sees a half-updated parameter set.
    void (*snapshot)(void);
    // Console command: the console registers each effect's name as a command
    // and routes "<name> <args...>" here (argv[0] = the effect name, exactly
    // like a standalone esp_console handler). Runs in the console task —
    // printf is fine. Manual argv parsing recommended (argtable3 rejects
    // negative numbers as unknown options).
    int  (*cmd)(int argc, char **argv);
    // Config serialization, ONE text format per effect, used by BOTH settings
    // persistence and the web API (GET/POST /api/fx/<name>):
    //   save_text: write the directive lines that recreate the current config
    //              into buf (NUL-terminated); return the byte length written
    //              (excluding the NUL), or -1 if cap is too small.
    //   load_text: parse and APPLY the text. The text is a TRUST BOUNDARY
    //              (NVS blob or HTTP body): validate and clamp everything.
    //              Return false on a malformed input (config unchanged or
    //              partially applied — keep each line atomic).
    // Contract: load_text(save_text output) must restore the config.
    int  (*save_text)(char *buf, size_t cap);
    bool (*load_text)(const char *txt);
    // Analog pot binding: with pot_label non-NULL, `pot fx` routes the knob
    // here (x01 = 0.0 at the left stop .. 1.0 at the right). Runs in the
    // button-poll task at ~50 Hz after a dead-band — publish config the same
    // way cmd would. pot_label is the short name `pot` prints.
    const char *pot_label;
    void (*pot_set)(float x01);
    // Web UI card: an HTML fragment (markup plus at most one <script>) that
    // the page shows as this effect's card, served from GET
    // /api/fx/<name>/panel. NULL gives the generic card, a text box over
    // GET/POST /api/fx/<name> (needs save_text and load_text). Embed the
    // fragment with EMBED_TXTFILES; docs/ADDING_EFFECTS.md has the contract
    // and main/audio/channel_panel.html is the worked example.
    const char *web_panel;
    // Optional legacy name that dsp_find also accepts (CLI `mode`, /api/mode,
    // /api/fx/<name>, saved settings), so a renamed effect keeps old scripts
    // working. A real name always wins over an alias.
    const char *alias;
} dsp_effect_t;

// Unified registry view: bypass at index 0, then registered effects in call
// order (channel from dsp_effects_init(), then rs_user_effects_init()'s).
// ALL iteration goes through these two, so a registered effect needs no
// special case anywhere.
int                 dsp_effect_count(void);
const dsp_effect_t *dsp_effect_at(int idx);   // NULL if idx out of range

// Active effect (index into the unified registry, stored in g_state.dsp_effect).
const dsp_effect_t *dsp_current(void);
bool        dsp_is_bypass(const dsp_effect_t *fx);     // the built-in pass-through
const char *dsp_effect_group(const dsp_effect_t *fx);  // fx->group, or fx->name if NULL
// Mode LED color the strip shows for an effect, full-scale 0xRRGGBB: 0 (off)
// for bypass, DSP_LED_RGB_DEFAULT (dim white) when led_rgb is unset.
#define DSP_LED_RGB_DEFAULT 0x808080u
uint32_t    dsp_effect_led(const dsp_effect_t *fx);
// Index of an effect in a different group that shows the same Mode LED
// color as fx, or -1. `mode` flags these and boot logs a warning, because
// two groups with one color cannot be told apart on the strip.
int         dsp_led_clash(const dsp_effect_t *fx);
int  dsp_find(const char *name);   // index by name, then by alias; -1 if none
void dsp_set(int idx);             // bounds-checked select

// ---- Out-of-tree effect registration ----------------------------------------
// An ESP-IDF component (components/rs_student_fx/ is the sandbox; see
// docs/ADDING_EFFECTS.md for replacing or extending it) registers its
// effects by defining rs_user_effects_init(), which the firmware declares
// WEAK and calls at the end of dsp_effects_init():
//
//   const dsp_effect_t g_fx_myeffect = { .name = "myeffect", ... };  // myeffect_fx.c
//   void rs_user_effects_init(void) { dsp_effects_register(&g_fx_myeffect); }
//
// The dsp_effect_t (and everything it points to) must have static storage
// duration — the registry keeps the pointer.
//
// Course modules (provided separately to course participants) install as a
// second component, components/rs_course_fx/, and register the same way from
// rs_course_effects_init(), which runs just before rs_user_effects_init().
//
// dsp_effects_init() registers the core's channel effect into one of these
// slots first, which leaves 8 for course modules and the sandbox together.
#define DSP_USER_EFFECT_SLOTS 9
bool dsp_effects_register(const dsp_effect_t *fx);   // false = table full/invalid
void rs_course_effects_init(void);                   // weak no-op; overridden by components/rs_course_fx/
void rs_user_effects_init(void);                     // weak no-op; overridden by the effects component

// Position of the current effect within its group, and the group size (used
// by the LED status: a group with a single effect lights no sub-mode LED).
void dsp_current_submode(int *pos, int *count);

// ---- Framework hooks (called from audio_dsp.c) ------------------------------
void dsp_effects_init(void);       // pair gating, channel registration, rs_user_effects_init()
void dsp_effects_reset_all(void);  // flush every effect's state (all pairs)
void dsp_effects_snapshot(void);   // per-block: dispatches to current->snapshot

// Select which codec pair's state the per-sample effects operate on (TDM
// per-channel DSP; pair 0 = main codec). Returns false if this boot's
// geometry has no such pair; the framework skips it. Called only from the DSP
// task.
bool dsp_effects_select_pair(int p);

// Per-pair state support for effects (in- or out-of-tree): the pair the
// framework selected for the running sample loop, and how many pairs this
// boot's geometry carries (1 in std stereo; codecs/2 in TDM). An effect that
// wants independent state per codec pair sizes its state [dsp_pair_count()]
// (capped at AUDIO_MAX_PAIRS) and indexes it with dsp_current_pair() inside
// process(). Effects with one shared state can ignore both.
//
// dsp_current_pair() is inline because process() calls it once per sample,
// and an out-of-line call there costs a call frame per sample on the audio
// core. g_dsp_cur_pair is the framework variable behind it: only
// dsp_effects_select_pair() writes it, and effects read it through
// dsp_current_pair().
extern int g_dsp_cur_pair;
static inline int dsp_current_pair(void) { return g_dsp_cur_pair; }
int dsp_pair_count(void);

// Thread-safe config publish. cmd/load_text/pot_set writers stage multi-field
// configs under this lock; the effect's snapshot hook copies under the same
// lock. Single aligned 32-bit scalars don't need it (atomic on Xtensa).
void dsp_cfg_lock(void);
void dsp_cfg_unlock(void);

// ---- Meter: generic effect -> LED-needle channel ----------------------------
// An analyzer effect publishes a normalized reading from its process()/dump
// path; the LED strip renders the CURRENT effect's meter as the orange needle
// whenever the seq is advancing (led_status.c), and /api/status reports it.
//   value: -1.0 = leftmost LED .. +1.0 = rightmost
//   conf:  0.0 = no lock (strip shows the idle glow) .. 1.0 = full brightness
// Writes are lock-free volatile (single writer: the DSP task); seq bumps on
// every publish so UIs can detect a stalled/idle analyzer.
void dsp_meter_publish(float value, float conf);
void dsp_meter_read(float *value, float *conf, uint32_t *seq);
