// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

// USB serial file transfer ("xfer" console command) for the Web Serial API.
//
// Transport: TinyUSB CDC0 (USB-OTG) — the SAME port as the console REPL; a
// command handler owns the endpoint for the duration of a transfer. Throughput
// is USB-bound, not the nominal baud. Downloads and framing lines ride the
// flow-controlled writer (lossless); `get` is a noise-tolerant line-framed
// base64 protocol with a CRC-32 integrity check — ASCII-safe (immune to
// line-ending translation) and resilient to stray ESP_LOG lines (the host
// ignores any line without the data tag). `getraw`, `live`, and `speed` are
// raw binary on the same port. The full wire spec — the contract for the
// browser/notebook side — is in docs/SERIAL_XFER.md.
//
// Terminal note: baud rate is ignored (native USB CDC) — any value works. If a
// terminal connects but shows no output, it is NOT the baud rate: set Flow
// control to None (PuTTY: Connection > Serial > Flow control = None). RTS/CTS
// or DTR/DSR flow control can suppress output because the CDC control-line
// state gates the console.
//
// Commands (run from the "dsp>" REPL or driven by the host):
//   xfer list                                   — list SD files
//   xfer get    <sd:NAME|flash>                 — download (flash → WAV), base64
//   xfer getraw <sd:NAME>                       — download, raw binary (~900 KB/s)
//   xfer put    <sd:NAME|flash> <size> <crc32hex> — upload
//   xfer live [pre|post]                        — stream live capture until aborted
//   xfer speed [bytes]                          — raw USB throughput probe

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the "xfer" console command. Call once at startup. */
void register_serial_xfer_cmd(void);

#ifdef __cplusplus
}
#endif
