<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# Adding a DSP effect

RadioSonic's DSP effects are data-driven and self-contained. An effect is one
`dsp_effect_t` record ([`main/audio/dsp_effects.h`](../main/audio/dsp_effects.h))
holding the math plus optional hooks for every control surface. The CLI, the
web UI, `status`, the LEDs, the front-panel buttons, the analog pot and
`settings save` all reach effects only through the registry, so an effect from
outside the core is handled exactly like one inside it.

The core firmware carries two effects. Bypass is the registry's one built-in
entry, at index 0. Channel, in
[`main/audio/channel_fx.c`](../main/audio/channel_fx.c), is a source file
that exports a `dsp_effect_t` record and is registered with
`dsp_effects_register()`, the same way your effect will be. It is this page's
worked example and sets every hook except `alias`;
[Worked example: the channel effect](#worked-example-the-channel-effect) goes
through its code hook by hook.

All other effects register from a component outside `main/`.
[`components/rs_student_fx/`](../components/rs_student_fx/) is the sandbox
where your effects live; delete that directory and the firmware still builds
and runs.

---

## The effect contract

Three entries are required: `name`, `label` and `process`, the math.
`dsp_effects_register()` checks only `name` and `process`, but the console
uses the label, so without one `status` prints `(null)` on its `DSP:` line
and `help` leaves out the effect's command. A minimal effect:

```c
static void fx_myeffect(float xL, float xR, float *yL, float *yR)  // one stereo sample
{
    *yL = xL;   // pass-through; your math replaces these two lines
    *yR = xR;
}

const dsp_effect_t g_fx_myeffect = {    // not static: student_fx.c registers it
    .name    = "myeffect",          // CLI/web key, max 15 chars
    .label   = "My Effect",         // what `status` prints
    .led_rgb = 0x00A0FF,            // Mode LED color while active, 0xRRGGBB
    .process = fx_myeffect,
};
```

Two optional fields set how the effect shows up on the front panel:

- `led_rgb` is the color of the Mode LED (LED 3 of the 4-LED strip) while
  the effect is active, as `0xRRGGBB` at full scale; the LED code applies the
  board's brightness. 0 leaves it unset and shows a dim white. Pick a color
  that tells your effect apart from the others on the board (channel uses
  green, `0x00FF00`). `mode` with no argument lists every effect with its
  group and color and flags a color that another group already uses; the
  firmware also logs that clash as a warning at boot.
- `group` is the effect's family for the front-panel buttons. Effects with the
  same `group` string form one entry in the Feature-button cycle, and
  pressing the Feature button while holding BOOT (shift) steps between them;
  LED 4 shows the position within the group (green for the first, orange for
  the later ones). NULL makes the effect a group of its own.

Everything else is optional:

| Hook | Gives you | In channel |
|------|-----------|------------|
| `reset` | state flush when playback ends and when audio resumes after sleep | [`fx_channel_reset`](#reset) |
| `print` | extra detail under `status` | [`fx_channel_print`](#print) |
| `snapshot` | per-block coherent copy of multi-field config | [`fx_channel_snapshot`](#snapshot) |
| `cmd` | a console command named after your effect | [`fx_channel_cmd`](#cmd), the `channel` command |
| `save_text` / `load_text` | `settings save` persistence, the web API (`GET/POST /api/fx/<name>`) and a generic web card | [`fx_channel_save_text` / `fx_channel_load_text`](#save_text-and-load_text) |
| `pot_label` + `pot_set` | the analog knob via `pot fx` | [`"channel delay"` + `fx_channel_pot_set`](#pot-binding) |
| `web_panel` | your own web card in place of the generic one ([Web panel](#web-panel)) | [`main/audio/channel_panel.html`](#channels-panel) |
| `leds` | LEDs 2, 3 and 4 painted by your effect while it is current ([Indicator LEDs](#indicator-leds)) | not used by channel |
| `alias` | an old name that `mode`, `/api/mode`, `/api/fx/<name>` and saved settings still accept after a rename | not used by channel |
| `dsp_meter_publish()` (a function to call) | the LED strip as a live needle + the `meter` in `/api/status` | not used by channel (see [Analyzers and the LED needle](#analyzers-and-the-led-needle)) |

### Registering your effect in the sandbox

The firmware declares `rs_user_effects_init()` weak and calls it at the end
of DSP init; `components/rs_student_fx/student_fx.c` defines it. Your effect
is one new source file in that component, for example
`components/rs_student_fx/myeffect_fx.c`, starting with
`#include "student_fx.h"` (which brings in `dsp_effects.h`) and
`#include "common.h"` (for `FS_HZ` and `AUDIO_MAX_PAIRS`). In a component of
your own, such as one made from the template described below, include
`dsp_effects.h` directly in place of `student_fx.h`. Either way, include the
C and ESP-IDF headers for everything else the file calls. A file with hooks
modelled on channel's needs these, the same headers `channel_fx.c` includes:

```c
#include "common.h"         // FS_HZ, AUDIO_MAX_PAIRS
#include <math.h>           // isnan, to ignore NaN in the config writer
#include <stdio.h>          // printf, snprintf, sscanf
#include <stdlib.h>         // strtol, strtod
#include <string.h>         // memset, strcmp, strchr
#include "esp_heap_caps.h"  // heap_caps_malloc
#include "esp_log.h"        // ESP_LOGE, with your own static const char *TAG
```

Four edits register it:

1. Add the file to the explicit `SRCS` list in
   `components/rs_student_fx/CMakeLists.txt`. A file missing from that list
   is not compiled. List it as well in the file's
   `set_source_files_properties(... PROPERTIES COMPILE_OPTIONS "-O2")` call,
   so `process()` gets the same `-O2` build as the core's signal path.
2. Make the record non-static (`const dsp_effect_t g_fx_myeffect = {...}`, as
   above) so `student_fx.c` can link to it.
3. Declare it in `components/rs_student_fx/student_fx.h`:
   `extern const dsp_effect_t g_fx_myeffect;`. If the effect allocates state
   at boot, declare its init function there as well:
   `void myeffect_init(void);`.
4. In `rs_user_effects_init()` in `student_fx.c`, at the
   `<- your effect goes here` marker, call your init function and register
   the record:

   ```c
   void rs_user_effects_init(void)
   {
       /* ...any init calls and registrations already there... */
       myeffect_init();                       // only if your effect has an init
       dsp_effects_register(&g_fx_myeffect);
   }
   ```

   The marker shows both calls. Leave out `myeffect_init();` if your effect
   has no init function, as with the minimal effect above, because a call to
   a function that was never declared stops the build.

`rs_user_effects_init()` runs after the boot's audio geometry is final, so
`dsp_pair_count()` returns the real codec-pair count there, and before a
saved default effect is applied, so a saved effect resolves by name at boot.
`dsp_effects_register()` logs an error and returns false when the name is
already registered or all `DSP_USER_EFFECT_SLOTS` (12) slots are taken.
Channel takes one before `rs_user_effects_init()` runs, which leaves 11 for
course modules and the sandbox together. Each effect an installed course
module registers takes one of those 11, and so does every
`dsp_effects_register()` call already in `student_fx.c`, so count them
before adding yours. An effect whose
registration failed is missing from the `mode` list that `help` prints.

Channel, in `main/`, has the same four pieces. `main/CMakeLists.txt` lists
`audio/channel_fx.c` in `SRCS` and in its `-O2` call, the record
`g_fx_channel` is non-static, `main/audio/channel_fx.h` declares it next to
`channel_fx_init()`, and the core's `dsp_effects_init()` in
[`main/audio/dsp_effects.c`](../main/audio/dsp_effects.c) registers it with
the same two lines:

```c
    channel_fx_init();
    dsp_effects_register(&g_fx_channel);
```

`dsp_effects_init()` runs them just before it calls `rs_user_effects_init()`,
so channel is always index 1, right after bypass, and the sandbox's effects
follow it.

Only one definition of `rs_user_effects_init` can exist. To develop in a
fully external component instead, remove `components/rs_student_fx/` and
define the hook in yours. Your component's `CMakeLists.txt` must use
`WHOLE_ARCHIVE`, or the linker keeps the weak no-op and silently drops your
effects.
[`templates/radiosonic-filters-template/`](../templates/radiosonic-filters-template/)
is a skeleton for this route: a `CMakeLists.txt` with `WHOLE_ARCHIVE` and the
`-O2` property, and a source file with a commented `myeffect` record and init
function.

After a rebuild your effect appears in `mode`, the web UI's Mode buttons, and
the front-panel Feature cycle, and `settings save` persists it by name. An
effect with a text config also gets its own card on the web page (the page
builds the buttons and the cards from `/api/modes` at load). None of this
needs a change in `main/`.

Course modules, the effects used in SigPro Labs courses, install the same
way as a separate component in `components/rs_course_fx/`. They register
through a second weak hook, `rs_course_effects_init()`, which runs just
before `rs_user_effects_init()`, so they never touch the sandbox and their
effects list before yours. `.gitignore` excludes that directory: course
modules are licensed to course participants only, so they must never be
committed to a repository you share or publish.

---

## Worked example: the channel effect

Channel is a multi-tap delay. Each side (L and R, configured independently)
has up to 10 paths, each a delay and a weight, summed from a 1.5 s stereo
delay ring in PSRAM. Path 0 at 0 ms with weight 1.0 is the dry signal. The
boot default is the dry signal plus three reflections, offset slightly
between the sides.

The code is one source file,
[`main/audio/channel_fx.c`](../main/audio/channel_fx.c), with its header
[`main/audio/channel_fx.h`](../main/audio/channel_fx.h) and its web card,
[`main/audio/channel_panel.html`](../main/audio/channel_panel.html). The
subsections below follow the file from top to bottom: state, config writers,
the hooks, the init function `channel_fx_init()` and the record
`g_fx_channel`. The excerpts are verbatim but abridged, with `/* ... */`
marking each cut, so read them next to the file.

Channel is written and registered the way a sandbox effect is, so its hooks,
init function and record can be copied into your own file as a starting
point. Two things differ, both because channel lives in `main/` and yours
lives in a component:

- The core registers channel: `dsp_effects_init()` calls `channel_fx_init()`
  and `dsp_effects_register(&g_fx_channel)`. You make the same two calls for
  your effect in `rs_user_effects_init()` (see
  [Registering your effect in the sandbox](#registering-your-effect-in-the-sandbox)).
- `main/CMakeLists.txt` compiles `channel_fx.c` at `-O2` and embeds
  `channel_panel.html`. Your component's own `CMakeLists.txt` does both for
  your files.

A copy of `channel_fx.c` needs its two non-static names changed:
`g_fx_channel` and `channel_fx_init` become `g_fx_myeffect` and
`myeffect_init`. The core already defines both, so a copy that keeps them
fails to link with `multiple definition`. In the sandbox, replace
`#include "channel_fx.h"` with `#include "student_fx.h"`, and give `TAG` and
the log messages your own text. The `static` functions are private to the
file and can keep their names.

When you copy the record, give it your own `name`, `label` and `led_rgb`.
Delete `.web_panel` and the `extern` line above the record, or point both at
your own fragment ([Embedding the fragment](#embedding-the-fragment)), and
delete or replace `.pot_label`. The build succeeds with them left in. Your
effect then shows channel's web card, whose buttons post to
`/api/fx/channel` and change channel's settings, and `pot` prints channel's
knob label for it.

To hear it before reading the code, run these on the console:

```
mode channel            # select the effect
status                  # the DSP line, then one line per path
channel L 1 300 0.6     # move the left first reflection to 300 ms, weight 0.6
```

### State

```c
typedef struct {
    uint8_t        num_paths;   // active path count (DSP loop bound)
    channel_path_t paths[CHANNEL_MAX_PATHS];
} channel_cfg_t;

/* ... */
static channel_cfg_t s_ch_cfg_L = { .num_paths = 4, .paths = {
    { 0.000f, 1.00f },  // dry
    { 0.045f, 0.50f },  // first reflection
    { 0.120f, 0.40f },
    { 0.200f, 0.30f },
}};
/* ... */
static channel_cfg_t s_chL, s_chR;

/* ... */
typedef struct {
    float *delay_buf;   // stereo interleaved [L0,R0,L1,R1,...] ring, PSRAM
    int    delay_head;
} channel_pair_t;
static channel_pair_t s_ch_pair[AUDIO_MAX_PAIRS];
```

Channel keeps three kinds of state:

- The staged config, `s_ch_cfg_L` and `s_ch_cfg_R`. The right side has the
  same shape, with its reflections at 48, 128 and 215 ms. The control hooks
  (`cmd`, `load_text`, `pot_set`) write it.
- The snapshot, `s_chL` and `s_chR`. Only the `snapshot` hook writes it and
  only `process()` reads it.
- The delay line: one `channel_pair_t` per codec pair, so each pair in TDM
  mode has its own ring and write position. A ring holds 1.5 s of stereo
  float samples, 576,000 bytes, in PSRAM. Delays are limited to 1000 ms, so
  every read falls inside the ring. The rings are allocated at boot by
  [`channel_fx_init()`](#init).

### Config writers

Every config change goes through one of three writers: `channel_set_path()`,
`channel_set_count()` and `channel_clear()`. `cmd`, `load_text` and `pot_set`
all call them, so the range checks live in one place.

```c
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
```

The writer ignores NaN, clamps the rest, and takes
`dsp_cfg_lock()` only for the assignments. Setting a path past the active
count raises the count, so the delay, the weight and `num_paths` change
together under one lock. An index outside 0 to 9 is ignored.

### process

```c
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
```

The parts to copy:

- `dsp_current_pair()` picks this pair's ring. In TDM mode the framework runs
  `process()` over the whole block once per codec pair, selecting the pair
  first; in standard stereo mode it is always pair 0.
- A NULL ring means the allocation failed, and the effect passes audio
  through. Check your own per-pair buffers the same way.
- The input goes into the ring before any read, so a path at 0 ms reads the
  current sample. That is how path 0 carries the dry signal.
- The loops read only the snapshot (`s_chL`, `s_chR`), take no lock and call
  nothing that can block. Their bounds come from the snapshot's `num_paths`.
- The left output sums only the ring's left samples and the right output only
  its right samples.

### reset

```c
static void fx_channel_reset(void)
{
    for (int p = 0; p < AUDIO_MAX_PAIRS; p++) {
        channel_pair_t *st = &s_ch_pair[p];
        if (st->delay_buf) memset(st->delay_buf, 0, DELAY_SAMPLES * 2 * sizeof(float));
        st->delay_head = 0;
    }
}
```

The framework calls `reset` for every registered effect, including the ones
not selected, when playback ends and when audio resumes after sleep. Channel
clears every pair's ring and rewinds its write position. The config is left
as it was.

### snapshot

```c
static void fx_channel_snapshot(void)
{
    dsp_cfg_lock();
    s_chL = s_ch_cfg_L;
    s_chR = s_ch_cfg_R;
    dsp_cfg_unlock();
}
```

Once per block, before the sample loop, the DSP task calls the current
effect's `snapshot`. Channel copies both staged sides in one critical
section. Every sample in the block, and every TDM pair, then sees the same
left and right config, and a change takes effect at the next block boundary.

### print

```c
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
```

`status` prints the effect's label on its `DSP:` line and then calls
`print`. It runs in the console task, so `printf` is fine. Channel prints
one line for each path where either side has a nonzero weight, reading the
staged config without the lock; a torn read could only garble one line of
display.

### cmd

The console registers a command named after each effect that has a `cmd`
hook, with the effect's `label` as its help text. `channel ...` arrives in
`fx_channel_cmd()` with `argv[0]` set to `"channel"`, in the console task.
The command takes four forms:

```
channel L|R <idx> <delay_ms> <weight>   set one path: idx 0-9, 0-1000 ms, weight -1.0 to +1.0
channel L|R -n <count>                  number of active paths, 0-10 (also --count)
channel L|R -x                          back to dry pass-through (also --clear)
channel pot [tap [lo_ms hi_ms]]         show or set the knob's path and sweep range
```

```c
static int fx_channel_cmd(int argc, char **argv)
{
    const char *usage =
        "Usage: channel L|R <idx> <delay_ms> <weight>  |  -n <count>  |  -x\n"
        "       channel pot [tap [lo_ms hi_ms]]\n";
    if (argc < 2) { printf("%s", usage); return 1; }
    char *endp;
    /* ... */
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
```

Channel parses `argv` by hand because argtable3, the console's usual parser,
reads a negative weight such as `-0.4` as an unknown option. Every number
goes through `strtol` or `strtod` with an end-pointer check, so input like
`12abc` is refused. In the set and `-n` forms, a value outside its range
prints an error, returns 1 and changes nothing. The delay and weight checks
are written as `!(x >= lo && x <= hi)` because `strtod` accepts `nan` and
every comparison with NaN is false: in that form NaN fails the check and gets
the same error (see the trust-boundary note under
[save_text and load_text](#save_text-and-load_text)). Valid input goes
through the same `channel_set_path()` as every other control surface. The
`pot` form is covered under [Pot binding](#pot-binding).

### save_text and load_text

One text format serves both `settings save` and the web API. Channel's is a
set of directive lines that replay in order, documented above
`fx_channel_save_text()`:

```c
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
```

With the boot default config it writes:

```
clear L
clear R
set L 0 0.0000 1.0000
set L 1 45.0000 0.5000
set L 2 120.0000 0.4000
set L 3 200.0000 0.3000
count L 4
set R 0 0.0000 1.0000
set R 1 48.0000 0.5000
set R 2 128.0000 0.4000
set R 3 215.0000 0.3000
count R 4
```

`save_text` copies both sides under the lock and formats after releasing it.
It keeps adding each `snprintf` length even after the output is cut short, so
the final check returns -1 when `cap` was too small. It writes only the
active paths (`i < num_paths`).

Keep the output small; channel's, with all ten paths set on both sides, is
under 600 bytes. Every effect's `save_text` output goes into one 12 KB
settings buffer, filled in registry order: channel first, then the effects
`rs_user_effects_init()` registers, in the order it registers them. `GET /api/fx/<name>` passes a `cap` of 8 KB. When the text does not
fit, return -1 as channel does. `settings save` then leaves out your section
and logs a `config too large` warning, and the GET answers 500
`Config too large`. If the effects before yours have already filled the
buffer, your section and every one after it are left out with no warning.
`settings show` lists the sections each saved slot holds, so it shows
whether yours was stored.

```c
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
```

`load_text` matches each line with `sscanf` and applies it at once through
the writers, so each line is atomic. Atomicity is per line: a block boundary
can fall between `clear L` and the `set L` lines after it, so one block may
play the dry signal. Blank and unknown lines are skipped. It returns true
when at least one line matched. Because `save_text` starts with `clear L`
and `clear R`, replaying its output resets every path the text does not
mention, so no state from before the load survives it. The round trip
restores each side's path count and its active paths. Paths at or beyond
`num_paths` are not saved and come back as `clear` leaves them. After
`channel L -n 2`, `settings save` and `settings load`, for example, the left
side's paths 2 and 3 are zero, and `channel L -n 4` no longer brings their
reflections back.

`load_text` is a trust boundary: its text comes from NVS or an HTTP body.
Channel relies on its writers, which clamp out-of-range delays and weights
and ignore an out-of-range index, and any side letter other than `R` or `r`
selects the left side. A line such as `set L 42 10 0.5` still counts as
matched, so the POST answers `OK` and nothing changes. To report a refused
value, return false when any line is refused. The POST then answers 400
`Config rejected` and `settings load` logs a warning. A `load_text` that
applies each line as it parses, as channel's does, leaves the lines before
the refused one applied.

Range checks written as `x < lo || x > hi` do not catch NaN. `sscanf` and
`strtod` both accept `nan`, and every comparison with NaN is false. Channel's
console parser writes its checks as `!(x >= lo && x <= hi)`, which NaN fails,
and `load_text` passes values straight to `channel_set_path()`, which drops
NaN with `isnan()` before it clamps. Without those checks a POST line
`set L 1 nan 0.5` would put NaN in the config that `process()` converts to a
delay index. Do the same in your own effect.

The same text reaches the effect from three places:

- `GET /api/fx/channel` returns the `save_text` output as `text/plain`.
- `POST /api/fx/channel` with a `text/plain` body of directive lines, such as
  `set L 1 30 0.6`, applies them and answers `OK`, or 400 `Config rejected`
  when no line matched.
- `settings save` stores the text in NVS under a `[channel]` section header,
  and `settings load` hands that section back to `load_text`.

The settings store reads any line that starts with `[` as the next section
header, so no line of your `save_text` output may start with `[`. The
effect's section would end at that line, and the lines after it would never
reach your `load_text`.

### Pot binding

```c
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
```

The record sets `.pot_label = "channel delay"`, which `pot` and `pot fx`
print. With the knob routed by `pot fx` and channel current, the button-poll
task reads the knob every 20 ms and calls `pot_set` with 0.0 at the left stop
and 1.0 at the right. It calls only after the filtered reading moves past a
dead-band, so a knob at rest never overrides a value set from the console or
the web.

`fx_channel_pot_set()` maps the position linearly onto a sweep range and
moves one path's delay on both sides:

- The defaults are path 1 over 0 to 250 ms. `channel pot <tap> <lo_ms>
  <hi_ms>` changes them; `settings save` does not store them.
- It reads the path's weights under the lock and writes them back unchanged,
  so the knob moves only the time. Both sides get the same delay, so turning
  the knob replaces the default 45/48 ms offset on path 1.
- The delay engine works in whole samples, so it skips the publish when the
  new delay, in samples, equals the left side's current one.
- It publishes through `channel_set_path()`, the writer `cmd` uses.

### init

```c
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
```

`dsp_effect_t` has no init field, so the init function is called by name,
once at boot, next to the registration. `dsp_effects_init()` calls
`channel_fx_init()` after the audio geometry is final and calls
`rs_user_effects_init()` right after, so `dsp_pair_count()` returns the real
codec-pair count in both. The function allocates one PSRAM ring per
pair, zeroes it, and logs an error when an allocation fails. Entries past the
pair count stay NULL, and `process()` and `reset` check for NULL before they
touch a ring. An effect with per-pair buffers follows the same pattern in its
own init function.

### The record

```c
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
```

The record is defined at file scope, so it exists for the whole run, which
the registry needs because it keeps a pointer to the record. It has no
`static` keyword, so another file can link to it.
[`main/audio/channel_fx.h`](../main/audio/channel_fx.h) declares it next to
the init function:

```c
// channel_fx.c: L/R multi-tap delay (the in-tree worked example)
extern const dsp_effect_t g_fx_channel;
void channel_fx_init(void);
```

`dsp_effects.c` includes that header and registers the record from
`dsp_effects_init()`, as
[Registering your effect in the sandbox](#registering-your-effect-in-the-sandbox)
shows. The fields:

- `name` is the console command, the key for `mode channel`, the URL segment
  in `/api/fx/channel` and the settings section header.
- `label` is what `status` prints and the console help text. The web Mode
  button shows the label when it is 12 characters or fewer and the
  capitalized name otherwise, so channel's button reads `Channel`.
- `led_rgb` is green. `group` is unset, so channel is a group of its own and
  the first entry in the Feature-button cycle. Bypass has its own button and
  is not in the cycle.
- `web_panel` points at the embedded `channel_panel.html`;
  [Web panel](#web-panel) covers the fragment and how it is embedded.

Your effect's record has the same shape: a non-static
`const dsp_effect_t g_fx_myeffect = { ... };` at file scope, declared in
`student_fx.h` next to your init function and registered from
`rs_user_effects_init()`.

---

## Web panel

At load the web page reads `GET /api/modes`, which lists the registry in
order:

```json
[{"name":"bypass","label":"Bypass","group":"bypass","led":"#000000","config":false,"panel":false},
 {"name":"channel","label":"Channel (L/R multi-tap)","group":"channel","led":"#00FF00","config":true,"panel":true}, ...]
```

`group` is the effect's front-panel group and `led` the Mode LED color the
strip shows for it (`#000000` for bypass, dim white `#808080` when `led_rgb`
is unset). `config` is true when the effect has both `save_text` and
`load_text`, and `panel` is true when it has a `web_panel`. The page then
walks the list in registry order and decides each effect's card:

- With a panel, the card is the effect's own fragment, fetched from
  `GET /api/fx/<name>/panel` (404 when the effect has none). If the fetch
  fails, an effect with a text config gets the generic card instead.
- With a text config and no panel, the card is the generic one: a text box
  filled from `GET /api/fx/<name>`, with Read (fetch it again), Load file
  (fill the box from a local `.txt` file and apply it) and Apply. Apply posts
  the text to `POST /api/fx/<name>`, leaving out blank lines and lines that
  start with `#`, so a config file can carry comments.
- With neither (bypass), there is no card.

The generic card shows the effect's `save_text` output and applies edited
lines through its `load_text`. Channel has a panel, so its generic card
appears only if the panel fetch fails, and then shows the directive lines
listed under [save_text and load_text](#save_text-and-load_text). Write a
panel when labelled fields, ranges, or one-click actions would be easier to
use than editing directive lines.

### The fragment contract

A panel is the inner HTML of one card. The page supplies the card and its
stylesheet, so the page's `row` class (a label and its controls on one line)
and `log` class (a small output area) work inside a panel.

- Start with an `<h2>` title.
- Prefix every element id with the effect name (`channel-side`,
  `channel-ms`), so ids cannot collide with the page or another panel.
- Include at most one classic `<script>`. The page runs it once, when it adds
  the card.
- The script's top level declares only `function` and `async function`
  functions, each named with the effect name as prefix (`channelSetTap`,
  `channelClear`), and may call them, for example to fill the fields when
  the card appears. Panel scripts share the page's global scope: a function
  declaration with an existing name silently replaces the page's own
  function, and a top-level `const`, `let` or `class` can clash with the
  page's, so leave those out.
- Those functions may call the page helpers: `byId(id)`; `fxGet(name)` and
  `fxPost(name, text)`, which are async and resolve to the firmware's
  response text; `postForm(url, fields)` for a form POST to any API
  endpoint; and `refreshStatus()` to repaint the Status card.
- Write plain ES2017 (async/await is fine) and load nothing external: no CDN
  scripts, fonts or images. In AP mode the browser has no internet.

### Channel's panel

[`main/audio/channel_panel.html`](../main/audio/channel_panel.html) follows
the contract. A comment at the top of the file restates the rules; the rest
is:

```html
<h2>Channel (Delay / Reverb)</h2>
<div class="row">
  <label>Ch</label>
  <select id="channel-side"><option>L</option><option>R</option></select>
  <label style="margin-left:.3rem">Idx</label>
  <input id="channel-idx" type="number" min="0" max="9" value="0" style="width:48px">
</div>
<div class="row">
  <label>ms</label>
  <input id="channel-ms" type="number" min="0" max="1000" value="0">
</div>
<div class="row">
  <label>w</label>
  <input id="channel-w" type="number" step="0.01" min="-1" max="1" value="1.0">
</div>
<div class="row">
  <button onclick="channelSetTap()">Set tap</button>
  <button onclick="channelClear()">Clear ch</button>
</div>
<div class="row">
  <label>Count</label>
  <input id="channel-count" type="number" min="0" max="10" value="1">
  <button onclick="channelSetCount()">Set</button>
</div>
<script>
  /* Each handler sends one of the channel effect's own config directives
     (the same lines GET /api/fx/channel returns) through fxPost, then
     refreshes the Status card. */

  async function channelSetTap() {
    await fxPost('channel',
      `set ${byId('channel-side').value} ${byId('channel-idx').value}` +
      ` ${byId('channel-ms').value} ${byId('channel-w').value}`);
    refreshStatus();
  }

  async function channelClear() {
    await fxPost('channel', 'clear ' + byId('channel-side').value);
    refreshStatus();
  }

  async function channelSetCount() {
    await fxPost('channel', `count ${byId('channel-side').value} ${byId('channel-count').value}`);
    refreshStatus();
  }
</script>
```

- The `<h2>` is the card title, and every id starts with `channel-`.
- The script declares three functions, `channelSetTap`, `channelClear` and
  `channelSetCount`, which the buttons' `onclick` attributes call. Each sends
  one of channel's directives through `fxPost('channel', ...)` and then calls
  `refreshStatus()`.
- The handlers ignore the reply from `fxPost`. A panel that should report a
  rejected line writes the reply into an element, such as one with the
  `log` class.
- The panel never reads the config back, so its fields start at their HTML
  defaults (side L, path 0, 0 ms, weight 1.0, count 1). A panel that should
  show the device's current config calls `fxGet(name)` from a function the
  script runs at its top level, and fills the fields from the reply.

Each button sends the same `set`, `clear` or `count` line a user would type
into the generic card or post to `/api/fx/channel`. `load_text` stays the
trust boundary and checks the values whatever sent them.

### Embedding the fragment

The fragment is an `.html` file compiled into the firmware with ESP-IDF's
`EMBED_TXTFILES`, which appends a NUL so the data is a C string. A panel
change therefore needs a rebuild and flash.

Channel's panel takes three pieces. `main/CMakeLists.txt` lists the file:

```cmake
    # channel_panel.html is the channel effect's web card (its .web_panel).
    EMBED_TXTFILES "ui/web_page.html" "audio/channel_panel.html"
```

`channel_fx.c` declares the symbol ESP-IDF generates and points the record's
`web_panel` at it:

```c
extern const char _binary_channel_panel_html_start[];
/* ... */
    .web_panel = _binary_channel_panel_html_start,
```

The symbol is `_binary_`, then the file name with every character that is
not a letter or a digit replaced by `_`, then `_start` (ESP-IDF builds it
with CMake's `string(MAKE_C_IDENTIFIER)`). The directory is not part of it, so
`audio/channel_panel.html` becomes `_binary_channel_panel_html_start`, and a
file named `echo-panel.v2.html` would become `_binary_echo_panel_v2_html_start`.
The symbol is global, so the converted name must be unique across the whole
firmware. Two panels named `my-panel.html` and `my_panel.html`, even in
different components, would produce the same symbol.

A sandbox effect puts the same three pieces in its component:

1. Save the fragment in `components/rs_student_fx/` under a name no other
   component uses, for example `my_panel.html`.
2. In `components/rs_student_fx/CMakeLists.txt`, add `"my_panel.html"` to
   the `EMBED_TXTFILES` line inside `idf_component_register()`. If the file
   has no such line, add `EMBED_TXTFILES "my_panel.html"` there. Leave
   `main/CMakeLists.txt` as it is.
3. In your effect's source file, declare the symbol and point `web_panel` at
   it:

   ```c
   extern const char _binary_my_panel_html_start[];

   const dsp_effect_t g_fx_myeffect = {
       .name = "myeffect", .label = "My Effect", .led_rgb = 0x00A0FF,
       /* ...your hooks... */
       .web_panel = _binary_my_panel_html_start,
   };
   ```

---

## Rules of the road

### What the framework guarantees and expects

- `process()` gets floats in [-1, +1] and should return roughly the same
  range. The framework hard-clips the output to that range, so an overshoot
  distorts and does no other harm. With expansion codecs (TDM) the clip
  applies to the stereo mix of all pairs' outputs.
- `process()` runs on core 1 inside the real-time DSP task, 48,000 times per
  second for each codec pair (one pair in standard stereo, up to three with
  expansion codecs). No `printf`, no `malloc`, no blocking calls and no
  ESP-IDF APIs in the hot path: only math on memory you already own.
- `reset()` runs for every registered effect when playback ends and when
  audio resumes after sleep. Clear delay lines and filter state there and do
  nothing else, because when playback ends it runs in an audio task.
- `cmd()` runs in the console task (printf is fine). Parse `argv` by hand if
  any argument can be negative, because argtable3 misreads `-1.0` as an
  option flag.
- `load_text()` input arrives from NVS or an HTTP POST, so it is a trust
  boundary. Reject non-finite numbers with `isfinite()`, then validate and
  clamp every value before it can reach the hot path.
- Contract: `load_text(save_text())` must restore everything `process()`
  uses. Channel's `clear`, `set` and `count` lines meet it by starting from
  `clear` and saving each side's count and active paths; paths beyond the
  count are not saved and come back cleared. The same lines work typed into
  the generic web card or posted to the API.
- No line of the `save_text` output may start with `[`, because the settings
  store reads such a line as the next effect's section header.

### Parameters and thread safety

- A single parameter that fits one aligned 32-bit word, such as one gain or
  one rate, can be a plain file-static. Aligned 32-bit writes are atomic on
  the ESP32-S3, so `cmd` and `load_text` can change it live without the
  lock.
- Fields that must change together must be published atomically. Channel's
  `channel_set_path()` changes a path's delay and weight and can raise the
  side's path count; a block that saw half of that change would sum a path
  nobody set up. In a recursive filter the risk is worse, because a new `b0`
  with an old `a1` can make it unstable. Stage such fields under
  `dsp_cfg_lock()` / `dsp_cfg_unlock()`, and implement the `snapshot` hook to
  copy them, under the same lock, into what `process()` reads. The framework
  calls the current effect's `snapshot` once per block, so every sample in a
  block sees one coherent set. In channel, `channel_set_path()` stages each
  change under the lock and `fx_channel_snapshot()` copies both sides once
  per block. Skip the lock only where one block of mixed old and new values
  is harmless, and say why in a comment.
- Keep the lock short. It is a critical section that masks interrupts on the
  core holding it, so clamp before taking it and format after releasing it,
  as `channel_set_path()` and `fx_channel_save_text()` do. `process()` never
  takes it.

### Per-pair state (TDM)

With expansion codecs the same effect runs once per codec pair over each
block. Per-sample state (delay lines, filter history) therefore needs one
copy per pair: size it `[AUDIO_MAX_PAIRS]` and index it with
`dsp_current_pair()` inside `process()`, as channel does with `s_ch_pair[]`.
Allocate large per-pair buffers in PSRAM from your init function, one for
each of the `dsp_pair_count()` pairs, as `channel_fx_init()` does;
`rs_user_effects_init()` runs after the TDM geometry is final, so the count is
right there. Pass audio through when
a pair's buffer is NULL, as `fx_channel()` does, and clear every pair in
`reset`, as `fx_channel_reset()` does.

An analyzer that keeps one shared state and only measures the main board's
input needs no per-pair copy. Its `process()` writes the outputs first
(`*yL = xL; *yR = xR;` for pass-through) and returns before the analysis
when `dsp_current_pair()` is not 0.

### CPU budget

- In standard stereo a block is 512 samples, 10.67 ms at 48 kHz, and
  `process()` runs 512 times per block. With expansion codecs (TDM) a block
  is 256 frames, 5.33 ms, and `process()` runs once per frame for each codec
  pair, so the time each call can take shrinks by the pair count. Your
  per-call cost times the calls per block must fit well inside the block.
  The `Load` line of `status` shows the live per-block time and the budget
  for the current mode, which are the numbers to watch while you tune. For
  scale, read channel's Load at its default 4 paths per side, then again
  with all 10 set on both sides: each path is one ring read per sample, so
  the full config makes 20. When the DSP overruns, incoming audio is lost
  (I2S RX DMA overruns) and the output glitches.
- Avoid data-dependent branches inside per-sample loops. A circular buffer's
  wrap test on every read is the usual offender, and its cost grows with the
  number of reads. Channel's `ch_delayed_sample()` tests for wrap on each of
  its at most 20 reads per sample. An effect that makes hundreds of reads per
  sample should lay out its buffer so the inner loop needs no wrap test.
- Buffers beyond a few KB belong in PSRAM
  (`heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM)`, allocated once at init).
  Channel's delay ring is 1.5 s × 48 kHz × 2 channels × 4 bytes = 576,000
  bytes per pair. Small state is fine as ordinary statics, which live in
  internal RAM and are faster to read. Internal RAM is the board's scarcest
  resource, so before adding more than a few KB of it, compare the internal
  `Heap` figures that `status` prints with and without your change.

### Effects that own a task

Work too heavy for `process()`, such as a long FFT every few hundred
milliseconds, can run in a task the effect creates. `process()` hands the task
its samples through a ring buffer, and the task does the work on core 0.

- **One task, created once.** Create it in the effect's init function at
  boot. Pin it to core 0 with `xTaskCreatePinnedToCore(..., 0)` (core 1 runs
  audio only) at priority 2 or lower, so the console, the web server and the
  recorder stay ahead of it. Add a row for it to the task map in
  `app_main` step 5 of
  [`main/radiosonic_main.c`](../main/radiosonic_main.c).
- **A ring with one writer and one reader.** Put the ring in PSRAM.
  `process()` writes samples and then advances the head; the task reads them
  and then advances the tail. Each index is a `uint32_t` written by one side
  only, so neither side needs a lock, but the order of memory accesses
  matters: publish an index with `__atomic_store_n(..., __ATOMIC_RELEASE)`
  and read the other side's with `__atomic_load_n(..., __ATOMIC_ACQUIRE)`,
  so a reader never sees a new head before the samples behind it, and a
  writer never reuses a slot before the reader is done with it. When the ring is full,
  `process()` drops the new samples and counts them; show that count in your
  status output. Size the ring for a few of the task's work units, so a task
  that falls briefly behind loses nothing.
- **No RTOS calls in `process()`.** The rule in
  [What the framework guarantees and expects](#what-the-framework-guarantees-and-expects)
  still holds: no queue sends, task notifications or semaphores. The task
  finds new samples by polling the head.
- **Sleep between polls.** The task calls `vTaskDelay()` between polls (every
  50 ms suits work units of a few hundred milliseconds) and between chunks of
  a long computation. Core 0's idle task must run at least every 5 s, or the
  task watchdog trips.
- **Stack.** A task stack comes from internal RAM by default, and one of
  4 KB or more is a row in the internal RAM ledger
  ([`docs/ARCHITECTURE.md`](ARCHITECTURE.md), "Internal RAM budget"),
  checked against the boot `heap:` lines. A task that never calls flash or
  NVS functions can keep its stack in PSRAM instead, which costs internal RAM
  only for the task's control block:

  ```c
  xTaskCreatePinnedToCoreWithCaps(myeffect_task, "myfx", 4096, NULL, 2,
                                  &s_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ```

  Flash and NVS calls, reads included, turn off the cache, and PSRAM with
  it, so a task whose stack is in PSRAM must never make them: no `settings`
  or NVS calls, no partition or SPI-flash calls. Math, `vTaskDelay()` and
  `ESP_LOGI` are fine. This needs `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`,
  which ESP-IDF enables by default on the ESP32-S3.
- **Buffers.** Allocate the task's buffers in PSRAM once, at init, with
  `heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM)` where a SIMD kernel
  needs 16-byte alignment. A library's own init function can allocate tables
  in internal RAM without saying so: read what it allocates before you call
  it, and compare the `heap:` boot lines with and without your effect. Any
  internal allocation of 4 KB or more goes in the ledger.
- **Shared PSRAM bandwidth.** Core 1 reads PSRAM too (delay lines, the record
  and playback rings), so a task that streams through large PSRAM buffers can
  slow the audio tasks. Check that the `status` Load line and the overrun
  counters stay where they were with your effect running. Time the task's own
  work with `esp_timer_get_time()` and report the average and maximum in your
  status output.
- **Leaving the effect.** `dsp_effect_t` has no select or deselect hook, and
  `process()` runs only while the effect is current. The task can tell from
  the ring instead: when the head has not moved for 500 ms, the effect is no
  longer current, and the task clears its state, so selecting the effect
  again starts fresh. Leave `reset` empty or NULL, because it runs in an
  audio task and must not touch state the task owns.
- **Values shared with the task.** Parameters the task reads follow
  [Parameters and thread safety](#parameters-and-thread-safety): a single
  32-bit value can be a plain static, and for several that change together
  the task takes a copy under `dsp_cfg_lock()` once per work unit. Results go
  the other way as `volatile` scalars that the task writes and the `print`,
  `cmd`, `save_text` and `leds` hooks only read.
- **Logging.** The task may log with `ESP_LOGI`, one line per change of
  state, never one per work unit, because the console drops output under load
  ([`docs/ARCHITECTURE.md`](ARCHITECTURE.md), "Resource-change checklist").
  Keep the string `DSP:` out of the lines: host tools read it as a mode
  change.

### Analyzers and the LED needle

An analyzer passes audio through and publishes a measurement with
`dsp_meter_publish(value, conf)` from the DSP task, typically in `process()`
each time a measurement completes. Value −1 is the leftmost
LED and +1 the rightmost; conf 0 shows the idle glow and 1 full brightness.
While the meter is being published the strip renders it as the orange needle
automatically, and `/api/status` carries it. Scale the value to something a
person can read off the strip, such as a direction or a level.

### Indicator LEDs

An effect that reports up to three on/off results can drive LEDs 2, 3 and 4
itself through the `leds` hook. While the effect is current, the LED task
calls the hook on every poll, about 10 times a second, and paints `rgb[0]`,
`rgb[1]` and `rgb[2]` onto LEDs 2, 3 and 4 in place of the Bypass, Mode and
Sub-mode view. LED 1 keeps showing the input source. Colors are `0xRRGGBB` at
full scale with 0 for off, and the LED code applies the board's brightness,
as it does for `led_rgb`. The strip is repainted only when a value changes.
When another effect becomes current, for example when Bypass is pressed, the
normal view returns.

```c
static volatile bool s_found[3];   // written by the effect's task

static void fx_myeffect_leds(uint32_t rgb[3])
{
    for (int i = 0; i < 3; i++) rgb[i] = s_found[i] ? 0x0000FF : 0;   // blue, or off
}
```

- The hook runs in the LED task on core 0, at the same time as `process()`
  and any task of your own. Read values your effect has already published,
  as `volatile` scalars, and return. Do no computing, locking or logging
  there.
- Never call the strip driver (`rgb_leds_*`) from your effect or its task.
  The LED task is its only writer, and the driver's pixel buffer and RMT
  channel have no lock.
- Use one hue for all three. The strip's red, green and blue dies differ in
  brightness at the same setting, so mixed colors read unevenly.
- Use the meter or `leds`, not both. While an effect with a `leds` hook is
  current the meter is ignored, so a stray publish from another effect (every
  effect's reset runs on a flush) cannot take the strip.
- Still set `led_rgb`: `mode` lists it and the boot clash check compares it.

### Naming

`name` is the CLI/web key: lowercase, no spaces, at most 15 characters, so it
fits the settings store's name field and the web API's name buffers. It must
also differ from every console command, and nothing checks that for you: ESP-IDF's
console keeps the last command registered under a name. An effect named
`status` or `gain` therefore replaces that core command, and an effect named
`codec` or `ver` loses its own command to the core one. Run `help` and pick a
name it does not list. If you rename an effect, put the old
name in `alias` so scripts and saved settings that use it keep working. A
real name always wins over an alias.

---

## Checklist

- [ ] `fx_*()` math (per-sample, float in/out, no blocking calls)
- [ ] `fx_*_reset()` clears all state, for every pair
- [ ] `led_rgb` that tells your effect apart on the Mode LED; `group` if it
      belongs in a Feature-button family with other effects
- [ ] multi-field params: stage under `dsp_cfg_lock()` + `snapshot` hook
- [ ] per-pair state indexed by `dsp_current_pair()` *(or a pair-0 early-out
      that passes audio through)*
- [ ] large per-pair buffers allocated in PSRAM by your init function, called
      from `rs_user_effects_init()`, with a NULL check in `process()`
- [ ] optional: `print`, `cmd`, `save_text`/`load_text` (validate input), pot
      hook, `alias`, `dsp_meter_publish`
- [ ] optional `web_panel`: an `.html` fragment with a unique file name next
      to the source, listed in the component's `EMBED_TXTFILES`, with ids and
      functions prefixed by the effect name
- [ ] optional `leds`: reads only `volatile` values the effect publishes;
      NULL in an effect that publishes on the meter
- [ ] optional task of its own: one, created at init, pinned to core 0 at
      priority 2 or lower, fed through a PSRAM ring with no RTOS calls in
      `process()`, `vTaskDelay()` between polls, a row in the task map, and
      its stack in the RAM ledger or in PSRAM
- [ ] each hook compared with its `fx_channel_*` counterpart, and the init
      function with `channel_fx_init()`, in `main/audio/channel_fx.c`
- [ ] registered: file in `SRCS` and the `-O2` list (component
      `CMakeLists.txt`), non-static `g_fx_*` record, `extern` and init
      declared in `student_fx.h`, init call and `dsp_effects_register()` in
      `rs_user_effects_init()` (`student_fx.c`)
- [ ] build and flash, then check `mode <name>`, `status`, your command,
      buttons and LEDs, the web card, and a `settings save`/`load` round trip
