// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdbool.h>

// Start the CLI task on the Native USB port
void start_console_task(void);

// True when a USB host has enumerated the TinyUSB device (mount state).
// Used by power_sleep to choose tethered-idle vs real light sleep. A
// charge-only supply does not enumerate, so it reads false.
bool console_usb_attached(void);
