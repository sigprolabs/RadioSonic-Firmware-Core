// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Start the CLI task on the Native USB port
void start_console_task(void);

// True when a USB host has enumerated the TinyUSB device (mount state).
// Used by power_sleep to choose tethered-idle vs real light sleep. A
// charge-only supply does not enumerate, so it reads false.
bool console_usb_attached(void);

// Raw line input for a console command that takes pasted multi-line data (a
// coefficient list, a config block). Call it only from a cmd hook, which runs
// in the console task: it reads the console's own byte stream, with no line
// editing, up to a line end (CR, LF or CRLF), echoing the bytes as they
// arrive so the terminal shows the paste. wait_ms is the time allowed for
// the first byte, idle_ms the quiet time after any byte. Returns the line
// length without the line end, 0 for an empty line; -1 when the input has
// gone quiet (nothing within wait_ms, or idle_ms after the last byte), with
// an unfinished last line, if any, left in buf so a paste without a final
// newline is not lost; -2 when the terminal disconnected. A line longer than
// cap-1 bytes is truncated; buf is always NUL-terminated.
// docs/ADDING_EFFECTS.md ("cmd") shows the loop a paste verb builds on it.
int console_read_line(char *buf, size_t cap, uint32_t wait_ms, uint32_t idle_ms);
