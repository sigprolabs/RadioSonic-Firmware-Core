<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# TinyUSB CDC Console — Bring-up, Root Causes, and New Code

**Status (final, shipped July 2026):** Console complete and hardware-verified
(full smart-mode linenoise over a single native USB port), WiFi re-enabled
(brownout mitigated — TX-power cap + boot settle), and fast binary transfer
shipped (`xfer`, ~900 KB/s — see [docs/SERIAL_XFER.md](SERIAL_XFER.md)). This
document is kept as the bring-up case study: the root causes in §2 are the
teaching content. Sections written mid-effort speak in the tense of their
time; §7-§8 record the resolutions.

Primary file: [main/ui/cli_console.c](../main/ui/cli_console.c).

---

## 1. Why this work happened

The interactive console used to run on the **USB-Serial-JTAG (USJ)** controller,
where `esp_console` + linenoise "just work." But USJ tops out around **~50 KB/s**
(the native-port CPU/IRQ ceiling, esp-usb issue #54) — too slow for streaming audio
capture off the device. The two USB controllers on the ESP32-S3 (USB-Serial-JTAG and
USB-OTG) **share one PHY (GPIO19/20) and are mutually exclusive**, so to use the
faster **TinyUSB CDC** path the console had to move onto a CDC channel.

A prior attempt left the device with **no usable CLI**. linenoise misbehaved over a
hand-rolled CDC VFS, and a series of band-aid patches made it worse. This effort
restarted from a minimal known-good base and built the console up one verifiable
layer at a time.

> Key external fact: **Espressif's `esp_console` REPL officially does not support
> USB-OTG/TinyUSB CDC** as a console transport. This is a hand-built, unsupported
> integration — which is why it has to be assembled carefully.

---

## 2. The debugging journey (what was actually wrong)

The original symptom ("dead console / nothing on the terminal") was **not one bug**.
It was a stack of separate problems, each of which we isolated on hardware:

1. **Watching the wrong COM port.** When the firmware calls
   `tinyusb_driver_install()`, the shared PHY hands off from USB-Serial-JTAG to
   USB-OTG, and the device **re-enumerates as a different USB device with a different
   COM number** (no "connect" chime is guaranteed). On this machine:
   - **USJ port** = USB-Serial-JTAG (flashing + boot logs, until the handoff)
   - **CDC port** = TinyUSB CDC (the console — *this* is the port to watch; it enumerates ~1 s after boot as a NEW serial port)

   `idf.py monitor` cannot drive the TinyUSB CDC port (it issues serial control
   calls the CDC port rejects → `ClearCommError` / "waiting for device to
   reconnect"). Use **PuTTY / RealTerm on the CDC port** for the console; use monitor on
   the USJ port only for the pre-handoff boot log.

2. **TinyUSB device-task starvation (the freeze).** Under load the CDC output froze
   while the port stayed enumerated. Root cause (researched + confirmed in the
   component source): the TinyUSB device task defaults to **priority 5, pinned to
   core 1** — the same core that runs this project's real-time audio pipeline at
   **priority 9–12** (rx=11, dsp=9, tx=12). The USB task got starved exactly when
   load spiked, so USB transfers stopped being serviced. Documented Espressif failure
   mode: *"a long/blocking operation can starve the tinyusb_task, making the device
   unresponsive to the host."* Fixed by moving the task to **core 0, priority 12**
   (`tud_task()` sleeps on its event queue when idle, so the higher priority only
   costs CPU when there's real USB work).

3. **WiFi power-cycle (the reboot, `reset_reason=1` POWERON).** A separate issue that
   showed up once the AP started refusing association ("Association refused
   temporarily"). The endless WiFi retry storm spikes TX current and **browns out a
   marginal USB supply**, causing a full power-on reset every couple seconds —
   independent of the console. Confirmed by disabling `web_server_init()`: with WiFi
   off the board is rock-stable. This remains an **open, separate problem** (power
   supply / AP / WiFi config), not a console bug.

Theories that research **disproved** (so we abandoned them): a blocking 50 ms flush
being fatal (it isn't — flush-timeout is benign per esp-idf #10003), and concurrent
multi-task CDC writes corrupting state (TinyUSB's CDC FIFO is documented thread-safe).

---

## 3. The phased bring-up (`CONSOLE_PHASE`)

The console is built behind a single compile-time selector at the top of the file so
each layer can be flashed and verified before adding the next:

```c
#define CONSOLE_PHASE 4   // 1..4
```

| Phase | Adds | Verified on HW |
|------|------|----------------|
| 1 | Raw CDC only: heartbeat + echo via `tinyusb_cdcacm_*` (no stdio) | TX, RX, DTR detect |
| 2 | stdio redirect: `printf` + `read()` over the custom VFS | printf + echo, stable |
| 3 | `esp_console` + linenoise in **forced dumb mode** | `help`/commands run |
| 4 | `linenoiseProbe` smart/dumb auto-select, reconnect re-banner, log redraw | Tab/arrows/history/autocomplete |

The repo ships with `CONSOLE_PHASE 4` (the full console). Phases 1–3 remain in the
file (guarded by `#if`) as a regression ladder — drop the number to re-isolate a
layer if something breaks later.

---

## 4. New code in `cli_console.c` (detailed)

All of the following lives in the console back-end section of
[main/cli_console.c](../main/cli_console.c).

### `cdc0_usb_install()` — common to all phases
Installs TinyUSB (full-speed OTG) and CDC0. **This is where the PHY moves from USJ to
OTG.** The critical addition is overriding the device-task placement:

```c
tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
tusb_cfg.task.xCoreID  = 0;    // off the audio core (core 1)
tusb_cfg.task.priority = 12;   // serviced promptly under load
ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
```

CDC0 is initialized with two callbacks:
- `cdc0_rx_cb` — fires on RX; gives `s_cdc0_rx_sem` to wake a blocked reader.
- `cdc0_line_state_changed` — on **DTR 0→1** (terminal opened) sets `s_reprobe` and
  wakes the reader, so the console re-probes and re-banners on connect.

### The custom VFS (`cdc0_vfs_*`) — Phase 2+
The stock esp_tinyusb CDC VFS is **non-blocking only** (its read returns
`EWOULDBLOCK` and it ignores `O_NONBLOCK` changes), which makes linenoise spin. So we
register our own character VFS at `/dev/cdc0` and `freopen` stdin/stdout/stderr onto
it.

- **`cdc0_vfs_write`** — expands `\n`→`\r\n` into a 128-byte stack buffer, writes in
  **bulk** via `tinyusb_cdcacm_write_queue`, then a **non-blocking flush**
  (`tinyusb_cdcacm_write_flush(..., 0)`). Never waits on the USB endpoint from a
  caller's context. Serialized by `s_cdc0_tx_mtx` (TinyUSB's FIFO is thread-safe, but
  the mutex keeps whole log/print lines from interleaving byte-wise).
- **`cdc0_vfs_read`** — loops `tinyusb_cdcacm_read`; returns bytes when present
  (translating `\r`→`\n`). Behavior when no data:
  - `O_NONBLOCK` set → `EWOULDBLOCK` (this is the `linenoiseProbe` poll path).
  - Otherwise **blocks** on `s_cdc0_rx_sem` and loops — it **must not** return 0 for
    "no data yet," because `linenoiseEdit` submits the line on any read ≤ 0
    (`linenoise.c:921`). Even the **reconnect** event (`s_reprobe`, Phase 4 only) —
    which breaks a blocked edit so the loop re-banners — returns **-1 with
    errno=EIO**, never 0: a 0-return there made dumb mode treat the break as a
    submitted byte and self-feed, which corrupted the heap (fixed 2026-07).
- `cdc0_vfs_fcntl` honors `F_GETFL`/`F_SETFL` so `O_NONBLOCK` actually toggles.

### The linenoise read seam — Phase 2+
linenoise reads the cursor-query reply *and* normal keystrokes through a settable
`read_func`, and only `linenoiseProbe` uses a bare `read()` (with `O_NONBLOCK`
toggled directly). We install our own:

```c
static ssize_t cdc0_console_read(int fd, void *buf, size_t n) { return cdc0_vfs_read(fd, buf, n); }
void linenoiseSetReadCharacteristics(void) {            // overrides linenoise's __weak symbol
    int f = fileno(stdin);
    fcntl(f, F_SETFL, fcntl(f, F_GETFL) & ~O_NONBLOCK); // blocking by default
    linenoiseSetReadFunction(cdc0_console_read);        // stays installed even after probe
}
```

Overriding the weak `linenoiseSetReadCharacteristics()` ensures our `read_func`
survives `linenoiseProbe()` (which would otherwise reset it to the default `read`).

### `cdc0_stdio_redirect()` — Phase 2+
Creates `s_cdc0_tx_mtx`, registers the VFS, `freopen`s the three std streams onto
`/dev/cdc0`, sets **both unbuffered** (`setvbuf(_IONBF)` — the prompt and linenoise
escapes contain no `\n`, so line-buffering would strand them), installs the read
function, and (Phase 4) installs the log-redraw hook.

### `console_log_vprintf()` — Phase 4
Installed via `esp_log_set_vprintf`. While the console is blocked in `linenoise()`
(`s_editing` true), it emits `\r\x1b[K` (CR + erase-line) before the log text so an
async log from another task lands on its own clean line instead of corrupting the
edited line; linenoise redraws the prompt + buffer on the next keystroke.

### `console_probe()` — Phase 4
Bounded wait for DTR (so the banner isn't emitted into a disconnected port), then
`linenoiseProbe()` (sends `ESC[5n`): a terminal that answers → **smart mode** (full
history/editing/Tab/hints); one that doesn't (e.g. Web Serial / RealTerm) → **dumb
mode** (`linenoiseDumb` never issues cursor queries, so it can't hang). Re-runs on
every reconnect (`s_reprobe`).

### `console_task()`
Phase-branched. Phase 4 path: `cdc0_usb_install()` → `cdc0_stdio_redirect()` →
`esp_console_init()` → `register_console_commands()` → linenoise setup →
`console_probe()` → the classic `linenoise("dsp> ")` → `esp_console_run()` loop
(driven manually, **not** `esp_console_new_repl_*`, which would re-open the device
VFS and re-rotate the fds).

---

## 5. Config changes

In **`sdkconfig.defaults`** (the live `sdkconfig` is gitignored):
- `CONFIG_ESP_CONSOLE_NONE=y` — nothing else may pre-bind stdio fds 0/1/2.
- `CONFIG_TINYUSB_CDC_ENABLED=y`, `CONFIG_TINYUSB_CDC_COUNT=1`, RX/TX/EP bufsize 512.
- `CONFIG_TINYUSB_MODE_DMA=y` (kept; correct for the throughput goal).

**Open config decision:** the live `sdkconfig` currently keeps
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` (re-enabled during debugging so
boot logs appear on the USJ port before the PHY handoff — genuinely useful). The original
plan was `CONFIG_ESP_CONSOLE_SECONDARY_NONE`. Decide at cleanup.

**Temporary diagnostics still in the tree** (to be removed at cleanup):
- `web_server_init()` is commented out in [main/radiosonic_main.c](../main/radiosonic_main.c) (WiFi off to dodge the POWERON brownout).
- A `reset_reason` `ESP_LOGW` at the top of `console_task`.
- The TinyUSB task `priority = 12` is a debugging value; pick a final number and
  re-verify under WiFi load once #3 (power) is resolved.

---

## 6. How to flash / observe

```
idf.py -p <usj-port> flash   # hold BOOT, tap RESET, release BOOT, then flash
```
- Console: **PuTTY / RealTerm on the CDC port** (it appears ~1 s after
  reset, at the PHY handoff).
- Boot log: `idf.py -p <usj-port> monitor` — only useful until the handoff.
- Flashing without the BOOT button: the `bootloader` console command forces ROM
  download mode (re-enumerates as the JTAG unit).

---

## 7. Is fast Web Serial transfer possible now?

**Yes — shipped (July 2026), on a SINGLE shared CDC0.** The CDC1 plan below was
abandoned: a 2-CDC composite destabilized CDC0 on the dev host, and measurement
showed a second channel adds no throughput. What actually shipped:

1. **`xfer live` + `xfer speed` are raw binary on CDC0** (no base64) — the link
   sustains ~900 KB/s, comfortably real-time for 48 kHz/stereo/32-bit (~375 KB/s).
2. **`xfer get`/`put` remain base64 + CRC-32 line framing** (ASCII-safe, noise
   tolerant) on CDC0.
3. **The upload RX path was migrated** to `tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, ...)`
   — all `usb_serial_jtag_*` usage is gone (the component dependency too).

Historical plan (NOT current — kept for context): CDC_COUNT=2 with raw binary on a
dedicated CDC1. Do **not** implement this; `CONFIG_TINYUSB_CDC_COUNT` is 1 and the
single-channel design is the supported one. See [SERIAL_XFER.md](SERIAL_XFER.md) for
the live host contract.

---

## 8. Verified vs. open

**Verified on hardware:** raw CDC TX/RX, stdio redirect, `printf`/`read` echo,
`esp_console` + all registered commands, smart-mode linenoise (Tab, arrows, history,
autocomplete, hints) on PuTTY.

**All previously-open items are resolved (July 2026):**
- WiFi reboot loop: root cause was a `sys_evt` stack overflow, not power —
  `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4608`; `web_server_init()` restored.
  Brownout mitigations (TX-power cap `wifi power`, retry backoff) kept as
  defense-in-depth.
- TinyUSB task core0/pri12 confirmed by the project-wide priority/affinity pass
  (task map in `radiosonic_main.c`), verified under WiFi + console load.
- `serial_xfer` fully migrated (single CDC0; see section 7).
- `power_sleep.c` tether detection now uses the TinyUSB mount state.
- Reset-reason is now a permanent banner feature (plus coredump-to-flash);
  secondary console pinned to NONE in `sdkconfig.defaults`.
