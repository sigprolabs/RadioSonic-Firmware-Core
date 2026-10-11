// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

#include <stdint.h>

// The canonical 44-byte PCM WAV header — the single source for every path
// that emits one (SD recordings, flash-sample WAV wrapping, the serial live
// stream, and the HTTP live/download endpoints). Reading-side chunk parsing
// lives in audio_store.c (wav_find_chunk).

#ifdef __cplusplus
extern "C" {
#endif

// Bytes of RIFF container overhead that precede the PCM payload but follow the
// 8-byte RIFF id+size: riff_size == WAV_RIFF_OVERHEAD + data_size. (Used by
// audio_store.c's open-ended live headers: 0xFFFFFFFF - WAV_RIFF_OVERHEAD.)
#define WAV_RIFF_OVERHEAD  36u

typedef struct __attribute__((packed)) {
    char     riff_id[4];     // "RIFF"
    uint32_t riff_size;      // 36 + data_size
    char     wave_id[4];     // "WAVE"

    char     fmt_id[4];      // "fmt "
    uint32_t fmt_size;       // 16 for PCM
    uint16_t audio_format;   // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;

    char     data_id[4];     // "data"
    uint32_t data_size;      // bytes of PCM
} wav_header_t;

static inline void wav_make_header(wav_header_t *h,
                                   uint16_t channels,
                                   uint32_t sample_rate,
                                   uint16_t bits_per_sample,
                                   uint32_t data_size_bytes)
{
    h->riff_id[0] = 'R'; h->riff_id[1] = 'I'; h->riff_id[2] = 'F'; h->riff_id[3] = 'F';
    h->wave_id[0] = 'W'; h->wave_id[1] = 'A'; h->wave_id[2] = 'V'; h->wave_id[3] = 'E';

    h->fmt_id[0] = 'f'; h->fmt_id[1] = 'm'; h->fmt_id[2] = 't'; h->fmt_id[3] = ' ';
    h->fmt_size = 16;
    h->audio_format = 1;

    h->num_channels = channels;
    h->sample_rate = sample_rate;
    h->bits_per_sample = bits_per_sample;

    uint16_t bytes_per_sample = (uint16_t)(bits_per_sample / 8);
    h->block_align = (uint16_t)(channels * bytes_per_sample);
    h->byte_rate = sample_rate * h->block_align;

    h->data_id[0] = 'd'; h->data_id[1] = 'a'; h->data_id[2] = 't'; h->data_id[3] = 'a';
    h->data_size = data_size_bytes;

    h->riff_size = WAV_RIFF_OVERHEAD + data_size_bytes;
}

#ifdef __cplusplus
}
#endif