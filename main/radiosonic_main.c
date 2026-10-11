// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// ==========================================================
// RadioSonic — composition root
// ==========================================================
// app_main brings the system up in dependency order (SPI/codec probe →
// geometry → storage → I2S → DSP → settings → WiFi/web → tasks → console)
// and owns everything real-time: the audio tasks, their queues, the ping-pong
// buffers, and the I2S channels. The data path is
//
//   AIC3204 → I2S RX DMA → rx_fill(11,C1) → dsp(9,C1) → tx_feed(12,C1) → I2S TX
//
// with the recorder/playback rings tapping in from audio_store.c. The full
// picture (task table, rings, control plane, memory budget) is in
// docs/ARCHITECTURE.md; the task/core rules live at the task map in step 5.
#include "common.h"
#include "audio_pipeline.h"
#include "audio_codec.h"
#include "input_guard.h"
#include "audio_dsp.h"
#include "dsp_effects.h"
#include "sd_storage.h"
#include "cli_console.h"
#include "audio_store.h"
#include "wifi_manager.h"
#include "power_sleep.h"
#include "control_buttons.h"
#include "control_pot.h"
#include "led_status.h"
#include "rgb_leds.h"
#include "serial_xfer.h"
#include "settings_store.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include <assert.h>

// ==========================================================
// Global Variable Instantiation
// ==========================================================
// Effect configs (channel preset, filter coefficients, ...) live inside the
// effects themselves; see the compile-time defaults in channel_fx.c and
// components/rs_student_fx/.
app_state_t g_state = {
    .input_mode     = INPUT_MIC,
    .input_gain_db  = 0,
    .output_gain_db = 0,
    .dsp_effect     = 0,   // overridden to "channel" in app_main (index is registration order)
    .sd_card = NULL
};

// Ping-Pong Buffers — lock-free dual-buffer flow: while RX fills one buffer the
// DSP task processes the other, and while DSP writes one TX buffer the TX task
// streams the other. Stages hand off by toggling an index (0/1), so no copies
// or locks are needed. Each buffer is one block: PP_FRAMES * AUDIO_CHANNELS int32.
//
// Correctness invariant (the reason "no locks" is safe): each stage has only
// TWO buffers, so a consumer must finish its block within 2 block periods of
// the handoff or the producer starts overwriting a buffer still being read.
// QUEUE_DEPTH beyond 2 adds no elasticity — it only keeps the quiesce/park
// path simple. The invariant is enforced by I2S pacing plus the core-1
// priority ladder; violations show up as garbled audio and the rx "late
// audio block" warnings, never as crashes.
static int32_t s_rx_ping[PP_FRAMES * AUDIO_CHANNELS];
static int32_t s_rx_pong[PP_FRAMES * AUDIO_CHANNELS];
static int32_t s_tx_ping[PP_FRAMES * AUDIO_CHANNELS];
static int32_t s_tx_pong[PP_FRAMES * AUDIO_CHANNELS];
static volatile int s_last_buf_idx = 0;   // last index handed to the TX stage

// Debug view for the CLI `dump` command (see audio_pipeline.h for the caveat).
const int32_t *audio_pipeline_debug_buf(bool rx, int *frames_out)
{
    int idx = s_last_buf_idx;
    if (frames_out) *frames_out = g_geom.pp_frames;
    if (rx) return (idx == 0) ? s_rx_ping : s_rx_pong;
    return (idx == 0) ? s_tx_ping : s_tx_pong;
}

// Runtime block geometry (see common.h). Defaults describe the std-stereo
// path; app_main recomputes from the codec probe before I2S init.
audio_geom_t g_geom = {
    .n_codecs = 1, .rx_ch = 2, .slot_bits = 32, .pp_frames = PP_FRAMES,
    .rx_block_bytes = PP_BYTES, .tx_block_bytes = PP_BYTES, .tdm = false,
};

// ==========================================================
// Local Handles
// ==========================================================
// Block handoff message. CONTRACT: on s_dsp_queue, buf_ptr is the RX buffer to
// process. On s_tx_queue only .index is meaningful — tx_feed_task recomputes
// the TX buffer from it, and buf_ptr still points at the (already recycled) RX
// buffer. Do not read buf_ptr on the TX side.
typedef struct {
    int32_t *buf_ptr;
    int index;
} audio_msg_t;

static QueueHandle_t s_dsp_queue;
static QueueHandle_t s_tx_queue;
static i2s_chan_handle_t s_i2s_tx = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;

// I2S RX DMA overrun counter (issue #12): fires when a received DMA buffer is
// overwritten before rx_fill_task read it — audio silently lost upstream of
// the whole pipeline (recorder ring drop counters can't see it). Registered
// before channel enable; read via audio_pipeline_rx_overruns().
static volatile uint32_t s_rx_overruns = 0;
// Only count while rx_fill is actually consuming: RX is enabled before the
// codec init (whose input precharge takes ~3 s) and stays enabled while the
// pipeline is parked for codec_wake — the queue overflowing then is expected
// and says nothing about audio lost to a running pipeline.
static volatile bool s_rx_consuming = false;

static bool IRAM_ATTR on_rx_queue_overflow(i2s_chan_handle_t handle,
                                           i2s_event_data_t *event, void *ctx)
{
    (void)handle; (void)event; (void)ctx;
    if (s_rx_consuming) s_rx_overruns++;
    return false;   // no higher-priority task woken
}

uint32_t audio_pipeline_rx_overruns(void) { return s_rx_overruns; }

// Input DC/level monitor for the stuck-input guard (audio/input_guard.c).
// The line and mic inputs are AC-coupled, so any DC in the raw ADC data is the
// codec's own: about -0.004 FS on every healthy AIC3204 measured, 0.016 to
// 0.19 FS when its input stage is stuck. rx_fill folds every INSTATS_DECIM-th
// frame of the main codec's two channels into a sum, sum of squares and peak,
// and publishes mean, AC rms and peak every INSTATS_BLOCKS blocks (~0.34 s
// std; 4096 samples per channel per window is plenty for a 0.01 FS decision).
// Cost: ~128 frames x 2 ch x ~20 instructions per block at -Og — about 0.3%
// of the block budget at 160 MHz. No locks: plain volatile words; a torn read
// only mixes two adjacent windows.
#define INSTATS_BLOCKS 32
#define INSTATS_DECIM   4
static volatile int32_t  s_in_mean[2];   // int32 sample units (full scale 2^31)
static volatile int32_t  s_in_rms[2];
static volatile int32_t  s_in_peak[2];   // max |sample|
static volatile uint32_t s_in_seq;

static void input_stats_block(const int32_t *buf)
{
    static int64_t sum[2], sum_r[2], sq[2];   // sum_r / sq in (sample >> 12) units
    static int32_t peak[2];
    static int blocks, frames;
    if (g_geom.tdm && g_geom.slot_bits != 32) return;   // TDM 8x16: not monitored
    const int ch = g_geom.rx_ch, n = g_geom.pp_frames;
    for (int c = 0; c < 2; c++) {
        const int32_t *p = buf + c;
        int64_t s = 0, sr = 0, q = 0;
        int32_t pk = peak[c];
        for (int i = 0; i < n; i += INSTATS_DECIM, p += ch * INSTATS_DECIM) {
            int32_t v = *p, r = v >> 12;
            int32_t a = (v == INT32_MIN) ? INT32_MAX : (v < 0 ? -v : v);
            s  += v;
            sr += r;
            q  += (int64_t)r * r;
            if (a > pk) pk = a;
        }
        sum[c] += s; sum_r[c] += sr; sq[c] += q; peak[c] = pk;
    }
    frames += (n + INSTATS_DECIM - 1) / INSTATS_DECIM;
    if (++blocks < INSTATS_BLOCKS) return;
    for (int c = 0; c < 2; c++) {
        // Variance from the shifted samples on both sides (mixing the true
        // mean with floored squares biases it low by ~mean/4096 LSB^2).
        double mr  = (double)sum_r[c] / frames;
        double var = (double)sq[c] / frames - mr * mr;
        s_in_mean[c] = (int32_t)(sum[c] / frames);
        s_in_rms[c]  = (int32_t)(sqrt(var > 0.0 ? var : 0.0) * 4096.0);
        s_in_peak[c] = peak[c];
        sum[c] = sum_r[c] = sq[c] = 0;
        peak[c] = 0;
    }
    blocks = frames = 0;
    s_in_seq++;
}

#if defined(LINE_IN_LR_SWAPPED) && LINE_IN_LR_SWAPPED
// Swap the main codec's pair (channels 0/1) of one RX block — see
// LINE_IN_LR_SWAPPED in the board header. 512 frames of swaps per block.
static void line_in_swap_lr(int32_t *buf)
{
    const int ch = g_geom.rx_ch, n = g_geom.pp_frames;
    if (!g_geom.tdm || g_geom.slot_bits == 32) {
        for (int i = 0; i < n; i++) {
            int32_t *f = buf + i * ch, t = f[0];
            f[0] = f[1];
            f[1] = t;
        }
    } else {                                   // TDM 8x16: int16 slots
        int16_t *b16 = (int16_t *)buf;
        for (int i = 0; i < n; i++) {
            int16_t *f = b16 + i * ch, t = f[0];
            f[0] = f[1];
            f[1] = t;
        }
    }
}
#endif

bool audio_pipeline_input_stats(int32_t mean[2], int32_t rms[2], int32_t peak[2], uint32_t *seq)
{
    if (g_geom.tdm && g_geom.slot_bits != 32) return false;
    *seq = s_in_seq;
    for (int c = 0; c < 2; c++) {
        mean[c] = s_in_mean[c];
        rms[c]  = s_in_rms[c];
        peak[c] = s_in_peak[c];
    }
    return true;
}

// ----- Audio task configuration -----
// FreeRTOS priorities (higher = more urgent): TX highest so the codec output
// never starves; RX next (it paces the pipeline); DSP below them; LED lowest.
#define TASK_PRIO_TX    12
#define TASK_PRIO_RX    11
#define TASK_PRIO_DSP    9
#define TASK_PRIO_LED    1
#define TASK_PRIO_WEDGE 20   // wedge detector: above every core-0 suspect it watches
#define STACK_AUDIO  4096   // rx/tx/led: I2S transfers + queue messaging only
#define STACK_DSP    8192   // dsp: filter math + delay-line indexing
#define QUEUE_DEPTH     4   // audio blocks buffered between stages (~43 ms at 512/48 kHz)

// Pipeline quiesce for light sleep: each task parks itself at a safe point
// (never inside an I2S call) before the channels are disabled.
#define PIPE_PARKED_RX  BIT0
#define PIPE_PARKED_DSP BIT1
#define PIPE_PARKED_TX  BIT2
#define PIPE_PARKED_LED BIT3
#define PIPE_RESUME     BIT4
#define PIPE_PARKED_ALL (PIPE_PARKED_RX | PIPE_PARKED_DSP | PIPE_PARKED_TX | PIPE_PARKED_LED)
static EventGroupHandle_t s_pipe_eg;
static volatile bool      s_pipe_quiesce = false;

// Park the calling task until resume; returns after PIPE_RESUME is set.
static void pipe_park(EventBits_t parked_bit) {
    xEventGroupSetBits(s_pipe_eg, parked_bit);
    xEventGroupWaitBits(s_pipe_eg, PIPE_RESUME, pdFALSE, pdFALSE, portMAX_DELAY);
}

// ==========================================================
// Pipeline Tasks
// ==========================================================

// Reads I2S (always, to maintain hardware clock) or overlays playback data,
// then pushes buffer pointer to DSP queue.
static void rx_fill_task(void *arg) {
    int  idx         = 0;
    bool was_playing = false;
    int64_t last_loop_us = esp_timer_get_time();
    uint32_t late_blocks = 0;
    uint32_t read_fails  = 0;
    // Late-block threshold: 2× the block period (10.67 ms std, 5.33 ms TDM).
    const int64_t late_us = 2 * ((int64_t)g_geom.pp_frames * 1000000 / FS_HZ);
    ESP_LOGI("rx", "rx task running on core %d", xPortGetCoreID());

    while (1) {
        if (s_pipe_quiesce) {
            s_rx_consuming = false;
            pipe_park(PIPE_PARKED_RX);
            continue;
        }
        s_rx_consuming = true;

        int32_t *buf = (idx == 0) ? s_rx_ping : s_rx_pong;

        // Always read from I2S to keep the hardware clock running and the
        // DMA pipeline moving. This is the master timing source for the
        // entire pipeline regardless of whether we are playing back a file.
        // Always normalize to a full block so downstream DSP and recorder
        // never consume stale tail data after short reads.
        const size_t blk = g_geom.rx_block_bytes;
        size_t got = 0;
        while (got < blk) {
            size_t n = 0;
            if (i2s_channel_read(s_i2s_rx, ((uint8_t *)buf) + got, blk - got,
                                 &n, portMAX_DELAY) != ESP_OK) {
                got = 0;
                break;
            }
            got += n;
        }
        if (got != blk) {
            // A persistently failing RX read would otherwise be a silent
            // full-speed retry loop on core 1 — rate-limit like late_blocks
            // so a dead channel is diagnosable from the console.
            read_fails++;
            if ((read_fails & 0x1F) == 1) {
                ESP_LOGW("rx", "i2s_channel_read failed, count=%u", (unsigned)read_fails);
            }
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        int64_t dt_us = now_us - last_loop_us;
        last_loop_us = now_us;
        if (dt_us > late_us) {
            late_blocks++;
            if ((late_blocks & 0x1F) == 1) {
                ESP_LOGW("rx", "late audio block dt=%lldus count=%u",
                         (long long)dt_us, (unsigned)late_blocks);
            }
        }

#if defined(LINE_IN_LR_SWAPPED) && LINE_IN_LR_SWAPPED
        // LINE IN arrives with left and right swapped on these boards (jack
        // footprint): put them back before every consumer — stats, recorder,
        // DSP — so line-in left is left everywhere (issue #20).
        if (g_state.input_mode == INPUT_LINE_DIFF) line_in_swap_lr(buf);
#endif
        input_stats_block(buf);   // raw input, before any playback overlay

        bool playing = audio_store_is_playing();

        // Flush DSP state on the falling edge of playback so stale PCM
        // doesn't bleed into the effects' live state.
        if (was_playing && !playing) {
            dsp_flush_state();
        }
        was_playing = playing;

        // Pre-DSP capture tap: feed the raw input to the record ring (file
        // recording and/or live stream). The post-DSP tap submits from dsp_task
        // instead — see audio_store_get_record_tap().
        // MUST run BEFORE the playback overlay so "pre" always means the raw
        // physical input (mic/line-in) — this is what makes the line-out→
        // line-in analog loopback measurement possible (play a stimulus while
        // recording the return; 2026-07-08 L/R campaign). Previously a
        // recording made during playback captured the playback data itself.
        if (audio_store_capture_active() &&
            audio_store_get_record_tap() == AUDIO_TAP_PRE) {
            audio_store_record_submit(buf, blk);
        }

        // Overlay playback audio on top of the I2S-clocked buffer slot.
        // Playback streams are stereo-32 (enforced at play_start). In TDM the
        // stereo stream scatters into pair-0 slot positions only — the other
        // pairs keep the live mic audio just read from I2S. Small internal
        // staging keeps the ring drain chunked (PSRAM memcpy only, as before).
        if (playing) {
            if (!g_geom.tdm) {
                audio_store_read_block(buf, blk);
            } else {
                static int32_t stage[64 * 2];              // 512 B chunks
                const int ch = g_geom.rx_ch;
                int n = 0, frames_left = g_geom.pp_frames;
                while (frames_left > 0) {
                    int take = (frames_left > 64) ? 64 : frames_left;
                    audio_store_read_block(stage, (size_t)take * 2 * sizeof(int32_t));
                    if (g_geom.slot_bits == 32) {
                        for (int i = 0; i < take; i++, n++) {
                            buf[n*ch]     = stage[2*i];
                            buf[n*ch + 1] = stage[2*i + 1];
                        }
                    } else {
                        int16_t *b16 = (int16_t *)buf;
                        for (int i = 0; i < take; i++, n++) {
                            b16[n*ch]     = (int16_t)(stage[2*i]     >> 16);
                            b16[n*ch + 1] = (int16_t)(stage[2*i + 1] >> 16);
                        }
                    }
                    frames_left -= take;
                }
            }
        }

        audio_msg_t msg = { .buf_ptr = buf, .index = idx };
        xQueueSend(s_dsp_queue, &msg, portMAX_DELAY);

        idx ^= 1;
    }
}

// Reads from RX queue, processes through DSP, pushes to TX queue
static void dsp_task(void *arg) {
    audio_msg_t rx;
    while (1) {
        if (s_pipe_quiesce) {
            pipe_park(PIPE_PARKED_DSP);
            continue;
        }
        if (xQueueReceive(s_dsp_queue, &rx, pdMS_TO_TICKS(100))) {
            int32_t *out = (rx.index == 0) ? s_tx_ping : s_tx_pong;

            dsp_process(rx.buf_ptr, out, g_geom.pp_frames);

            // Post-DSP capture tap: feed the processed (filtered) output to the
            // record ring (file recording and/or live stream).
            if (audio_store_capture_active() &&
                audio_store_get_record_tap() == AUDIO_TAP_POST) {
                audio_store_record_submit(out, g_geom.tx_block_bytes);
            }

            s_last_buf_idx = rx.index;
            xQueueSend(s_tx_queue, &rx, portMAX_DELAY);
        }
    }
}

// Reads from DSP queue, writes to I2S output
static void tx_feed_task(void *arg) {
    audio_msg_t tx;
    while (1) {
        if (s_pipe_quiesce) {
            pipe_park(PIPE_PARKED_TX);
            continue;
        }
        if (xQueueReceive(s_tx_queue, &tx, pdMS_TO_TICKS(100))) {
            int32_t *buf = (tx.index == 0) ? s_tx_ping : s_tx_pong;
            i2s_channel_write(s_i2s_tx, buf, g_geom.tx_block_bytes, NULL, portMAX_DELAY);
        }
    }
}

// Board startup animation then heartbeat on BLINK_GPIO.
// s_led_beats is the wedge-detector's liveness signal (see wedge_watch_task).
static volatile uint32_t s_led_beats = 0;

static void led_task(void *arg) {
    gpio_config_t led = {
        .pin_bit_mask = (1ULL << BLINK_GPIO),
        .mode         = GPIO_MODE_OUTPUT
    };
    gpio_config(&led);

    rgb_leds_init();          // self-stubs on boards without a strip
    rgb_leds_startup_anim();

    int cnt = 0, state = 0;
    while (1) {
        s_led_beats++;
        if (s_pipe_quiesce) {
            pipe_park(PIPE_PARKED_LED);
            continue;
        }
        if (++cnt >= 5) { cnt = 0; gpio_set_level(BLINK_GPIO, state ^= 1); }
        // Reflect current input/DSP state on the 4-LED status strip (repaints
        // only on change; no-op on Rev 1). Also restores LEDs after sleep,
        // since this task parks during quiesce and resumes here on wake.
        led_status_render();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Wedge detector. Twice now the board has silently hung with USB enumerated
// but unresponsive, the heartbeat frozen, and NO watchdog trip (the wedged
// tasks were blocked, not spinning, so the idle tasks kept feeding the TWDT).
// A silent hang leaves no evidence; a panic leaves a full coredump. This task
// (high priority, core 0 — same core as every suspect) watches led_task's
// loop counter and aborts if it freezes, so the NEXT wedge writes a coredump
// naming every stuck task's exact position. Sleep parks led_task legitimately
// (s_pipe_quiesce), so that state is exempted.
static void wedge_watch_task(void *arg) {
    uint32_t last = 0;
    int      stalled = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t now = s_led_beats;
        if (s_pipe_quiesce)      { stalled = 0; }          // sleeping — parked is normal
        else if (now == last)    { stalled++; }
        else                     { stalled = 0; }
        last = now;
        if (stalled >= 3) {                                // ~6 s frozen
            ESP_LOGE("wedge", "led_task heartbeat frozen ~6s — aborting for coredump");
            abort();   // panic -> coredump partition -> `idf.py coredump-info`
        }
    }
}

// ==========================================================
// Pipeline suspend / resume (light sleep support)
// ==========================================================

esp_err_t audio_pipeline_suspend(void) {
    xEventGroupClearBits(s_pipe_eg, PIPE_PARKED_ALL | PIPE_RESUME);
    s_pipe_quiesce = true;

    // rx returns from i2s_channel_read every ~10ms while the channel is still
    // enabled, dsp/tx poll their queues at 100ms — all four park well inside
    // the timeout. Channels must stay enabled until everyone is parked.
    EventBits_t bits = xEventGroupWaitBits(s_pipe_eg, PIPE_PARKED_ALL,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
    if ((bits & PIPE_PARKED_ALL) != PIPE_PARKED_ALL) {
        ESP_LOGE("pipe", "suspend: tasks did not park (bits=0x%x)", (unsigned)bits);
        s_pipe_quiesce = false;
        xEventGroupSetBits(s_pipe_eg, PIPE_RESUME);
        return ESP_ERR_TIMEOUT;
    }

    // Queues only hold pointers to the static ping/pong buffers — safe to drop.
    xQueueReset(s_dsp_queue);
    xQueueReset(s_tx_queue);

    i2s_channel_disable(s_i2s_tx);
    i2s_channel_disable(s_i2s_rx);
    return ESP_OK;
}

void audio_pipeline_clocks_on(void) {
    i2s_channel_enable(s_i2s_tx);
    i2s_channel_enable(s_i2s_rx);
}

esp_err_t audio_pipeline_resume(void) {
    dsp_flush_state();
    s_pipe_quiesce = false;
    xEventGroupSetBits(s_pipe_eg, PIPE_RESUME);
    return ESP_OK;
}

// ==========================================================
// Internal-RAM watermark (see "Internal RAM budget" in docs/ARCHITECTURE.md)
// ==========================================================
// One line per boot phase + on-demand callers (httpd start, `status`). Largest
// free block is the number that gates late allocations (task stacks need one
// contiguous internal block); min-ever is the lifetime low-water mark.
void heap_checkpoint(const char *phase)
{
    ESP_LOGI("heap", "%s: internal free=%u largest=%u min-ever=%u",
             phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

// ==========================================================
// app_main
// ==========================================================
void app_main(void) {
    // 1. Hardware Initialization
    ESP_ERROR_CHECK(hal_spi_init());
    hal_buttons_init();
    codec_init_spi();
    // Read the codec's registers before anything touches it: after an MCU-only
    // reset they still hold the pre-restart state (`codec snapshot`).
    codec_snapshot_boot();
    // Expansion-codec presence decides the I2S geometry (std stereo vs TDM),
    // so probe before the I2S config below. SPI-only — no MCLK needed yet.
    codec_probe_all();

    // Block geometry from the probe. Slot width: auto = 32-bit up to 2
    // codecs; the S3's 128-bit TDM frame cap (I2S_LL_SLOT_FRAME_BIT_MAX)
    // forces 16-bit slots with 3 codecs. `settings slotwidth 16` halves the
    // RX bandwidth with one expansion; a forced 32 cannot beat the frame cap.
    {
        int n = codec_count();
        uint8_t sw = settings_get_slot_width_early();   // 0 = auto
        if (n > 1) {
            int slot_bits = (n >= 3) ? 16 : 32;
            if (sw == 16) slot_bits = 16;
            if (sw == 32 && n >= 3) {
                ESP_LOGW("main", "slotwidth 32 requested but 3 codecs need 16-bit slots (128-bit TDM frame cap) — using 16");
            }
            g_geom.n_codecs       = n;
            g_geom.rx_ch          = 2 * n;
            g_geom.slot_bits      = slot_bits;
            g_geom.pp_frames      = 256;
            g_geom.rx_block_bytes = (size_t)g_geom.pp_frames * g_geom.rx_ch * (g_geom.slot_bits / 8);
            g_geom.tx_block_bytes = (size_t)g_geom.pp_frames * 2 * (g_geom.slot_bits / 8);
            g_geom.tdm            = true;
        }
        ESP_LOGI("main", "audio geometry: %s, %d codec(s), %d ch RX, %d-bit slots, %d frames/block",
                 g_geom.tdm ? "TDM" : "std I2S", g_geom.n_codecs, g_geom.rx_ch,
                 g_geom.slot_bits, g_geom.pp_frames);
        // The RAM-ledger invariant that keeps every geometry inside the fixed
        // 4 KB ping-pong buffers (docs/ARCHITECTURE.md): pp_frames above is
        // hand-picked to satisfy it, and anyone tuning frames/channels/slot
        // width must hit this tripwire instead of silently overflowing the
        // static buffers.
        assert(g_geom.rx_block_bytes <= PP_BYTES && g_geom.tx_block_bytes <= PP_BYTES);
    }
    // 1b. Storage + CLI command registration
    mount_sdcard();

    ESP_ERROR_CHECK(audio_store_init());
    heap_checkpoint("storage-up");   // SD mount + rec DMA staging (REC_DMA_BYTES, adaptive) done
    register_audio_store_cmds();
    register_sd_diag_cmd();
    register_sd_format_cmd();
    register_sd_remount_cmd();
    register_serial_xfer_cmd();
    register_wifi_cmd();
    register_sleep_cmd();
    register_settings_cmd();
    register_pot_cmd();

    // NOTE: the old GPIO0 hold-to-record binding is retired — SW2 (GPIO0) is
    // now the front-panel shift key (issue #8). Recording remains available
    // via the `rec` CLI command, the web UI, and the HTTP API.

    // 2. I2S Configuration.
    // DMA sizing: dma_frame_num is scaled so bytes-per-descriptor stay at the
    // std baseline (240 frames × 8 B = 1920 B) in every mode — TDM must not
    // grow the ~23 KB I2S DMA entry in the internal-RAM ledger
    // (docs/ARCHITECTURE.md):
    //   TDM 4×32: RX frame = 16 B active → 120 frames; TDM 8×16: 12 B → 160.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (g_geom.tdm) {
        chan_cfg.dma_frame_num = (g_geom.slot_bits == 16) ? 160 : 120;
    }
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx));

    if (!g_geom.tdm) {
        i2s_std_config_t std_cfg = {
            .clk_cfg = {
                .sample_rate_hz = FS_HZ,
                .clk_src        = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple  = I2S_MCLK_MULTIPLE_256
            },
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(AUDIO_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_MCLK_PIN,
                .bclk = I2S_BCLK_PIN,
                .ws   = I2S_LRC_PIN,
                .dout = I2S_DOUT_PIN,
                .din  = I2S_DIN_PIN
            }
        };
        std_cfg.slot_cfg.slot_bit_width = AUDIO_BITS_PER_SAMPLE;
        std_cfg.slot_cfg.data_bit_width = AUDIO_BITS_PER_SAMPLE;
        std_cfg.slot_cfg.ws_width       = AUDIO_BITS_PER_SAMPLE;

        // Fail-fast like the TDM branch below — a failed std init is exactly
        // as boot-fatal, and cascading past it produces confusing symptoms.
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg));
    } else {
        // TDM, frame fixed at the S3's 128-bit cap → BCLK 6.144 MHz.
        // MCLK_MULTIPLE_512 (24.576 MHz) gives the integer bclk_div 4 the
        // driver requires for master TDM RX (div ≤ 2 corrupts multi-slot RX;
        // 256× would be silently bumped). PCM-short framing (1-BCLK WS pulse)
        // matches the AIC3204 DSP interface mode.
        i2s_tdm_config_t tdm_cfg = {
            .clk_cfg = {
                .sample_rate_hz = FS_HZ,
                .clk_src        = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple  = I2S_MCLK_MULTIPLE_512,
                // Slave-only field, ignored as master — the value here is
                // arbitrary. The master-mode divider (the required integer 4)
                // is derived from mclk_multiple above, not from this field.
                .bclk_div       = 8,
            },
            .slot_cfg = I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(
                            (i2s_data_bit_width_t)g_geom.slot_bits,
                            I2S_SLOT_MODE_STEREO,
                            (i2s_tdm_slot_mask_t)((1u << g_geom.rx_ch) - 1)),
            .gpio_cfg = {
                .mclk = I2S_MCLK_PIN,
                .bclk = I2S_BCLK_PIN,
                .ws   = I2S_LRC_PIN,
                .dout = I2S_DOUT_PIN,
                .din  = I2S_DIN_PIN
            }
        };
        tdm_cfg.slot_cfg.slot_bit_width = (i2s_slot_bit_width_t)g_geom.slot_bits;
        tdm_cfg.slot_cfg.total_slot     = 128 / g_geom.slot_bits;   // 4×32 or 8×16

        // Init order sets the clock roles. Since IDF v5.5.2 the channel
        // initialized second becomes the full-duplex slave (earlier IDF always
        // made RX the slave). TX goes first so it stays the BCLK/WS/MCLK
        // master, the configuration the TDM bench bring-up measured. The std
        // branch above already inits TX first.
        const i2s_tdm_slot_mask_t rx_slot_mask = tdm_cfg.slot_cfg.slot_mask;
        // TX: main-codec slots only. The DMA buffer stays stereo-interleaved
        // (driver sizes DMA by active slots; hardware zero-fills idle slots).
        tdm_cfg.slot_cfg.slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1);
        ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(s_i2s_tx, &tdm_cfg));
        // RX: one slot pair per codec, packed low (codecs' R28 offsets match).
        tdm_cfg.slot_cfg.slot_mask = rx_slot_mask;
        ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(s_i2s_rx, &tdm_cfg));
    }
    // Overrun visibility (issue #12) — must register before channel enable.
    const i2s_event_callbacks_t rx_cbs = { .on_recv_q_ovf = on_rx_queue_overflow };
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_i2s_rx, &rx_cbs, NULL));
    i2s_channel_enable(s_i2s_tx);
    i2s_channel_enable(s_i2s_rx);
    codec_init_all(g_geom.tdm, g_geom.slot_bits);
    heap_checkpoint("audio-up");     // I2S DMA descriptors/buffers allocated

    // 3. DSP Initialization (effects seed their own default designs in
    // rs_user_effects_init, called from inside dsp_init).
    dsp_init();
    dsp_set(dsp_find("channel"));   // default effect (its preset is in channel_fx.c)

    // Apply the saved power-on default (if any) over the compiled-in settings.
    // Needs codec + DSP up (it programs both); before control_buttons_init so
    // the button cursor syncs to the loaded effect at init.
    ESP_ERROR_CHECK(settings_store_init());

    // 4. WiFi + HTTP control panel — runs on Core 0, non-blocking. WiFi
    // lifecycle is wifi_manager.c (it starts web_server.c's httpd once the
    // radio is up). Brownout mitigation lives in wifi_manager.c: boot settle
    // delay, TX-power cap (`wifi power`, default 8.5 dBm), association-retry
    // backoff — plus the CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=13 build ceiling in
    // sdkconfig.defaults.
    wifi_manager_init();

    // 5. Tasks — project task/core map (2026-07 priority/affinity pass):
    //
    //   CORE 1 (audio only — nothing else may be pinned or float here):
    //     tx_feed 12 | rx_fill 11 | dsp 9
    //   CORE 0 (everything else):
    //     wifi driver 23 (IDF) | wedge 20 | lwIP tcpip 18 (pinned via
    //     sdkconfig) | TinyUSB device 12 (cli_console.c) | as_writer 10 |
    //     as_reader 9 | rec_stop/rec_live/httpd/restart 5 | console 5 |
    //     ctrl_btn 4 | wifi_init 3 | in_guard 2 (4 KB, input_guard.c) |
    //     led 1 | timer-svc 1 (pinned via sdkconfig)
    //
    //   Floating tasks are forbidden: an unpinned task can migrate onto core 1
    //   and steal audio headroom (lwIP at prio 18 would preempt the whole
    //   pipeline). New xTaskCreate calls must be xTaskCreatePinnedToCore(.., 0).
    s_pipe_eg = xEventGroupCreate();
    s_dsp_queue = xQueueCreate(QUEUE_DEPTH, sizeof(audio_msg_t));
    s_tx_queue  = xQueueCreate(QUEUE_DEPTH, sizeof(audio_msg_t));

    xTaskCreatePinnedToCore(tx_feed_task, "tx",  STACK_AUDIO, NULL, TASK_PRIO_TX,  NULL, 1);
    xTaskCreatePinnedToCore(rx_fill_task, "rx",  STACK_AUDIO, NULL, TASK_PRIO_RX,  NULL, 1);
    xTaskCreatePinnedToCore(dsp_task,     "dsp", STACK_DSP,   NULL, TASK_PRIO_DSP, NULL, 1);
    xTaskCreatePinnedToCore(led_task,     "led", STACK_AUDIO, NULL, TASK_PRIO_LED, NULL, 0);
    xTaskCreatePinnedToCore(wedge_watch_task, "wedge", 2560, NULL, TASK_PRIO_WEDGE, NULL, 0);
    input_guard_start();   // stuck-input backstop; reads rx_fill's input statistics

    // Sleep enter/wake sequencing (trigger = SW6 long-hold in control_buttons;
    // Rev 2; no-op on Rev 1).
    ESP_ERROR_CHECK(power_sleep_init());

    // Analog pot (Rev 3, IO1 = ADC1_CH0). Claims the ADC1 oneshot unit — which
    // the `adc` CLI command then borrows rather than opening a second handle.
    // Must run before control_buttons_init(): its poll rides the ctrl_btn task.
    ESP_ERROR_CHECK(control_pot_init());

    // Front-panel buttons: SW2 shift layer — feature/sub-mode select, bypass,
    // input select, in/out gain steps, SW6 sleep hold (Rev 2; no-op on Rev 1).
    // Its 20 ms loop also polls the pot (control_pot_poll).
    ESP_ERROR_CHECK(control_buttons_init());

    start_console_task();
    heap_checkpoint("app_main-done");  // TinyUSB + WiFi/httpd log their own phases
}
