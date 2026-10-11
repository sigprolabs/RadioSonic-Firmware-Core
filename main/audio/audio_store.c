// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// Unified audio storage — streaming record + playback, two backends, one hot
// path on each side.
//
// RECORD (both backends feed the SAME 1 MB PSRAM record ring, s_rec_ring):
//   hot path → audio_store_record_submit() appends at the head; the STORAGE
//   cursor is drained CONTINUOUSLY by rec_writer_task (Core 0, pri 10) through
//   internal DMA staging to the sink — SD: POSIX write() into an unbounded WAV
//   whose placeholder header is patched at stop; FLASH: lazy 64 KB erase +
//   esp_partition_write. A second best-effort LIVE cursor feeds HTTP/serial
//   streaming. Full cursor semantics: the "record ring" comment below.
//
// PLAYBACK (both backends share one 512 KB PSRAM ring + one reader task):
//   pb_reader_task (Core 0, pri 9) prefills the ring — SD: fread from the WAV;
//   FLASH: esp_partition_read — and audio_store_read_block() drains it.
//
// The audio-task hot paths (submit/read_block) are PSRAM memcpy only — no
// flash or SD I/O ever happens on core 1.
//
// Web/serial upload note:
//   h_upload_flash (web_server.c) and xfer put flash (serial_xfer.c) start takes
//   with AUDIO_REC_SRC_EXTERNAL: audio_store_capture_active() is false then, so
//   the audio hot path stays out of the ring and uploaded bytes can't interleave
//   with live audio. record_start claims the recorder atomically (s_rec_starting
//   under s_rec_lock), so concurrent starts from CLI/HTTP/serial are safe.
//   External takes and live capture are mutually exclusive — both would feed
//   the same ring.

#include "audio_store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"

#include "esp_console.h"
#include "argtable3/argtable3.h"

#include "wav_util.h"
#include "common.h"
#include "audio_pipeline.h"

// ---------------------------------------------------------------------------
// Logging tags
// ---------------------------------------------------------------------------

static const char *TAG     = "audio_store";

// ---------------------------------------------------------------------------
// Flash backend — partition geometry
// ---------------------------------------------------------------------------

#define PARTITION_LABEL  "audio_raw"
#define META_OFFSET      0u
#define PCM_OFFSET       64u        // >= sizeof(sample_meta_t), 4-byte aligned
#define META_MAGIC       0xA10DF00Du

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t channels;
    uint32_t pcm_bytes;
    uint8_t  _pad[48];   // pad to 64 bytes (PCM_OFFSET)
} sample_meta_t;
// PCM data starts at PCM_OFFSET; the metadata header must never grow past it.
_Static_assert(sizeof(sample_meta_t) == PCM_OFFSET,
               "sample_meta_t must fill exactly PCM_OFFSET bytes");

// ---------------------------------------------------------------------------
// Buffer / ring sizes
// ---------------------------------------------------------------------------

// Streaming record ring (PSRAM). Single producer (audio hot path); two
// consumers read it via independent cursors: STORAGE (writer task, guaranteed)
// and LIVE (HTTP, best-effort/droppable). See "record ring" section below.
#define REC_RING_BYTES    (1u * 1024u * 1024u)   // ~2.7 s of slack at 384 KB/s
// Writer DMA staging: internal RAM is the scarce resource (see the RAM budget
// in docs/ARCHITECTURE.md; 32 KB here contributed to httpd's stack allocation
// failing). 16 KB chunks still write far above the 384 KB/s requirement on
// SDMMC, and the alloc loop below halves further on a fragmented heap.
#define REC_DMA_BYTES     (16u * 1024u)          // writer DMA staging (adaptive)
#define REC_FLASH_ERASE   (64u * 1024u)          // flash erase granule (mult. of 4 KB)

// Shared playback ring (PSRAM) — used by BOTH flash and SD backends
#define PB_RING_BYTES  (512u * 1024u)
#define PB_CHUNK_BYTES (32u  * 1024u)   // reader prefill chunk
#define PB_LOW_WM      PB_CHUNK_BYTES   // wake reader when ring drops below this

// SD diagnostic
#define DIAG_STAGING_BYTES   (64u * 1024u)
#define DIAG_SECTOR_SIZE     512u
#define DIAG_END_MARGIN_SECS 2048u

// Sustained streaming test file size (per file). Sized down to a fraction of SD
// free space so the duplex phase (two files at once) leaves headroom.
#define DIAG_STREAM_MAX      (16u * 1024u * 1024u)
#define DIAG_STREAM_MIN      (1u  * 1024u * 1024u)
#define DIAG_RATE_FLOOR_KBPS 375.0f   // 48 kHz stereo 32-bit

// ---------------------------------------------------------------------------
// State — active backend tracking
// ---------------------------------------------------------------------------

static volatile bool   s_play_active  = false;
static audio_backend_t s_rec_backend  = AUDIO_BACKEND_FLASH;
static audio_backend_t s_play_backend = AUDIO_BACKEND_FLASH;

// Record tap: s_record_tap is the configured tap for the next recording;
// s_rec_tap_active is latched at record_start and read by the audio hot path.
static audio_record_tap_t s_record_tap     = AUDIO_TAP_PRE;
static audio_record_tap_t s_rec_tap_active = AUDIO_TAP_PRE;

// Who feeds the ring for the active storage take (latched at record_start).
// EXTERNAL (upload) takes exclude the audio hot path via capture_active().
static audio_record_source_t s_rec_source  = AUDIO_REC_SRC_AUDIO;

// ---------------------------------------------------------------------------
// State — flash backend: recording
// ---------------------------------------------------------------------------

static const esp_partition_t *s_part         = NULL;
static uint32_t               s_fl_rec_sr    = 0;
static uint16_t               s_fl_rec_bits  = 0;
static uint16_t               s_fl_rec_ch    = 0;

// ---------------------------------------------------------------------------
// State — flash backend: playback (owned by pb_reader_task)
// ---------------------------------------------------------------------------

static uint32_t s_fl_play_cur   = 0;   // read cursor, modified only by reader task
static uint32_t s_fl_play_total = 0;   // total PCM bytes in partition

// ---------------------------------------------------------------------------
// State — streaming record ring (PSRAM, one producer, multiple cursors)
// ---------------------------------------------------------------------------
//
// Positions are absolute byte counts (monotonic, never wrap in practice); the
// ring slot is pos % REC_RING_BYTES. The producer (audio_store_record_submit,
// hot path) only appends at s_rec_head. Each consumer owns its own tail:
//
//   STORAGE (writer task) — guaranteed: producer never overwrites unstored data
//                           (s_rec_head - s_rec_store_tail <= REC_RING_BYTES).
//   LIVE    (HTTP)        — best-effort: if it lags out of the window the
//                           producer snaps its tail forward (drops oldest),
//                           never back-pressuring the hot path or storage.
//
// All cursor reads/updates happen under s_rec_lock; the bulk memcpy into/out of
// the ring happens outside the lock (matching the playback reader pattern).

static uint8_t          *s_rec_ring       = NULL;
static SemaphoreHandle_t s_rec_lock       = NULL;
static volatile uint64_t s_rec_head       = 0;   // bytes produced (producer)
static volatile uint64_t s_rec_store_tail = 0;   // bytes drained by writer
static volatile uint64_t s_rec_live_tail  = 0;   // bytes drained by live HTTP

static volatile bool     s_rec_storing    = false; // storage consumer armed
static volatile bool     s_rec_live_on    = false; // live consumer armed
static uint64_t          s_rec_store_end  = 0;   // frozen head at record_stop
static uint64_t          s_rec_store_start= 0;   // head at record_start
static uint64_t          s_rec_store_limit= 0;   // max bytes for this take
static uint32_t          s_dropped_blocks = 0;   // storage overflow (writer behind)
static uint32_t          s_live_drops     = 0;   // live frames skipped (network behind)
static uint32_t          s_rec_ovf_start  = 0;   // I2S RX overrun count at record_start
static uint32_t          s_live_ovf_start = 0;   // I2S RX overrun count at live_open
static volatile bool     s_rec_starting   = false; // record_start claim (TOCTOU guard)

static uint32_t          s_sample_rate    = FS_HZ;
static uint16_t          s_bits           = AUDIO_BITS_PER_SAMPLE;
static uint16_t          s_channels       = AUDIO_CHANNELS;
static char              s_rec_path[128];

// Writer task + sink state (owned by the writer task)
static TaskHandle_t      s_writer_task     = NULL;
static SemaphoreHandle_t s_finish_sem      = NULL;
static volatile bool     s_writer_armed    = false;
static uint8_t          *s_rec_dma         = NULL;  // internal DMA staging
static size_t            s_rec_dma_sz      = 0;
static int               s_sink_fd         = -1;    // SD backend
static uint32_t          s_fl_write_off    = 0;     // flash PCM write cursor
static uint32_t          s_fl_erased_to    = 0;     // flash erased-up-to offset

// ---------------------------------------------------------------------------
// State — shared playback ring (PSRAM, used by both backends)
// ---------------------------------------------------------------------------

static uint8_t          *s_pb_ring       = NULL;
static size_t            s_pb_head       = 0;    // writer (reader task)
static size_t            s_pb_tail       = 0;    // reader (hot path)
static size_t            s_pb_used       = 0;    // bytes available to hot path
static SemaphoreHandle_t s_pb_lock       = NULL;
static TaskHandle_t      s_pb_task       = NULL;
static volatile bool     s_pb_eof        = false;
static bool              s_pb_loop       = false;

// SD playback file state. OWNERSHIP: only pb_reader_task touches the FILE*
// after play_start hands it over — including the close. play_stop() requests
// teardown (s_pb_stop_req) and waits for s_pb_stopped_sem instead of closing
// s_pb_fp itself: an fclose under a caller's feet while the reader is mid-
// fread (which deliberately runs OUTSIDE s_pb_lock so the audio hot path is
// never blocked behind SD I/O) would be a use-after-free.
static FILE             *s_pb_fp         = NULL;
static long              s_pb_data_off   = 0;
static uint32_t          s_pb_data_total = 0;
static uint32_t          s_pb_file_pos   = 0;
static volatile bool     s_pb_stop_req   = false; // play_stop -> reader teardown request
static SemaphoreHandle_t s_pb_stopped_sem = NULL; // reader -> play_stop: teardown done
static SemaphoreHandle_t s_pb_stop_mutex  = NULL; // serializes concurrent play_stops

// ---------------------------------------------------------------------------
// Streaming writer task — continuously drains the STORAGE cursor to the sink
// (SD WAV file or flash partition) while recording, then finalizes at stop.
// ---------------------------------------------------------------------------

static void flash_write_metadata(uint32_t pcm_bytes);  // defined below

// Copy n bytes out of the record ring starting at absolute position `pos`.
static void rec_ring_read(uint8_t *dst, uint64_t pos, size_t n)
{
    size_t off = (size_t)(pos % REC_RING_BYTES);
    size_t c1  = REC_RING_BYTES - off;
    if (c1 >= n) {
        memcpy(dst, s_rec_ring + off, n);
    } else {
        memcpy(dst,      s_rec_ring + off, c1);
        memcpy(dst + c1, s_rec_ring,       n - c1);
    }
}

// Pre-erase the full flash capture region — MUST complete before capture arms
// (issue #12). An esp_partition_erase_range during capture suspends the flash
// cache, stalling non-IRAM code on BOTH cores for the ~tens of ms each 64 KB
// block erase takes: rx_fill can't drain the I2S RX DMA queue, the queue
// overruns, and audio is lost UPSTREAM of the record ring — invisible to the
// dropped counter (verified on hardware 2026-07-07: ~1/3 loss, ~5.9 erases/s
// at 384 KB/s). With the region erased up front, the mid-take sink writes are
// page programs only (sub-ms stalls the DMA queue absorbs). Blocks that
// already read all-0xFF are skipped, so a take only re-erases what previous
// data dirtied. Live audio may stutter during the pre-erase itself — capture
// has not started yet, so nothing is lost.
static esp_err_t flash_pre_erase(void)
{
    int64_t  t0     = esp_timer_get_time();
    uint32_t erased = 0;

    for (uint32_t off = 0; off < s_part->size; off += REC_FLASH_ERASE) {
        // Blank-check the block (borrows the DMA staging buffer — the writer
        // task is idle until capture arms, which is after this returns).
        bool dirty = false;
        for (uint32_t o = 0; o < REC_FLASH_ERASE && !dirty; o += s_rec_dma_sz) {
            size_t n = REC_FLASH_ERASE - o;
            if (n > s_rec_dma_sz) n = s_rec_dma_sz;
            esp_err_t e = esp_partition_read(s_part, off + o, s_rec_dma, n);
            if (e != ESP_OK) return e;
            const uint32_t *w = (const uint32_t *)s_rec_dma;
            for (size_t i = 0; i < n / sizeof(uint32_t); i++) {
                if (w[i] != 0xFFFFFFFFu) { dirty = true; break; }
            }
        }
        if (dirty) {
            esp_err_t e = esp_partition_erase_range(s_part, off, REC_FLASH_ERASE);
            if (e != ESP_OK) return e;
            erased++;
        }
        vTaskDelay(1);   // let idle/housekeeping run between blocks
    }

    // rec_sink_write's lazy-erase loop is now a dead safety net for this take.
    s_fl_erased_to = s_part->size;
    ESP_LOGI(TAG, "flash pre-erase: %u/%u blocks in %d ms",
             (unsigned)erased, (unsigned)(s_part->size / REC_FLASH_ERASE),
             (int)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

// Write n PCM bytes to the active sink (from the internal DMA staging buffer).
static esp_err_t rec_sink_write(const uint8_t *p, size_t n)
{
    if (s_rec_backend == AUDIO_BACKEND_FLASH) {
        // Safety net only: flash_pre_erase() ran at record_start, so this loop
        // never executes. It must NEVER run mid-capture — erases here stall
        // the cores and splice the audio upstream of the ring (issue #12).
        uint32_t need = PCM_OFFSET + s_fl_write_off + (uint32_t)n;
        while (s_fl_erased_to < need) {
            ESP_LOGW(TAG, "unexpected mid-capture erase at 0x%X — expect audio gaps",
                     (unsigned)s_fl_erased_to);
            esp_err_t e = esp_partition_erase_range(s_part, s_fl_erased_to, REC_FLASH_ERASE);
            if (e != ESP_OK) return e;
            s_fl_erased_to += REC_FLASH_ERASE;
        }
        esp_err_t e = esp_partition_write(s_part, PCM_OFFSET + s_fl_write_off, p, n);
        if (e == ESP_OK) s_fl_write_off += (uint32_t)n;
        return e;
    }
    // SD: POSIX write straight from the DMA staging buffer.
    return (write(s_sink_fd, p, n) == (ssize_t)n) ? ESP_OK : ESP_FAIL;
}

static void rec_sink_finalize(uint64_t pcm_bytes, bool ok)
{
    float secs = (s_sample_rate && s_channels && s_bits)
                 ? (float)pcm_bytes / (float)(s_sample_rate * s_channels * (s_bits / 8)) : 0.0f;

    // Integrity check (issue #12): I2S RX overruns during the take mean audio
    // was lost UPSTREAM of the record ring — the take is spliced even though
    // dropped=0. Any stall long enough to overrun the RX DMA queue (flash
    // erase, NVS commit, future additions) is caught here. External (upload)
    // takes don't contain live audio, so overruns don't affect them.
    uint32_t ovf = (s_rec_source == AUDIO_REC_SRC_AUDIO)
                   ? audio_pipeline_rx_overruns() - s_rec_ovf_start : 0;
    if (ovf > 0) {
        ESP_LOGW(TAG, "%u I2S RX overrun(s) during capture — take has splice gaps", (unsigned)ovf);
        printf("WARNING: %u I2S RX overrun(s) during capture — the take has splice gaps\n",
               (unsigned)ovf);
    }

    if (s_rec_backend == AUDIO_BACKEND_FLASH) {
        if (ok && pcm_bytes > 0) flash_write_metadata((uint32_t)pcm_bytes);
        else ESP_LOGW(TAG, "Flash recording incomplete (%llu bytes)", (unsigned long long)pcm_bytes);
        printf("Flash recording done: %.1f s (%llu bytes)\n", secs, (unsigned long long)pcm_bytes);
    } else {
        if (s_sink_fd >= 0) {
            // Patch the placeholder WAV header with the real data size.
            wav_header_t h;
            wav_make_header(&h, s_channels, s_sample_rate, s_bits, (uint32_t)pcm_bytes);
            if (lseek(s_sink_fd, 0, SEEK_SET) == 0)
                (void)!write(s_sink_fd, &h, sizeof(h));
            close(s_sink_fd);
            s_sink_fd = -1;
        }
        ESP_LOGI(TAG, "Saved %.1f s (%llu bytes) -> %s", secs, (unsigned long long)pcm_bytes, s_rec_path);
        printf("Saved %.1f s (%llu bytes) -> %s\n", secs, (unsigned long long)pcm_bytes, s_rec_path);
    }
}

static void rec_writer_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "writer task on core %d", xPortGetCoreID());
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_writer_armed) continue;

        bool ok = true;
        for (;;) {
            xSemaphoreTake(s_rec_lock, portMAX_DELAY);
            bool     storing = s_rec_storing;
            uint64_t limit   = storing ? s_rec_head : s_rec_store_end;
            uint64_t tail    = s_rec_store_tail;
            xSemaphoreGive(s_rec_lock);

            uint64_t avail = limit - tail;
            if (avail == 0) {
                if (!storing) break;                       // stopped and fully drained
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
                continue;
            }

            size_t chunk = (avail > s_rec_dma_sz) ? s_rec_dma_sz : (size_t)avail;
            rec_ring_read(s_rec_dma, tail, chunk);
            esp_err_t e = rec_sink_write(s_rec_dma, chunk);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "sink write failed: %s — stopping", esp_err_to_name(e));
                ok = false;
                break;
            }

            xSemaphoreTake(s_rec_lock, portMAX_DELAY);
            s_rec_store_tail += chunk;
            xSemaphoreGive(s_rec_lock);
        }

        uint64_t pcm = s_rec_store_tail - s_rec_store_start;
        rec_sink_finalize(pcm, ok);
        s_writer_armed = false;
        xSemaphoreGive(s_finish_sem);
    }
}

// ---------------------------------------------------------------------------
// Unified playback reader task (Core 0, priority 9)
//
// Prefills the shared PSRAM ring from whichever backend is active:
//   FLASH → esp_partition_read() in 32 KB chunks
//   SD    → fread() in 32 KB chunks
//
// No file/flash I/O ever happens in the audio hot path (audio_store_read_block).
// ---------------------------------------------------------------------------

static void pb_reader_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            if (!s_play_active) break;

            // Snapshot ring state
            xSemaphoreTake(s_pb_lock, portMAX_DELAY);
            size_t space = PB_RING_BYTES - s_pb_used;
            size_t head  = s_pb_head;
            xSemaphoreGive(s_pb_lock);

            if (space < PB_CHUNK_BYTES) break;  // ring near-full — wait for hot path to drain

            size_t n       = 0;
            size_t to_read = 0;

            if (s_play_backend == AUDIO_BACKEND_FLASH) {
                // ---- Flash backend ----
                size_t remaining = s_fl_play_total - s_fl_play_cur;
                if (remaining == 0) {
                    if (!s_pb_loop) { s_pb_eof = true; break; }
                    s_fl_play_cur = 0;
                    remaining     = s_fl_play_total;
                }

                to_read = PB_CHUNK_BYTES;
                if (to_read > remaining) to_read = remaining;
                if (to_read > space)     to_read = space;

                // Write into ring, handling wrap with two partition reads
                size_t contig = PB_RING_BYTES - head;
                esp_err_t err;
                if (contig >= to_read) {
                    err = esp_partition_read(s_part,
                                             PCM_OFFSET + s_fl_play_cur,
                                             s_pb_ring + head, to_read);
                } else {
                    err = esp_partition_read(s_part,
                                             PCM_OFFSET + s_fl_play_cur,
                                             s_pb_ring + head, contig);
                    if (err == ESP_OK)
                        err = esp_partition_read(s_part,
                                                 PCM_OFFSET + s_fl_play_cur + contig,
                                                 s_pb_ring, to_read - contig);
                }
                if (err == ESP_OK) {
                    n = to_read;
                    s_fl_play_cur += (uint32_t)n;
                } else {
                    ESP_LOGE(TAG, "partition_read failed: %s", esp_err_to_name(err));
                    s_pb_eof = true;
                    break;
                }

            } else {
                // ---- SD backend ----
                if (!s_pb_fp) break;

                uint32_t file_rem = s_pb_data_total - s_pb_file_pos;
                if (file_rem == 0) {
                    if (!s_pb_loop) { s_pb_eof = true; break; }
                    fseek(s_pb_fp, s_pb_data_off, SEEK_SET);
                    s_pb_file_pos = 0;
                    file_rem      = s_pb_data_total;
                }

                to_read = PB_CHUNK_BYTES;
                if (to_read > (size_t)file_rem) to_read = (size_t)file_rem;
                if (to_read > space)            to_read = space;

                size_t contig = PB_RING_BYTES - head;
                if (contig >= to_read) {
                    n = fread(s_pb_ring + head, 1, to_read, s_pb_fp);
                } else {
                    size_t n1 = fread(s_pb_ring + head, 1, contig, s_pb_fp);
                    size_t n2 = (n1 == contig)
                                ? fread(s_pb_ring, 1, to_read - contig, s_pb_fp) : 0;
                    n = n1 + n2;
                }
                if (n > 0) s_pb_file_pos += (uint32_t)n;
                if (n < to_read) { if (!s_pb_loop) s_pb_eof = true; break; }
            }

            // Commit bytes to ring
            if (n > 0) {
                xSemaphoreTake(s_pb_lock, portMAX_DELAY);
                s_pb_head  = (s_pb_head + n) % PB_RING_BYTES;
                s_pb_used += n;
                xSemaphoreGive(s_pb_lock);
            }
        }

        // Stop handshake: this task owns s_pb_fp (see the state block above),
        // so the close happens here — never while an fread may be in flight.
        if (s_pb_stop_req) {
            if (s_pb_fp) { fclose(s_pb_fp); s_pb_fp = NULL; }
            xSemaphoreTake(s_pb_lock, portMAX_DELAY);
            s_pb_head = s_pb_tail = 0;
            s_pb_used = 0;
            xSemaphoreGive(s_pb_lock);
            s_pb_stop_req = false;
            xSemaphoreGive(s_pb_stopped_sem);
        }
    }
}

// ---------------------------------------------------------------------------
// WAV chunk parsing (read side; the writer half lives in wav_util.h)
// ---------------------------------------------------------------------------

// Walk the RIFF chunk list from the CURRENT file position looking for `id`.
// On success the position is at the chunk payload and *out_size holds its
// size. The 8-byte header read, little-endian size decode, and odd-byte pad
// skip exist here ONLY — both lookups below ride this one walk.
static esp_err_t wav_find_chunk(FILE *fp, const char id[4], uint32_t *out_size)
{
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, fp) != 8) return ESP_ERR_NOT_FOUND;
        uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                      ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(&ch[0], id, 4) == 0) { *out_size = sz; return ESP_OK; }
        if (fseek(fp, (long)sz + (sz & 1), SEEK_CUR) != 0) return ESP_ERR_NOT_FOUND;
    }
}

static esp_err_t wav_find_data_chunk(FILE *fp, long *out_off, uint32_t *out_size)
{
    if (fseek(fp, 0, SEEK_SET) != 0) return ESP_FAIL;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) return ESP_FAIL;
    if (memcmp(&hdr[0], "RIFF", 4) != 0 || memcmp(&hdr[8], "WAVE", 4) != 0)
        return ESP_ERR_INVALID_RESPONSE;
    esp_err_t err = wav_find_chunk(fp, "data", out_size);
    if (err != ESP_OK) return err;
    long off = ftell(fp);
    if (off < 0) return ESP_FAIL;
    *out_off = off;
    return ESP_OK;
}

// Parse the "fmt " chunk (channels / sample rate / bits); leaves the file
// position undefined.
static esp_err_t wav_read_fmt(FILE *fp, uint16_t *ch, uint32_t *sr, uint16_t *bits)
{
    if (fseek(fp, 12, SEEK_SET) != 0) return ESP_FAIL;
    uint32_t sz = 0;
    esp_err_t err = wav_find_chunk(fp, "fmt ", &sz);
    if (err != ESP_OK) return err;
    uint8_t f[16];
    if (sz < 16 || fread(f, 1, 16, fp) != 16) return ESP_FAIL;
    *ch   = (uint16_t)(f[2] | (f[3] << 8));
    *sr   = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
            ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
    *bits = (uint16_t)(f[14] | (f[15] << 8));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Flash metadata writer (shared by record_stop and auto-stop)
// ---------------------------------------------------------------------------

static void flash_write_metadata(uint32_t pcm_bytes)
{
    sample_meta_t m;
    memset(&m, 0xFF, sizeof(m));
    m.magic           = META_MAGIC;
    m.sample_rate     = s_fl_rec_sr;
    m.bits_per_sample = s_fl_rec_bits;
    m.channels        = s_fl_rec_ch;
    m.pcm_bytes       = pcm_bytes;
    memset(m._pad, 0x00, sizeof(m._pad));
    esp_partition_write(s_part, META_OFFSET, &m, sizeof(m));
    s_fl_play_total = pcm_bytes;
    // Silent metadata I/O — the take's single completion message is printed by
    // rec_sink_finalize (this used to double-print with a second, divergent
    // duration computation).
}

// ---------------------------------------------------------------------------
// Flash WAV export (for web streaming)
// ---------------------------------------------------------------------------

esp_err_t audio_store_flash_write_wav(audio_store_write_fn_t write_fn, void *ctx)
{
    if (!s_part) return ESP_ERR_NOT_FOUND;

    sample_meta_t m;
    esp_partition_read(s_part, META_OFFSET, &m, sizeof(m));
    if (m.magic != META_MAGIC || m.pcm_bytes == 0) return ESP_ERR_NOT_FOUND;

    wav_header_t hdr;
    wav_make_header(&hdr, m.channels, m.sample_rate, m.bits_per_sample, m.pcm_bytes);
    esp_err_t err = write_fn(&hdr, sizeof(hdr), ctx);
    if (err != ESP_OK) return err;

    uint8_t buf[4096];
    uint32_t offset = 0;
    while (offset < m.pcm_bytes) {
        uint32_t n = m.pcm_bytes - offset;
        if (n > sizeof(buf)) n = sizeof(buf);
        err = esp_partition_read(s_part, PCM_OFFSET + offset, buf, n);
        if (err != ESP_OK) return err;
        err = write_fn(buf, n, ctx);
        if (err != ESP_OK) return err;
        offset += n;
    }
    return ESP_OK;
}

size_t audio_store_flash_wav_size(void)
{
    if (!s_part) return 0;
    sample_meta_t m;
    esp_partition_read(s_part, META_OFFSET, &m, sizeof(m));
    if (m.magic != META_MAGIC || m.pcm_bytes == 0) return 0;
    return sizeof(wav_header_t) + m.pcm_bytes;
}

// ---------------------------------------------------------------------------
// Public API — init
// ---------------------------------------------------------------------------

// Boot-once init. Deliberate policy: failures do NOT unwind (rings/semaphores
// already allocated stay allocated) — app_main ESP_ERROR_CHECKs the result, so
// a failure here is fatal at boot and the "leak" never outlives the panic.
esp_err_t audio_store_init(void)
{
    if (s_writer_task) return ESP_OK;   // idempotence guard (boot calls once)

    // --- Flash backend ---
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY,
                                      PARTITION_LABEL);
    if (!s_part) {
        ESP_LOGW(TAG, "Partition '%s' not found — flash backend unavailable", PARTITION_LABEL);
    } else {
        ESP_LOGI(TAG, "Flash partition '%s': offset=0x%X size=0x%X (%u KB)",
                 PARTITION_LABEL, (unsigned)s_part->address,
                 (unsigned)s_part->size, (unsigned)(s_part->size / 1024));

        sample_meta_t m;
        esp_partition_read(s_part, META_OFFSET, &m, sizeof(m));
        if (m.magic == META_MAGIC && m.pcm_bytes > 0) {
            s_fl_play_total = m.pcm_bytes;
            float secs = (float)m.pcm_bytes /
                         (float)(m.sample_rate * m.channels * (m.bits_per_sample / 8));
            ESP_LOGI(TAG, "Flash: existing sample %.1fs %uHz %uch %ubit",
                     secs, (unsigned)m.sample_rate,
                     (unsigned)m.channels, (unsigned)m.bits_per_sample);
        } else {
            ESP_LOGI(TAG, "Flash: no valid sample");
        }
    }

    // --- Allocate PSRAM rings + locks ---
    s_finish_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_finish_sem, ESP_ERR_NO_MEM, TAG, "finish sem");

    s_pb_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_pb_lock, ESP_ERR_NO_MEM, TAG, "pb mutex");

    // Playback stop handshake (see the ownership comment at the s_pb_fp block).
    s_pb_stopped_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_pb_stopped_sem, ESP_ERR_NO_MEM, TAG, "pb stop sem");
    s_pb_stop_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_pb_stop_mutex, ESP_ERR_NO_MEM, TAG, "pb stop mutex");

    s_rec_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rec_lock, ESP_ERR_NO_MEM, TAG, "rec mutex");

    // Streaming record ring (PSRAM).
    s_rec_ring = heap_caps_malloc(REC_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_rec_ring, ESP_ERR_NO_MEM, TAG, "rec ring alloc");

    // Writer DMA staging — internal SRAM is mandatory (a PSRAM source forces the
    // SDMMC diskio bounce path). Adaptive: shrink to fit a fragmented heap.
    s_rec_dma_sz = REC_DMA_BYTES;
    while (s_rec_dma_sz >= 4096u) {
        s_rec_dma = heap_caps_malloc(s_rec_dma_sz,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_rec_dma) break;
        s_rec_dma_sz /= 2;
    }
    ESP_RETURN_ON_FALSE(s_rec_dma, ESP_ERR_NO_MEM, TAG, "rec dma staging");

    s_pb_ring = heap_caps_malloc(PB_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_pb_ring, ESP_ERR_NO_MEM, TAG, "pb ring alloc");

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(rec_writer_task, "as_writer", 4096, NULL, 10, &s_writer_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "writer task");

    // Priority 9 — above most WiFi application tasks, below writer (10) and audio (11/12)
    ok = xTaskCreatePinnedToCore(pb_reader_task, "as_reader", 4096, NULL, 9, &s_pb_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "reader task");
    ESP_LOGI(TAG, "Init OK. rec_ring=%u KB dma=%u KB pb_ring=%u KB (streaming record + playback)",
             (unsigned)(REC_RING_BYTES / 1024), (unsigned)(s_rec_dma_sz / 1024),
             (unsigned)(PB_RING_BYTES / 1024));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API — recording
// ---------------------------------------------------------------------------

bool audio_store_is_recording(void) { return s_rec_storing; }

// Hot-path gate: true only when the AUDIO pipeline should feed the ring. An
// EXTERNAL (upload) take owns the ring exclusively — the hot path submitting
// alongside it would interleave live audio into the uploaded sample.
bool audio_store_capture_active(void)
{
    return (s_rec_storing && s_rec_source == AUDIO_REC_SRC_AUDIO) || s_rec_live_on;
}

void audio_store_set_record_tap(audio_record_tap_t tap)
{
    s_record_tap = tap;
    if (s_rec_storing || s_rec_live_on) {
        ESP_LOGW(TAG, "Record tap changed mid-capture — applies to the next start");
    }
}

audio_record_tap_t audio_store_get_record_tap(void)
{
    return (s_rec_storing || s_rec_live_on) ? s_rec_tap_active : s_record_tap;
}

// Latch the tap on the first active consumer so it can't change mid-capture.
// Caller holds s_rec_lock.
static void capture_arm_tap(void)
{
    if (!s_rec_storing && !s_rec_live_on) s_rec_tap_active = s_record_tap;
}

// Format the active tap produces (TDM-aware): PRE = the raw RX frame
// (rx_ch interleaved channels at slot width), POST = the stereo mixdown.
static void tap_format(audio_record_tap_t tap, uint16_t *ch, uint16_t *bits)
{
    *bits = (uint16_t)g_geom.slot_bits;
    *ch   = (tap == AUDIO_TAP_PRE) ? (uint16_t)g_geom.rx_ch : 2;
}

// SD bandwidth gate for raw multichannel takes. Measured FAT-DMA ceiling is
// ~1.1 MB/s (sd_diag); with the S3's 128-bit TDM frame cap the worst feasible
// rate is 768 KB/s, so this cannot trip today — it guards future edits.
#define REC_SD_RATE_CEILING  900000u

// Body of record_start; runs with the recorder claimed (s_rec_starting held
// by the public wrapper below), so no early return here can double-arm.
static esp_err_t record_start_claimed(audio_backend_t backend, const char *sd_path,
                                      uint32_t sample_rate, uint16_t bits_per_sample,
                                      uint16_t channels, audio_record_source_t source)
{
    // An external take and live capture would both feed the ring — refuse.
    if (source == AUDIO_REC_SRC_EXTERNAL && s_rec_live_on) {
        ESP_LOGE(TAG, "Cannot upload while live capture is active");
        return ESP_ERR_INVALID_STATE;
    }

    // AUDIO-source takes record what the hot path actually submits — the tap
    // decides channels/bits (TDM-aware). Override mismatched caller args so a
    // WAV header can never mislabel the ring data. External (upload) takes
    // keep the caller's format verbatim.
    if (source == AUDIO_REC_SRC_AUDIO) {
        audio_record_tap_t t = (!s_rec_storing && !s_rec_live_on) ? s_record_tap
                                                                  : s_rec_tap_active;
        uint16_t want_ch, want_bits;
        tap_format(t, &want_ch, &want_bits);
        if (channels != want_ch || bits_per_sample != want_bits || sample_rate != FS_HZ) {
            ESP_LOGW(TAG, "rec format %uch/%ubit@%u overridden to tap format %uch/%ubit@%d",
                     channels, bits_per_sample, (unsigned)sample_rate,
                     want_ch, want_bits, FS_HZ);
            channels = want_ch; bits_per_sample = want_bits; sample_rate = FS_HZ;
        }
        uint32_t rate = sample_rate * channels * (bits_per_sample / 8);
        if (backend == AUDIO_BACKEND_SD && rate > REC_SD_RATE_CEILING) {
            ESP_LOGE(TAG, "record rate %u B/s exceeds the SD ceiling %u — switch to 16-bit slots",
                     (unsigned)rate, (unsigned)REC_SD_RATE_CEILING);
            return ESP_ERR_INVALID_ARG;
        }
    }

    s_rec_backend  = backend;
    s_rec_source   = source;
    s_sample_rate  = sample_rate;
    s_bits         = bits_per_sample;
    s_channels     = channels;
    s_dropped_blocks = 0;

    if (backend == AUDIO_BACKEND_FLASH) {
        if (!s_part) { ESP_LOGE(TAG, "Flash partition unavailable"); return ESP_FAIL; }
        if (s_play_active && s_play_backend == AUDIO_BACKEND_FLASH) {
            ESP_LOGE(TAG, "Cannot record to flash while playing from flash");
            return ESP_ERR_INVALID_STATE;
        }
        s_fl_rec_sr = sample_rate; s_fl_rec_bits = bits_per_sample; s_fl_rec_ch = channels;
        s_fl_write_off = 0; s_fl_erased_to = 0;
        s_rec_store_limit = s_part->size - PCM_OFFSET;   // flash PCM capacity (~38 s @ 384 KB/s)

        // Erase the whole capture region BEFORE arming — never mid-take
        // (issue #12, see flash_pre_erase). Capture starts when this returns.
        printf("Preparing flash (erasing previous take — a few seconds if it was long)...\n");
        esp_err_t pe = flash_pre_erase();
        if (pe != ESP_OK) {
            ESP_LOGE(TAG, "flash pre-erase failed: %s", esp_err_to_name(pe));
            return pe;
        }
    } else {
        if (!g_state.sd_card) { ESP_LOGE(TAG, "No SD card mounted"); return ESP_ERR_NOT_FOUND; }
        const char *path = (sd_path && *sd_path) ? sd_path : "/sdcard/rec.wav";
        strncpy(s_rec_path, path, sizeof(s_rec_path) - 1);
        s_rec_path[sizeof(s_rec_path) - 1] = '\0';

        // Open file and write a placeholder WAV header (sizes patched at stop).
        s_sink_fd = open(s_rec_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (s_sink_fd < 0) { ESP_LOGE(TAG, "open %s failed", s_rec_path); return ESP_FAIL; }
        wav_header_t h;
        wav_make_header(&h, channels, sample_rate, bits_per_sample, 0);
        if (write(s_sink_fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) {
            ESP_LOGE(TAG, "WAV header write failed"); close(s_sink_fd); s_sink_fd = -1;
            return ESP_FAIL;
        }
        s_rec_store_limit = UINT64_MAX;
        uint64_t total_fs = 0, free_fs = 0;
        if (esp_vfs_fat_info("/sdcard", &total_fs, &free_fs) == ESP_OK
            && free_fs > sizeof(wav_header_t)) {
            s_rec_store_limit = free_fs - sizeof(wav_header_t);   // cap by free space
        }
    }

    while (xSemaphoreTake(s_finish_sem, 0) == pdTRUE) {}   // drain stale completions

    // Baseline for the end-of-take integrity check. Taken AFTER the flash
    // pre-erase: overruns during the erase are expected and harmless (capture
    // hasn't started); overruns after this point mean the take has gaps.
    s_rec_ovf_start = audio_pipeline_rx_overruns();

    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
    capture_arm_tap();
    s_rec_store_start = s_rec_head;     // capture from "now"
    s_rec_store_tail  = s_rec_head;
    s_rec_storing     = true;
    s_writer_armed    = true;
    xSemaphoreGive(s_rec_lock);

    xTaskNotifyGive(s_writer_task);
    ESP_LOGI(TAG, "Recording started [%s] sr=%u bits=%u ch=%u tap=%s src=%s",
             backend == AUDIO_BACKEND_FLASH ? "flash" : s_rec_path,
             (unsigned)sample_rate, (unsigned)bits_per_sample, (unsigned)channels,
             s_rec_tap_active == AUDIO_TAP_POST ? "post" : "pre",
             source == AUDIO_REC_SRC_EXTERNAL ? "external" : "audio");
    return ESP_OK;
}

esp_err_t audio_store_record_start(audio_backend_t backend, const char *sd_path,
                                   uint32_t sample_rate, uint16_t bits_per_sample,
                                   uint16_t channels, audio_record_source_t source)
{
    // Claim the recorder atomically. A bare `if (s_rec_storing)` check is a
    // TOCTOU: CLI, HTTP and serial can all call this concurrently, pass the
    // check together, and double-arm (concurrent pre-erases sharing s_rec_dma,
    // interleaved sinks). The claim covers the whole slow setup (pre-erase /
    // file open) until the take is armed or abandoned.
    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
    if (s_rec_storing || s_writer_armed || s_rec_starting) {
        xSemaphoreGive(s_rec_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_rec_starting = true;
    xSemaphoreGive(s_rec_lock);

    esp_err_t err = record_start_claimed(backend, sd_path, sample_rate,
                                         bits_per_sample, channels, source);
    s_rec_starting = false;
    return err;
}

// Hot path — appends one block to the record ring (PSRAM). No file/flash/network
// I/O here: the writer task and live HTTP reader drain the ring via their cursors.
void audio_store_record_submit(const void *data, size_t len)
{
    if (!data || len == 0) return;
    if (!s_rec_storing && !s_rec_live_on) return;

    bool auto_stop = false;

    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
    uint64_t head = s_rec_head;
    if (s_rec_storing) {
        uint64_t unstored  = head - s_rec_store_tail;          // waiting for writer
        uint64_t ring_room = REC_RING_BYTES - unstored;
        if (len > ring_room) {                                 // writer behind → drop newest
            s_dropped_blocks++;
            xSemaphoreGive(s_rec_lock);
            return;
        }
        uint64_t stored = head - s_rec_store_start;
        if (stored + len >= s_rec_store_limit) {               // capacity reached → last block
            len = (s_rec_store_limit > stored) ? (size_t)(s_rec_store_limit - stored) : 0;
            auto_stop = true;
        }
    }
    xSemaphoreGive(s_rec_lock);

    if (len > 0) {
        size_t off = (size_t)(head % REC_RING_BYTES);
        size_t c1  = REC_RING_BYTES - off;
        if (c1 >= len) {
            memcpy(s_rec_ring + off, data, len);
        } else {
            memcpy(s_rec_ring + off, data, c1);
            memcpy(s_rec_ring, (const uint8_t *)data + c1, len - c1);
        }

        xSemaphoreTake(s_rec_lock, portMAX_DELAY);
        s_rec_head += len;
        head = s_rec_head;
        if (s_rec_live_on && (s_rec_head - s_rec_live_tail) > REC_RING_BYTES) {
            s_live_drops++;                                    // live consumer fell behind
            s_rec_live_tail = s_rec_head - REC_RING_BYTES;
        }
        bool storing = s_rec_storing;
        xSemaphoreGive(s_rec_lock);

        if (storing) xTaskNotifyGive(s_writer_task);           // writer only drains storage
    }

    if (auto_stop) {
        xSemaphoreTake(s_rec_lock, portMAX_DELAY);
        s_rec_storing   = false;
        s_rec_store_end = head;
        xSemaphoreGive(s_rec_lock);
        xTaskNotifyGive(s_writer_task);
        ESP_LOGW(TAG, "Record capacity reached — auto-stopped");
    }
}

void audio_store_record_stop(void)
{
    // Claim the stop under the lock (CLI and HTTP can both call this): the
    // caller that flips s_rec_storing is the one that waits on s_finish_sem.
    // A losing concurrent stop returns immediately — without the claim, both
    // would block on the semaphore and one would hang forever (mirror of
    // record_start's s_rec_starting TOCTOU claim).
    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
    bool claim = s_rec_storing;
    if (claim) {
        // Drop any stale finalize signal first (an auto-stop nobody waited
        // on leaves a count behind); done before the flip, while the writer
        // cannot yet be finalizing OUR take, so the wait below pairs with
        // exactly our finalize.
        while (xSemaphoreTake(s_finish_sem, 0) == pdTRUE) { }
        s_rec_storing   = false;
        s_rec_store_end = s_rec_head;
    }
    xSemaphoreGive(s_rec_lock);
    if (!claim) return;   // nothing in progress, or another stop owns the wait

    xTaskNotifyGive(s_writer_task);
    xSemaphoreTake(s_finish_sem, portMAX_DELAY);     // wait for drain + finalize
}

// ---------------------------------------------------------------------------
// Public API — live capture (best-effort second consumer of the record ring)
// ---------------------------------------------------------------------------

esp_err_t audio_store_live_open(void)
{
    if (!s_rec_ring) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
    // An external (upload) take owns the ring — live audio arming mid-upload
    // would interleave into it (mirror of the check in record_start).
    if (s_rec_storing && s_rec_source == AUDIO_REC_SRC_EXTERNAL) {
        xSemaphoreGive(s_rec_lock);
        ESP_LOGE(TAG, "Cannot open live capture during an upload");
        return ESP_ERR_INVALID_STATE;
    }
    capture_arm_tap();
    // Latch the streaming-WAV-header format from the tap unless a storage
    // take already owns s_* (record_start set them to the same tap format).
    if (!s_rec_storing) {
        s_sample_rate = FS_HZ;
        tap_format(s_rec_tap_active, &s_channels, &s_bits);
    }
    s_rec_live_tail = s_rec_head;   // start from "now"
    s_rec_live_on   = true;
    s_live_drops    = 0;
    s_live_ovf_start = audio_pipeline_rx_overruns();   // splice tripwire baseline
    xSemaphoreGive(s_rec_lock);
    ESP_LOGI(TAG, "Live capture opened tap=%s",
             s_rec_tap_active == AUDIO_TAP_POST ? "post" : "pre");
    return ESP_OK;
}

void audio_store_live_close(void)
{
    xSemaphoreTake(s_rec_lock, portMAX_DELAY);
    s_rec_live_on = false;
    xSemaphoreGive(s_rec_lock);
}

bool audio_store_live_active(void) { return s_rec_live_on; }

// I2S RX overruns since live_open — nonzero means the live stream is spliced
// (issue #12 class). Live capture has no finalize step, so the transports
// (xfer live, /api/rec/live) report this when their stream ends.
uint32_t audio_store_live_overruns(void)
{
    return audio_pipeline_rx_overruns() - s_live_ovf_start;
}

size_t audio_store_live_read(void *dst, size_t want, int timeout_ms)
{
    if (!dst || want == 0 || !s_rec_live_on) return 0;
    int waited = 0;
    for (;;) {
        xSemaphoreTake(s_rec_lock, portMAX_DELAY);
        uint64_t avail = s_rec_head - s_rec_live_tail;
        uint64_t tail  = s_rec_live_tail;
        xSemaphoreGive(s_rec_lock);

        if (avail > 0) {
            size_t n = (avail > want) ? want : (size_t)avail;
            rec_ring_read((uint8_t *)dst, tail, n);
            xSemaphoreTake(s_rec_lock, portMAX_DELAY);
            s_rec_live_tail += n;
            xSemaphoreGive(s_rec_lock);
            return n;
        }
        if (!s_rec_live_on || waited >= timeout_ms) return 0;
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
    }
}

void audio_store_live_wav_header(void *dst16)
{
    // Streaming WAV header: near-max sizes so players accept an open-ended stream
    // (data_size chosen so riff_size = 36 + data_size doesn't wrap).
    wav_header_t *h = (wav_header_t *)dst16;
    wav_make_header(h, s_channels, s_sample_rate, s_bits,
                    0xFFFFFFFFu - WAV_RIFF_OVERHEAD);
    h->riff_size = 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Public API — playback
// ---------------------------------------------------------------------------

bool audio_store_is_playing(void) { return s_play_active; }

esp_err_t audio_store_play_start(audio_backend_t backend, const char *sd_path, bool loop)
{
    audio_store_play_stop();  // stop and clear ring before starting
    s_play_backend = backend;

    if (backend == AUDIO_BACKEND_FLASH) {
        if (!s_part) { ESP_LOGE(TAG, "Flash partition unavailable"); return ESP_FAIL; }

        sample_meta_t m;
        esp_partition_read(s_part, META_OFFSET, &m, sizeof(m));
        if (m.magic != META_MAGIC || m.pcm_bytes == 0) {
            ESP_LOGE(TAG, "No valid sample in flash");
            return ESP_ERR_NOT_FOUND;
        }
        // Playback contract: 48 kHz stereo 32-bit only (the overlay feeds the
        // pipeline in that shape). Other formats — e.g. a TDM multichannel or
        // 16-bit take — played as garbage silently before; refuse instead.
        if (m.channels != 2 || m.sample_rate != FS_HZ || m.bits_per_sample != 32) {
            ESP_LOGE(TAG, "flash sample is %uch/%ubit @ %u Hz — playback needs 2ch/32bit @ %d (convert on host)",
                     m.channels, m.bits_per_sample, (unsigned)m.sample_rate, FS_HZ);
            return ESP_ERR_NOT_SUPPORTED;
        }

        xSemaphoreTake(s_pb_lock, portMAX_DELAY);
        s_fl_play_total = m.pcm_bytes;
        s_fl_play_cur   = 0;
        s_pb_head = 0; s_pb_tail = 0; s_pb_used = 0;
        s_pb_eof  = false;
        s_pb_loop = loop;
        xSemaphoreGive(s_pb_lock);

        float secs = (float)m.pcm_bytes /
                     (float)(m.sample_rate * m.channels * (m.bits_per_sample / 8));
        ESP_LOGI(TAG, "Flash playback: %.1fs loop=%d — prefilling ring via reader task",
                 secs, loop ? 1 : 0);

    } else {
        const char *path = (sd_path && *sd_path) ? sd_path : "/sdcard/rec.wav";
        FILE *fp = fopen(path, "rb");
        if (!fp) { ESP_LOGE(TAG, "open %s failed", path); return ESP_FAIL; }

        long data_off = 0; uint32_t data_sz = 0;
        if (wav_find_data_chunk(fp, &data_off, &data_sz) != ESP_OK) {
            ESP_LOGE(TAG, "WAV data chunk not found: %s", path);
            fclose(fp); return ESP_FAIL;
        }
        // Playback contract: 48 kHz stereo 32-bit only (see the flash branch).
        uint16_t fch = 0, fbits = 0; uint32_t fsr = 0;
        if (wav_read_fmt(fp, &fch, &fsr, &fbits) != ESP_OK ||
            fch != 2 || fsr != FS_HZ || fbits != 32) {
            ESP_LOGE(TAG, "%s is %uch/%ubit @ %u Hz — playback needs 2ch/32bit @ %d (convert on host)",
                     path, fch, fbits, (unsigned)fsr, FS_HZ);
            fclose(fp); return ESP_ERR_NOT_SUPPORTED;
        }

        xSemaphoreTake(s_pb_lock, portMAX_DELAY);
        s_pb_fp = fp; s_pb_data_off = data_off; s_pb_data_total = data_sz;
        s_pb_file_pos = 0; s_pb_head = 0; s_pb_tail = 0; s_pb_used = 0;
        s_pb_eof = false; s_pb_loop = loop;
        xSemaphoreGive(s_pb_lock);

        fseek(fp, data_off, SEEK_SET);
        float secs = audio_duration_from_bytes(data_sz);
        ESP_LOGI(TAG, "SD playback: %s (%.1fs) loop=%d", path, secs, loop ? 1 : 0);
    }

    s_play_active = true;
    xTaskNotifyGive(s_pb_task);  // start prefilling ring
    return ESP_OK;
}

void audio_store_play_stop(void)
{
    // Serialize stops (CLI + HTTP can race, and play_start calls this too):
    // the mutex makes a second caller wait until the first teardown is done,
    // so play_start can never hand the reader a new file while the old one
    // is still being closed.
    xSemaphoreTake(s_pb_stop_mutex, portMAX_DELAY);
    if (s_play_active || s_pb_fp) {
        s_pb_stop_req = true;
        s_play_active = false;
        xTaskNotifyGive(s_pb_task);       // wake a parked reader
        // Reader owns the file close + ring reset. Bounded wait: worst case
        // one 32 KB fread + fclose. The timeout only fires if the SD card
        // has wedged mid-read (known failure mode — needs a power cycle).
        if (xSemaphoreTake(s_pb_stopped_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "play_stop: reader did not acknowledge (SD wedged?)");
        }
    }
    xSemaphoreGive(s_pb_stop_mutex);
}

// Hot path — PSRAM ring drain only; no flash or SD I/O regardless of backend
void audio_store_read_block(void *dst, size_t len)
{
    if (!dst) return;

    xSemaphoreTake(s_pb_lock, portMAX_DELAY);
    size_t available = s_pb_used;
    size_t tail      = s_pb_tail;
    bool   active    = s_play_active;
    bool   eof       = s_pb_eof;
    xSemaphoreGive(s_pb_lock);

    if (!active) { memset(dst, 0, len); return; }

    size_t to_copy = (available >= len) ? len : available;
    if (to_copy > 0) {
        uint8_t *out = (uint8_t *)dst;
        size_t part1 = PB_RING_BYTES - tail;
        if (part1 >= to_copy) {
            memcpy(out, s_pb_ring + tail, to_copy);
        } else {
            memcpy(out,         s_pb_ring + tail, part1);
            memcpy(out + part1, s_pb_ring,        to_copy - part1);
        }

        xSemaphoreTake(s_pb_lock, portMAX_DELAY);
        s_pb_tail  = (s_pb_tail + to_copy) % PB_RING_BYTES;
        s_pb_used -= to_copy;
        bool low   = (s_pb_used < PB_LOW_WM);
        xSemaphoreGive(s_pb_lock);

        if (low && !eof) xTaskNotifyGive(s_pb_task);
    }

    if (to_copy < len) {
        memset((uint8_t *)dst + to_copy, 0, len - to_copy);
        if (eof && to_copy == 0) s_play_active = false;
    }
}

// ---------------------------------------------------------------------------
// Legacy stream helpers (`rec stream` / `rec streamhex`)
// ---------------------------------------------------------------------------
// LEGACY alternate transports, superseded by `xfer getraw` and /api/rec/live.
// They emit bulk data through printf/stdout — the exact console-TX pattern
// that rule 4 of the resource-change checklist (docs/ARCHITECTURE.md) forbids
// for new code, because that path may DROP bytes under TX pressure (a lesson
// this project paid for twice). Kept only as a last-resort capture path for
// hosts that can't speak the xfer protocol; do NOT copy this pattern — use
// the flow-controlled writer in serial_xfer.c.

static esp_err_t audio_store_stream_file_raw(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "open %s failed", path); return ESP_FAIL; }
    fflush(stdout);
    const char *hdr = "\n===REC_WAV_BEGIN===\n";
    write(STDOUT_FILENO, hdr, strlen(hdr));
    uint8_t buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        if (write(STDOUT_FILENO, buf, n) < 0) { fclose(fp); return ESP_FAIL; }
    const char *ftr = "\n===REC_WAV_END===\n";
    write(STDOUT_FILENO, ftr, strlen(ftr));
    fflush(stdout);
    fclose(fp);
    return ESP_OK;
}

static esp_err_t audio_store_stream_file_hex(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    FILE *fp = fopen(path, "rb");
    if (!fp) { ESP_LOGE(TAG, "open %s failed", path); return ESP_FAIL; }
    printf("===REC_WAV_HEX_BEGIN===\n");
    uint8_t buf[128]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        for (size_t i = 0; i < n; i++) printf("%02X", buf[i]);
    printf("\n===REC_WAV_HEX_END===\n");
    fclose(fp);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SD Diagnostic
// ---------------------------------------------------------------------------

// Long SD loops monopolize the running core, starving its idle task and tripping
// the task watchdog. Block briefly on a ~1 s cadence so idle can run and feed the
// WDT; the occasional 1-tick delay adds <1% to measured throughput.
static inline void diag_yield(int64_t *last_us)
{
    int64_t now = esp_timer_get_time();
    if (now - *last_us >= 1000000) { *last_us = now; vTaskDelay(1); }
}

// Background writer used by the simultaneous read+write (full-duplex) phase of
// sd_diag. Streams its own file with a private DMA staging buffer while the
// command task reads another file concurrently — mirrors record-while-play.
typedef struct {
    size_t            bytes;       // in:  bytes to write
    const char       *path;        // in:  output file
    int64_t           elapsed_us;  // out: write duration
    volatile bool     ok;          // out: completed without error
    SemaphoreHandle_t done;        // out: given when finished
} diag_writer_ctx_t;

static void diag_writer_task(void *arg)
{
    diag_writer_ctx_t *c = (diag_writer_ctx_t *)arg;
    c->ok = false;
    c->elapsed_us = 0;

    // DMA-capable internal buffer (adaptive). A non-DMA source forces the SDMMC
    // diskio bounce path and collapses throughput, so use internal SRAM here.
    size_t   sz  = DIAG_STAGING_BYTES;
    uint8_t *buf = NULL;
    while (sz >= 4096u) {
        buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (buf) break;
        sz /= 2;
    }
    if (buf) {
        memset(buf, 0x5A, sz);
        // POSIX I/O — straight to FATFS with our DMA buffer, no stdio buffering.
        int fd = open(c->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            size_t written = 0;
            int64_t t0 = esp_timer_get_time(), y = t0;
            bool ok = true;
            while (written < c->bytes) {
                size_t n = (c->bytes - written < sz) ? (c->bytes - written) : sz;
                if (write(fd, buf, n) < (ssize_t)n) { ok = false; break; }
                written += n;
                diag_yield(&y);
            }
            c->elapsed_us = esp_timer_get_time() - t0;
            c->ok = ok;
            close(fd);
        }
        free(buf);
    }
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

// Phase: raw sdmmc_write_sectors throughput at several burst sizes (no FAT).
static void diag_raw_sectors(uint8_t *dma_buf, size_t dma_sz, uint64_t total_sectors)
{
    printf("sdmmc_write_sectors (raw, no FAT overhead):\n");
    if (!dma_buf) {
        printf("  skipped — no DMA-capable internal buffer (internal DMA free %u KB, largest %u KB)\n",
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024));
        return;
    }
    if (dma_sz != DIAG_STAGING_BYTES)
        printf("  (DMA buffer reduced to %u KB)\n", (unsigned)(dma_sz / 1024));
    static const size_t test_sectors[] = { 1, 8, 16, 32, 64, 128 };
    for (size_t t = 0; t < sizeof(test_sectors)/sizeof(test_sectors[0]); t++) {
        size_t sectors      = test_sectors[t];
        size_t bytes_per_it = sectors * DIAG_SECTOR_SIZE;
        if (bytes_per_it > dma_sz) {
            printf("  %4zu sectors: skipped\n", sectors); continue;
        }
        size_t   total_bytes = 4u * 1024u * 1024u;
        size_t   total_iters = total_bytes / bytes_per_it;
        uint64_t start_sec   = total_sectors
                               - (uint64_t)(total_iters * sectors)
                               - (uint64_t)DIAG_END_MARGIN_SECS;
        int64_t t0 = esp_timer_get_time(), y = t0; bool ok = true;
        for (size_t i = 0; i < total_iters; i++) {
            if (sdmmc_write_sectors(g_state.sd_card, dma_buf,
                                    (size_t)(start_sec + i * sectors),
                                    sectors) != ESP_OK) {
                printf("  WRITE ERROR at iter %zu\n", i); ok = false; break;
            }
            diag_yield(&y);
        }
        if (!ok) continue;
        float kb_per_s = ((float)(total_iters * bytes_per_it) / 1024.0f)
                         / ((float)(esp_timer_get_time() - t0) / 1e6f);
        printf("  %4zu sectors (%6zu bytes): %6.0f KB/s  %s\n",
               sectors, bytes_per_it, kb_per_s, kb_per_s >= 375.0f ? "OK" : "SLOW");
    }
}

// Phase: fwrite through FAT — the buffer's memory type is the dominant factor:
// a DMA-capable source DMAs straight to the card; a PSRAM source forces the
// diskio bounce copy. Run both back to back to make the gap visible.
static void diag_fat_fwrite(uint8_t *dma_buf, size_t dma_sz, uint8_t *ps_buf)
{
    printf("\nfwrite (FAT, 4 MB):\n");
    struct { const char *label; uint8_t *src; size_t chunk; } fw[] = {
        { "DMA-capable buf", dma_buf, dma_sz },
        { "PSRAM buf      ", ps_buf,  DIAG_STAGING_BYTES },
    };
    for (size_t t = 0; t < sizeof(fw)/sizeof(fw[0]); t++) {
        if (!fw[t].src) { printf("  %s: (no buffer)\n", fw[t].label); continue; }
        FILE *fp = fopen("/sdcard/diag_tmp.bin", "wb");
        if (!fp) { printf("  fopen failed\n"); break; }
        setvbuf(fp, NULL, _IONBF, 0);
        size_t total_bytes = 4u * 1024u * 1024u, written = 0; bool ok = true;
        int64_t t0 = esp_timer_get_time(), y = t0;
        while (written < total_bytes) {
            size_t sz = (total_bytes - written < fw[t].chunk) ? (total_bytes - written) : fw[t].chunk;
            size_t n = fwrite(fw[t].src, 1, sz, fp);
            written += n;
            if (n < sz) { printf("  short write\n"); ok = false; break; }
            diag_yield(&y);
        }
        int64_t us = esp_timer_get_time() - t0;
        fclose(fp); remove("/sdcard/diag_tmp.bin");
        if (ok) {
            float kb_per_s = (float)written / 1024.0f / ((float)us / 1e6f);
            printf("  %s (chunk %2uKB): %6.0f KB/s  %s\n", fw[t].label,
                   (unsigned)(fw[t].chunk / 1024), kb_per_s, kb_per_s >= 375.0f ? "OK" : "SLOW");
        }
    }
}

// Streaming phase 3: simultaneous read (file A) + write (file B) — record-while-play.
static void diag_duplex(uint8_t *dma_buf, size_t dma_sz, size_t stream_bytes,
                        const char *fa, const char *fb)
{
    printf("  simultaneous read+write (record-while-play):\n");
    diag_writer_ctx_t ctx = { .bytes = stream_bytes, .path = fb,
                              .elapsed_us = 0, .ok = false,
                              .done = xSemaphoreCreateBinary() };
    if (!ctx.done) {
        printf("    sem alloc failed\n");
    } else {
        // Pin to CPU1 — the console (reader) runs on CPU0, so this keeps
        // each core's idle task fed and the two streams truly concurrent.
        BaseType_t tok = xTaskCreatePinnedToCore(diag_writer_task, "diag_wr",
                                                 4096, &ctx, 5, NULL, 1);
        if (tok != pdPASS) {
            printf("    writer task create failed\n");
            vSemaphoreDelete(ctx.done);
        } else {
            // Concurrently read file A in this task.
            bool r_ok = false; size_t rd = 0; int64_t r_us = 0;
            int fd = open(fa, O_RDONLY);
            if (fd >= 0) {
                ssize_t n; int64_t t0 = esp_timer_get_time(), y = t0;
                while ((n = read(fd, dma_buf, dma_sz)) > 0) { rd += (size_t)n; diag_yield(&y); }
                r_us = esp_timer_get_time() - t0;
                close(fd);
                r_ok = true;
            }
            xSemaphoreTake(ctx.done, portMAX_DELAY);
            vSemaphoreDelete(ctx.done);

            float r_kb = (r_ok && r_us) ? (float)rd / 1024.0f / ((float)r_us / 1e6f) : 0.0f;
            float w_kb = (ctx.ok && ctx.elapsed_us)
                         ? (float)ctx.bytes / 1024.0f / ((float)ctx.elapsed_us / 1e6f) : 0.0f;
            printf("    read : %6.0f KB/s  %s\n", r_kb, r_kb >= DIAG_RATE_FLOOR_KBPS ? "OK" : "SLOW");
            printf("    write: %6.0f KB/s  %s\n", w_kb, w_kb >= DIAG_RATE_FLOOR_KBPS ? "OK" : "SLOW");
            printf("    duplex %s — both directions must sustain %.0f KB/s\n",
                   (r_kb >= DIAG_RATE_FLOOR_KBPS && w_kb >= DIAG_RATE_FLOOR_KBPS) ? "OK" : "SLOW",
                   DIAG_RATE_FLOOR_KBPS);
        }
    }
    remove(fb);
}

// Phase: sustained streaming — large-file write, read, then full duplex.
// Sized from SD free space so the duplex phase (two files at once) fits.
static void diag_streaming(uint8_t *dma_buf, size_t dma_sz)
{
    printf("\nStreaming throughput (sustained, FAT):\n");
    size_t stream_bytes = DIAG_STREAM_MAX;
    {
        uint64_t total_fs = 0, free_fs = 0;
        if (esp_vfs_fat_info("/sdcard", &total_fs, &free_fs) == ESP_OK) {
            size_t quarter = (free_fs > 0) ? (size_t)(free_fs / 4u) : 0;
            if (quarter < stream_bytes) stream_bytes = quarter;
        }
    }
    if (dma_buf) stream_bytes &= ~(dma_sz - 1u);  // align to chunk

    if (!dma_buf) {
        printf("  skipped — no DMA-capable buffer\n");
        return;
    }
    if (stream_bytes < DIAG_STREAM_MIN) {
        printf("  skipped — insufficient SD free space\n");
        return;
    }
    const char *fa = "/sdcard/diag_stream.bin";
    const char *fb = "/sdcard/diag_dup_w.bin";
    printf("  file size: %u MB per stream\n", (unsigned)(stream_bytes / (1024u * 1024u)));

    // Phase 1: sustained write of file A.
    bool wrote_a = false;
    {
        int fd = open(fa, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) { printf("  write: open failed\n"); }
        else {
            size_t written = 0; int64_t t0 = esp_timer_get_time(), y = t0; bool ok = true;
            while (written < stream_bytes) {
                size_t sz = (stream_bytes - written < dma_sz)
                            ? (stream_bytes - written) : dma_sz;
                if (write(fd, dma_buf, sz) < (ssize_t)sz) { printf("  short write\n"); ok = false; break; }
                written += sz;
                diag_yield(&y);
            }
            int64_t us = esp_timer_get_time() - t0;
            close(fd);
            if (ok) {
                float kb = (float)written / 1024.0f / ((float)us / 1e6f);
                printf("  write : %6.0f KB/s  %s\n", kb, kb >= DIAG_RATE_FLOOR_KBPS ? "OK" : "SLOW");
                wrote_a = true;
            }
        }
    }

    // Phase 2: sustained read of file A.
    if (wrote_a) {
        int fd = open(fa, O_RDONLY);
        if (fd < 0) { printf("  read: open failed\n"); }
        else {
            size_t rd = 0; ssize_t n; int64_t t0 = esp_timer_get_time(), y = t0;
            while ((n = read(fd, dma_buf, dma_sz)) > 0) { rd += (size_t)n; diag_yield(&y); }
            int64_t us = esp_timer_get_time() - t0;
            close(fd);
            float kb = (float)rd / 1024.0f / ((float)us / 1e6f);
            printf("  read  : %6.0f KB/s  %s\n", kb, kb >= DIAG_RATE_FLOOR_KBPS ? "OK" : "SLOW");
        }
    }

    if (wrote_a) diag_duplex(dma_buf, dma_sz, stream_bytes, fa, fb);
    remove(fa);
}

static int cmd_sd_diag(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!g_state.sd_card) { printf("No SD card mounted.\n"); return 1; }

    // Primary I/O buffer — DMA-capable internal SRAM. This is what the SDMMC
    // diskio layer needs to DMA straight from/to the card; a non-DMA (PSRAM)
    // buffer forces a sector-by-sector bounce copy that collapses throughput.
    // Adaptive: internal SRAM is tight with WiFi/audio running, so shrink to fit.
    // NULL → skip the buffer-bound tests, but the rest still runs.
    size_t   dma_sz  = DIAG_STAGING_BYTES;
    uint8_t *dma_buf = NULL;
    while (dma_sz >= 4096u) {
        dma_buf = heap_caps_malloc(dma_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (dma_buf) break;
        dma_sz /= 2;
    }
    if (dma_buf) memset(dma_buf, 0xA5, dma_sz);

    // PSRAM buffer — used only to demonstrate the non-DMA bounce penalty.
    uint8_t *ps_buf = heap_caps_malloc(DIAG_STAGING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ps_buf) memset(ps_buf, 0xA5, DIAG_STAGING_BYTES);

    uint64_t total_sectors = (uint64_t)g_state.sd_card->csd.capacity;
    printf("\n=== SD Write Diagnostic ===\n");
    printf("Required for 48kHz stereo 32-bit: 375 KB/s\n");
    printf("Card: %llu sectors (%llu MB)\n\n",
           (unsigned long long)total_sectors,
           (unsigned long long)(total_sectors / 2048));

    diag_raw_sectors(dma_buf, dma_sz, total_sectors);
    diag_fat_fwrite(dma_buf, dma_sz, ps_buf);
    diag_streaming(dma_buf, dma_sz);

    // --- Memory summary ----------------------------------------------------
    printf("\nMemory:\n");
    printf("  internal free: %u KB (largest block %u KB)\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    printf("  psram    free: %u KB (largest block %u KB)\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

    if (dma_buf) free(dma_buf);
    if (ps_buf)  free(ps_buf);
    printf("\nDiagnostic complete.\n");
    return 0;
}

void register_sd_diag_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sd_diag",
        .help    = "SD throughput diagnostic: raw/FAT write, sustained stream read+write, full duplex, memory",
        .func    = &cmd_sd_diag,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

// ---------------------------------------------------------------------------
// Console: rec
// ---------------------------------------------------------------------------

static struct {
    struct arg_str *op;
    struct arg_str *backend;
    struct arg_str *path;
    struct arg_int *sr;
    struct arg_int *bits;
    struct arg_int *ch;
    struct arg_end *end;
} rec_args;

static int cmd_rec(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&rec_args);
    if (nerrors) { arg_print_errors(stderr, rec_args.end, argv[0]); return 1; }

    const char *op   = rec_args.op->count      ? rec_args.op->sval[0]      : "status";
    const char *be   = rec_args.backend->count ? rec_args.backend->sval[0] : "flash";
    const char *path = rec_args.path->count    ? rec_args.path->sval[0]    : NULL;
    // Defaults follow the tap's TDM-aware format (record_start overrides
    // mismatches anyway for audio-source takes — these keep the print honest).
    uint16_t def_ch, def_bits;
    tap_format(audio_store_get_record_tap(), &def_ch, &def_bits);
    uint32_t sr   = rec_args.sr->count   ? (uint32_t)rec_args.sr->ival[0]   : FS_HZ;
    uint16_t bits = rec_args.bits->count ? (uint16_t)rec_args.bits->ival[0] : def_bits;
    uint16_t ch   = rec_args.ch->count   ? (uint16_t)rec_args.ch->ival[0]   : def_ch;

    audio_backend_t backend = (strcmp(be, "sd") == 0) ? AUDIO_BACKEND_SD : AUDIO_BACKEND_FLASH;

    if (strcmp(op, "start") == 0) {
        esp_err_t err = audio_store_record_start(backend, path, sr, bits, ch,
                                                 AUDIO_REC_SRC_AUDIO);
        if (err == ESP_ERR_INVALID_STATE) {
            // Several states map to this error — name the right one instead of
            // guessing (losing a concurrent-start race or an upload/live
            // conflict used to print the unrelated flash-playback message).
            if (s_rec_storing || s_rec_starting || s_writer_armed)
                printf("rec start failed: a recording is active or another start/stop is in progress — try 'rec status'\n");
            else if (audio_store_is_playing() && backend == AUDIO_BACKEND_FLASH)
                printf("rec start failed: cannot record to flash while playing from flash — run 'play stop' first\n");
            else
                printf("rec start failed: recorder busy (live capture or upload owns the ring?) — see 'rec status'\n");
            return 1;
        }
        if (err != ESP_OK) { printf("rec start failed: %s\n", esp_err_to_name(err)); return 1; }
        printf("recording [%s] sr=%u bits=%u ch=%u\n",
               backend == AUDIO_BACKEND_FLASH ? "flash" : (path ? path : "/sdcard/rec.wav"),
               (unsigned)sr, (unsigned)bits, (unsigned)ch);
        return 0;
    }
    if (strcmp(op, "stop") == 0) {
        audio_store_record_stop();
        return 0;
    }
    if (strcmp(op, "tap") == 0) {
        // 'be' holds the second positional arg here (pre|post).
        if (rec_args.backend->count) {
            if      (strcmp(be, "post") == 0) audio_store_set_record_tap(AUDIO_TAP_POST);
            else if (strcmp(be, "pre")  == 0) audio_store_set_record_tap(AUDIO_TAP_PRE);
            else { printf("Usage: rec tap <pre|post>\n"); return 1; }
        }
        printf("record tap = %s (%s)\n",
               audio_store_get_record_tap() == AUDIO_TAP_POST ? "post" : "pre",
               audio_store_get_record_tap() == AUDIO_TAP_POST ? "DSP output" : "raw input");
        return 0;
    }
    if (strcmp(op, "live") == 0) {
        if (rec_args.backend->count && strcmp(be, "stop") == 0) {
            audio_store_live_close(); printf("live capture closed\n"); return 0;
        }
        audio_store_live_open();
        printf("live capture armed (tap=%s) — GET /api/rec/live to stream\n",
               audio_store_get_record_tap() == AUDIO_TAP_POST ? "post" : "pre");
        return 0;
    }
    if (strcmp(op, "status") == 0) {
        uint64_t captured = s_rec_storing ? (s_rec_head - s_rec_store_start)
                                          : (s_rec_store_end - s_rec_store_start);
        float secs = (s_sample_rate && s_channels && s_bits)
            ? (float)captured / (float)(s_sample_rate * s_channels * (s_bits / 8)) : 0.0f;
        uint64_t backlog = s_rec_head - s_rec_store_tail;   // unstored bytes in ring
        // i2s_ovf = RX DMA overruns since the take started: loss upstream of the
        // ring (issue #12) that `dropped` cannot see. Nonzero = spliced audio.
        printf("recording=%d [%s] tap=%s %.1fs  ring backlog=%u/%u KB  dropped=%u i2s_ovf=%u | live=%d live_drops=%u\n",
               s_rec_storing ? 1 : 0,
               s_rec_backend == AUDIO_BACKEND_FLASH ? "flash" : "sd",
               audio_store_get_record_tap() == AUDIO_TAP_POST ? "post" : "pre",
               secs,
               (unsigned)(backlog / 1024), (unsigned)(REC_RING_BYTES / 1024),
               (unsigned)s_dropped_blocks,
               (unsigned)(audio_pipeline_rx_overruns() - s_rec_ovf_start),
               s_rec_live_on ? 1 : 0, (unsigned)s_live_drops);
        return 0;
    }
    if (strcmp(op, "stream") == 0) {
        return (audio_store_stream_file_raw(path ? path : "/sdcard/rec.wav") == ESP_OK) ? 0 : 1;
    }
    if (strcmp(op, "streamhex") == 0) {
        return (audio_store_stream_file_hex(path ? path : "/sdcard/rec.wav") == ESP_OK) ? 0 : 1;
    }

    printf("Usage: rec <start|stop|status|tap|live|stream|streamhex> [flash|sd|pre|post] [path] [sr] [bits] [ch]\n");
    printf("       rec tap <pre|post>   pre = raw input (default), post = DSP output\n");
    printf("       rec start [flash|sd] streams continuously (SD: unbounded; flash: ~38 s)\n");
    printf("       rec live             arm live HTTP capture (GET /api/rec/live)\n");
    printf("       ch/bits default from tap+geometry (TDM pre = all mic channels)\n");
    return 1;
}

// ---------------------------------------------------------------------------
// Console: play
// ---------------------------------------------------------------------------

static struct {
    struct arg_str *op;
    struct arg_str *backend;
    struct arg_str *path;
    struct arg_lit *loop;
    struct arg_end *end;
} play_args;

static int cmd_play(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&play_args);
    if (nerrors) { arg_print_errors(stderr, play_args.end, argv[0]); return 1; }

    const char *op   = play_args.op->count      ? play_args.op->sval[0]      : "status";
    const char *be   = play_args.backend->count ? play_args.backend->sval[0] : "flash";
    const char *path = play_args.path->count    ? play_args.path->sval[0]    : NULL;
    bool loop = (play_args.loop->count > 0);

    audio_backend_t backend = (strcmp(be, "sd") == 0) ? AUDIO_BACKEND_SD : AUDIO_BACKEND_FLASH;

    if (strcmp(op, "start") == 0) {
        esp_err_t err = audio_store_play_start(backend, path, loop);
        if (err != ESP_OK) { printf("play failed: %s\n", esp_err_to_name(err)); return 1; }
        printf("playing [%s] loop=%d\n",
               backend == AUDIO_BACKEND_FLASH ? "flash" : (path ? path : "/sdcard/rec.wav"),
               loop ? 1 : 0);
        return 0;
    }
    if (strcmp(op, "stop")   == 0) { audio_store_play_stop(); printf("stopped\n"); return 0; }
    if (strcmp(op, "status") == 0) {
        printf("playing=%d [%s]  ring=%u KB\n",
               s_play_active ? 1 : 0,
               s_play_backend == AUDIO_BACKEND_FLASH ? "flash" : "sd",
               (unsigned)(s_pb_used / 1024));
        return 0;
    }

    printf("Usage: play <start|stop|status> [flash|sd] [path] [--loop]\n");
    return 1;
}

void register_audio_store_cmds(void)
{
    rec_args.op      = arg_str0(NULL, NULL, "<start|stop|status|tap|live|stream|streamhex>", "operation");
    rec_args.backend = arg_str0(NULL, NULL, "<flash|sd|pre|post>", "backend (flash) or tap (pre|post)");
    rec_args.path    = arg_str0(NULL, NULL, "<path>",       "SD file path");
    rec_args.sr      = arg_int0(NULL, NULL, "<sr>",         "sample rate (default 48000)");
    rec_args.bits    = arg_int0(NULL, NULL, "<bits>",       "bits per sample (default 32)");
    rec_args.ch      = arg_int0(NULL, NULL, "<ch>",         "channels (default 2)");
    rec_args.end     = arg_end(6);
    const esp_console_cmd_t c_rec = {
        .command  = "rec",
        .help     = "Record (streaming): start/stop/status/tap/live/stream/streamhex [flash|sd] [path] [sr] [bits] [ch]; 'rec tap pre|post' selects raw vs DSP-output capture; 'rec live' arms HTTP live capture",
        .func     = &cmd_rec,
        .argtable = &rec_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_rec));

    play_args.op      = arg_str0(NULL, NULL, "<start|stop|status>", "operation");
    play_args.backend = arg_str0(NULL, NULL, "<flash|sd>",          "backend (default: flash)");
    play_args.path    = arg_str0(NULL, NULL, "<path>",              "SD WAV file path");
    play_args.loop    = arg_lit0(NULL, "loop",                      "loop playback");
    play_args.end     = arg_end(4);
    const esp_console_cmd_t c_play = {
        .command  = "play",
        .help     = "Play: start/stop/status [flash|sd] [path] [--loop]",
        .func     = &cmd_play,
        .argtable = &play_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_play));
}
