// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

// Unified audio storage with streaming capture.
//
// Recording is a single-producer / multi-consumer PSRAM ring. The audio hot path
// appends blocks via audio_store_record_submit(); two consumers drain the ring
// through independent cursors:
//
//   STORAGE — a continuous writer task → SD WAV file (unbounded, limited only by
//             card space) or the "audio_raw" flash partition (no-SD fallback,
//             ~38 s). Guaranteed: the producer never overwrites unstored data.
//   LIVE    — audio_store_live_read(), used by the /api/rec/live HTTP endpoint to
//             stream to a notebook over Wi-Fi. Best-effort: if the network lags,
//             the oldest unsent audio is dropped rather than stalling capture.
//
//   AUDIO_BACKEND_SD    — WAV file on SD card (/sdcard/*.wav), streamed live.
//   AUDIO_BACKEND_FLASH — raw "audio_raw" partition, streamed live (fallback).
//
// The record tap (pre/post-DSP) selects what is captured; see audio_record_tap_t.
//
// Hot-path integration (rx_fill_task = pre-DSP, dsp_task = post-DSP; block
// sizes come from the runtime geometry, not the std-stereo constants):
//
//   if (audio_store_capture_active() && audio_store_get_record_tap() == AUDIO_TAP_PRE)
//       audio_store_record_submit(rx_buf, g_geom.rx_block_bytes);   // rx_fill_task
//   ...
//   if (audio_store_capture_active() && audio_store_get_record_tap() == AUDIO_TAP_POST)
//       audio_store_record_submit(dsp_out, g_geom.tx_block_bytes);  // dsp_task, after dsp_process

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Backend selector
// ---------------------------------------------------------------------------

typedef enum {
    AUDIO_BACKEND_FLASH = 0,  // raw partition "audio_raw", ~38 s, direct I/O
    AUDIO_BACKEND_SD    = 1,  // WAV file on SD card, PSRAM-buffered
} audio_backend_t;

// ---------------------------------------------------------------------------
// Record tap — where in the audio chain the recorder samples from
// ---------------------------------------------------------------------------

typedef enum {
    AUDIO_TAP_PRE  = 0,  // raw I2S input, before DSP (default; legacy behavior)
    AUDIO_TAP_POST = 1,  // processed output, after dsp_process (filtered signal)
} audio_record_tap_t;

// ---------------------------------------------------------------------------
// Record source — who feeds the ring for this take
// ---------------------------------------------------------------------------

typedef enum {
    AUDIO_REC_SRC_AUDIO = 0,  // audio hot path submits (normal recording)
    AUDIO_REC_SRC_EXTERNAL,   // an uploader (HTTP/serial) submits; the hot path
                              // MUST stay out or live audio interleaves with
                              // the uploaded bytes (capture_active excludes it)
} audio_record_source_t;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

/**
 * @brief Initialize both backends (find flash partition, allocate PSRAM buffers,
 *        spawn writer/reader tasks). Call once at startup.
 */
esp_err_t audio_store_init(void);

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

/**
 * @brief Start streaming recording to storage.
 *
 * SD:    opens the WAV file, writes a placeholder header, and a writer task
 *        streams the record ring to disk continuously (unbounded — limited only
 *        by free space). Returns ESP_ERR_NOT_FOUND if no card is mounted.
 * FLASH: pre-erases the whole capture region (blocks for several seconds — the
 *        full 13.9 MiB partition on the 16 MB layout; issue #12 — an erase
 *        mid-take stalls both cores and splices the audio), then streams to
 *        the "audio_raw" partition (no-SD fallback, ~38 s).
 *
 * Claims the recorder atomically — concurrent starts from CLI/HTTP/serial get
 * ESP_ERR_INVALID_STATE.
 *
 * @param backend    AUDIO_BACKEND_SD or AUDIO_BACKEND_FLASH
 * @param sd_path    SD only — output WAV path. NULL → "/sdcard/rec.wav"
 * @param sample_rate        Hz (48000 in every current geometry)
 * @param bits_per_sample    For AUDIO-source takes these two are ADVISORY:
 * @param channels           record_start overrides them with the latched tap's
 *                           TDM-aware format (pre = rx_ch × slot width, post =
 *                           stereo mixdown) so a WAV header can never mislabel
 *                           the ring data. EXTERNAL takes use them verbatim.
 * @param source     AUDIO_REC_SRC_AUDIO (hot path feeds the take) or
 *                   AUDIO_REC_SRC_EXTERNAL (uploader feeds it; refused while
 *                   live capture is active — the two would share the ring)
 */
esp_err_t audio_store_record_start(audio_backend_t backend,
                                   const char     *sd_path,
                                   uint32_t        sample_rate,
                                   uint16_t        bits_per_sample,
                                   uint16_t        channels,
                                   audio_record_source_t source);

/**
 * @brief Append one PCM block to the record ring. Call from the audio hot path
 *        (rx_fill_task / dsp_task), never an ISR. No file/flash/network I/O here.
 *        A no-op unless storage recording or live capture is active.
 */
void audio_store_record_submit(const void *data, size_t len);

/**
 * @brief Stop storage recording, drain the ring, and finalize (patch the WAV
 *        header / write flash metadata). Blocks until the writer is done.
 *        Live capture, if active, continues.
 */
void audio_store_record_stop(void);

/** @brief True if storage recording is active (any source). */
bool audio_store_is_recording(void);

/**
 * @brief True if the AUDIO HOT PATH should submit blocks: an audio-source
 *        storage take or live capture is active. False during external
 *        (upload) takes — the uploader owns the ring then.
 */
bool audio_store_capture_active(void);

/**
 * @brief Select where the recorder samples audio from. Takes effect at the next
 *        audio_store_record_start() — the tap is latched for the duration of a
 *        take so it cannot change mid-recording. Default: AUDIO_TAP_PRE.
 *
 *   AUDIO_TAP_PRE  — raw I2S input (submitted from rx_fill_task before DSP).
 *   AUDIO_TAP_POST — processed output (submitted from dsp_task after dsp_process).
 */
void audio_store_set_record_tap(audio_record_tap_t tap);

/**
 * @brief Get the record tap. While recording, returns the tap latched for the
 *        active take (what the hot path uses); otherwise the configured tap that
 *        the next recording will use.
 */
audio_record_tap_t audio_store_get_record_tap(void);

// ---------------------------------------------------------------------------
// Live capture — best-effort second consumer of the record ring
// ---------------------------------------------------------------------------
//
// Used by the /api/rec/live HTTP endpoint to stream captured audio (at the
// configured tap) to a host over Wi-Fi. Independent of storage recording: live
// can run with or without an SD card and with or without a file recording.

/** @brief Arm live capture from "now". Latches the tap if no capture was active. */
esp_err_t audio_store_live_open(void);

/** @brief Disarm live capture. */
void audio_store_live_close(void);

/** @brief True if live capture is armed. */
bool audio_store_live_active(void);

/**
 * @brief I2S RX overruns since live_open — loss UPSTREAM of the ring (issue
 *        #12 class); nonzero means the live stream has splice gaps. Transports
 *        should report this when the stream ends (live has no finalize step,
 *        so the storage-take warning never fires for it).
 */
uint32_t audio_store_live_overruns(void);

/**
 * @brief Read up to `want` bytes of live PCM into dst. Blocks up to timeout_ms.
 * @return bytes copied (>0), or 0 on timeout / not active.
 */
size_t audio_store_live_read(void *dst, size_t want, int timeout_ms);

/** @brief Fill a 44-byte buffer (a full wav_header_t) with a streaming WAV
 *         header (open-ended size). */
void audio_store_live_wav_header(void *dst44);

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

/**
 * @brief Start playback. Both backends prefill the shared 512 KB PSRAM ring
 *        from the Core-0 reader task (SD: fread; FLASH: esp_partition_read);
 *        the hot path only ever drains the ring (audio_store_read_block).
 *        Playback contract: 48 kHz / stereo / 32-bit sources only (enforced).
 *
 * @param backend    AUDIO_BACKEND_FLASH or AUDIO_BACKEND_SD
 * @param sd_path    SD only — WAV file path. NULL → "/sdcard/rec.wav"
 * @param loop       Restart at end-of-sample/file instead of stopping
 */
esp_err_t audio_store_play_start(audio_backend_t backend,
                                 const char     *sd_path,
                                 bool            loop);

/** @brief Stop playback immediately. */
void audio_store_play_stop(void);

/** @brief True if playback is active (call from rx_fill_task or console). */
bool audio_store_is_playing(void);

/**
 * @brief Fill dst with exactly len bytes of playback PCM.
 * Zero-fills on EOF (non-loop) or when not playing.
 * Call from rx_fill_task only.
 */
void audio_store_read_block(void *dst, size_t len);

// ---------------------------------------------------------------------------
// Flash WAV export
// ---------------------------------------------------------------------------

/** Callback type for audio_store_flash_write_wav. */
typedef esp_err_t (*audio_store_write_fn_t)(const void *data, size_t len, void *ctx);

/**
 * @brief Stream the flash sample as a complete WAV (header + PCM) via a caller-supplied
 *        write callback.  Returns ESP_ERR_NOT_FOUND if no valid sample is stored.
 *        Reads PCM in 4 KB chunks — safe to call from any task.
 */
esp_err_t audio_store_flash_write_wav(audio_store_write_fn_t write_fn, void *ctx);

/**
 * @brief Total size in bytes of the flash sample as a WAV (44-byte header + PCM),
 *        or 0 if no valid sample is stored. Used to announce size before streaming.
 */
size_t audio_store_flash_wav_size(void);

// ---------------------------------------------------------------------------
// Console registration
// ---------------------------------------------------------------------------

/** @brief Register "rec" and "play" console commands. */
void register_audio_store_cmds(void);

/** @brief Register "sd_diag" throughput diagnostic command (SD card only). */
void register_sd_diag_cmd(void);

#ifdef __cplusplus
}
#endif
