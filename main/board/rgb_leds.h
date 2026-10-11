// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdint.h>

// Initialize the RMT channel and encoder for the SK6805-EC15 LED strip.
// Must be called before rgb_leds_startup_anim().
void rgb_leds_init(void);

// Blocking startup animation (~4 s — 4 LEDs x fade in/out; see hal.h).
// Each of the 4 LEDs fades in/out in turn: red, green, blue, white.
void rgb_leds_startup_anim(void);

// Turn the whole strip off. Safe to call if rgb_leds_init() never ran.
void rgb_leds_all_off(void);

// Set one pixel's color (RGB order, 0-255) in the framebuffer.
// Call rgb_leds_refresh() to push the framebuffer to the strip.
void rgb_leds_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);

// Push the framebuffer to the strip. No-op if rgb_leds_init() never ran.
void rgb_leds_refresh(void);
