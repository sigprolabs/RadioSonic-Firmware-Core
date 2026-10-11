// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

#include <stdbool.h>
#include <stddef.h>

// HTTP control panel. One job: serve the embedded web UI page and the
// REST-ish /api/* endpoints, wrapped in the heap-gated start/retry machinery
// the httpd task stack needs on this internal-RAM-starved board. WiFi itself
// (credentials, STA/AP fallback, brownout mitigation, sleep) lives in
// wifi_manager.c, which drives this module's lifecycle via
// web_server_start()/web_server_stop(); web_server never calls back into
// wifi_manager except the read-only status getter (for /api/status) and the
// credential-save helpers behind /api/wifi/*.

// Start the HTTP server. Heap-gated: the attempt is refused — and retried on
// a one-shot timer — unless the largest free internal block fits the httpd
// task stack, because entering httpd_start without that room leaks the bound
// port-80 listen socket (see the block comment in web_server.c). No-op if the
// server is already running. Called by wifi_manager once the radio is up
// (boot bring-up, light-sleep resume).
void web_server_start(void);

// Stop the server: halt the pending retry timer, stop httpd, clear the
// handle, and reset the retry budget so the next start gets fresh attempts.
// Called by wifi_manager on light-sleep suspend and `wifi off`.
void web_server_stop(void);

// True once httpd has come up successfully since boot (sticky — stop/suspend
// do not clear it). This is the flag formerly known as s_wifi_ready:
// wifi_manager gates suspend() and the post-bring-up reconnect cadence on it.
bool web_server_started(void);

// Unused httpd-task stack (bytes) at the deepest point of the SD download
// handler — the path HTTPD_STACK_SIZE is sized for. 0 until a download has run.
// This is the evidence for how far that stack can be trimmed when internal RAM
// is short; do not shrink HTTPD_STACK_SIZE without reading it after a transfer.
size_t web_server_httpd_stack_headroom(void);

// The configured httpd task stack size (HTTPD_STACK_SIZE) — the ledger number
// the headroom above is measured against. One owner; `status` prints both.
size_t web_server_httpd_stack_size(void);
