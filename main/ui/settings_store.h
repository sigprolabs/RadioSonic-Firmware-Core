// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdint.h>
#include "esp_err.h"

// Persistent settings store (NVS). Saved: the framework fields the core owns
// (active DSP effect by name, input source, in/out gains) plus one text
// section per effect, serialized by that effect's own save_text/load_text
// hooks (dsp_effects.h) — the store knows nothing about any effect's
// parameters, so out-of-tree effects persist exactly like shipped ones.
// NOT saved: the pot assignment/range (`pot`), TDM bring-up state (mixgain,
// codec r63/offset), and WiFi credentials (own NVS keys). What each effect
// persists is its own policy (it may leave out data it can regenerate).
// The TDM slot width persists separately — see settings_get_slot_width_early.
//
// Two slots:
//   * "user"    — the save/load button pair (scratch preset; btn5)
//   * "default" — applied automatically at boot
//
// Front panel (btn5): press = save user, shift+press = load user (falls back
// to the default slot if no user save exists), long-hold = save as boot
// default. CLI mirror: `settings save | load | default | show | clear`.

#define SETTINGS_SLOT_USER    "user"
#define SETTINGS_SLOT_DEFAULT "default"

// Init NVS and apply the "default" slot if one has been saved. Call in
// app_main after codec_init_all() + dsp_init() (it programs the codec and
// the effects' state) and before control_buttons_init().
esp_err_t settings_store_init(void);

// Snapshot the current g_state (+ every effect's save_text) into a slot.
esp_err_t settings_save(const char *slot);

// Load a slot and apply it (codec, DSP config, active effect, button cursor).
// ESP_ERR_NVS_NOT_FOUND if the slot has never been saved.
esp_err_t settings_load(const char *slot);

// Register the `settings` console command.
void register_settings_cmd(void);

// TDM slot-width override, standalone NVS u8 key (not in the settings blob —
// must be readable BEFORE codec/DSP init and survive blob-layout bumps).
// Returns 0 (auto), 16, or 32. Does the idempotent nvs_flash_init dance, so
// it is safe as the very first NVS access at boot (app_main geometry step).
// Set via `settings slotwidth <auto|16|32>`; effective after reboot.
uint8_t settings_get_slot_width_early(void);
