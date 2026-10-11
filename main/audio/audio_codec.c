// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "audio_codec.h"
#include "common.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "codec";

// One SPI device handle per codec instance. s_present is the boot probe
// verdict; s_active additionally means "init sequence has run" — register
// writes (gain, sleep/wake, mutes) fan out only to active codecs.
static spi_device_handle_t s_spi[CODEC_COUNT];
static bool s_present[CODEC_COUNT] = { true, false, false };
static bool s_active[CODEC_COUNT]  = { false, false, false };
static bool s_tdm_mode = false;   // set by codec_init_all; selects the R5 PLL P divider
static int  s_slot_bits = 32;     // TDM slot width (memory sample width matches)
// TDM base data offset in BCLKs (R28 of the main codec; expansions add
// 2*slot_bits per pair).
//
// MEASURED 0 on hardware 2026-07-27 (Rev 2 bench, main + exp1, TDM 4x32).
// The pre-measurement guess was 1, to match the ESP driver's PCM-short
// bit_shift — that guess was WRONG. At base 1 (and 2, and 3) every slot
// arrived shifted by one BCLK: the sign bit was lost and the MSB filled from
// the neighbouring slot, so roughly half of all samples read as ~2^31 instead
// of small mic-level values. Proof it was framing and not loud audio: 100% of
// those huge samples became small again when the MSB was flipped back.
// At base 0 the corruption is exactly zero on all four channels, repeatably,
// with per-channel noise floors of 5.6-12.2 M.
static uint8_t s_tdm_base = 0;
static bool s_r63_straight = false;  // R63 mapping in use (std: cross; TDM: expected straight)

// P0 R63 (DAC data-path setup): CROSS-MAPPED — D5:D4=10 (Left DAC <- RIGHT
// channel interface data), D3:D2=10 (Right DAC <- LEFT), TI SLAA557 p.110.
// This compensates for swapped jack pins.
#define CODEC_R63_DAC_ON          0xE8  // L+R DAC powered, cross-mapped (TX-quirk comp)
#define CODEC_R63_DAC_ON_STRAIGHT 0xD4  // L+R DAC powered, straight mapping (TDM candidate)
#define CODEC_R63_DAC_OFF         0x28  // DACs powered down, data-path bits retained

static uint8_t r63_on_value(void)
{
    return s_r63_straight ? CODEC_R63_DAC_ON_STRAIGHT : CODEC_R63_DAC_ON;
}

// --- Register access: one lock, one shadow -----------------------------------
// The AIC3204 selects the page through register 0, so a page write and the
// register write after it must be ONE atomic step. The console, httpd,
// ctrl_btn and pot tasks all write the codec; without the lock an interleaved
// page select from another task lands a write on the wrong page (a page-1
// routing value on page 0 repurposes MISO/DIN). Recursive because the
// probe/reattach paths nest. Control path only — no audio task touches SPI.
static SemaphoreHandle_t s_spi_lock;
static uint32_t s_spi_errors;   // failed spi_device_transmit calls since boot

// What the firmware believes the codec holds: every value written to pages
// 0/1, so `codec check` can read them back and compare (the "is the codec
// actually in the state status reports?" question). P0 R0 (page select) and
// P0 R1 (self-clearing soft reset) are never shadowed; a soft reset clears the
// codec's shadow because the registers return to their defaults.
static uint8_t  s_shadow[CODEC_COUNT][2][128];
static uint32_t s_shadow_valid[CODEC_COUNT][2][4];

static void spi_lock(void)   { if (s_spi_lock) xSemaphoreTakeRecursive(s_spi_lock, portMAX_DELAY); }
static void spi_unlock(void) { if (s_spi_lock) xSemaphoreGiveRecursive(s_spi_lock); }

// Helper: Write 8-bit register
// Frame Format: [Register Address << 1] [Value]
static esp_err_t aic_write_byte(codec_id_t id, uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { (uint8_t)(reg << 1), val };
    uint8_t rx[2] = {0}; // Dummy rx buffer required for full-duplex SPI transaction
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx
    };
    esp_err_t ret = spi_device_transmit(s_spi[id], &t);
    if (ret != ESP_OK) s_spi_errors++;
    return ret;
}

static esp_err_t aic_read_byte(codec_id_t id, uint8_t reg, uint8_t *val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) | 0x01), 0x00 }; // read bit in bit0
    uint8_t rx[2] = { 0, 0 };

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t ret = spi_device_transmit(s_spi[id], &t);
    if (ret == ESP_OK) {
        *val = rx[1]; // data comes back in 2nd byte
    } else {
        s_spi_errors++;
    }
    return ret;
}

// Helper: Select Page, then Write Register (atomic, shadowed)
static void aic_write_page(codec_id_t id, uint8_t page, uint8_t reg, uint8_t val) {
    spi_lock();
    aic_write_byte(id, 0x00, page); // Reg 0 is always Page Select
    aic_write_byte(id, reg, val);
    if (page <= 1 && reg < 128 && reg != 0) {
        if (page == 0 && reg == 1) {
            if (val & 0x01) memset(s_shadow_valid[id], 0, sizeof(s_shadow_valid[id]));
        } else {
            s_shadow[id][page][reg] = val;
            s_shadow_valid[id][page][reg >> 5] |= 1u << (reg & 31);
        }
    }
    spi_unlock();
}

// Helper: Select Page, then Read Register (atomic)
static esp_err_t aic_read_page(codec_id_t id, uint8_t page, uint8_t reg, uint8_t *val) {
    spi_lock();
    esp_err_t ret = aic_write_byte(id, 0x00, page);
    if (ret == ESP_OK) ret = aic_read_byte(id, reg, val);
    spi_unlock();
    return ret;
}

// Attach all codec CS lines to the shared SPI bus. The expansion devices run a
// slower control clock than the main codec: their SPI runs through the
// expansion connector wiring, so start conservative (readback probing makes
// integrity failures self-evident).
static SemaphoreHandle_t s_route_lock;   // see "Input routing" below

void codec_init_spi(void) {
    if (!s_spi_lock) s_spi_lock = xSemaphoreCreateRecursiveMutex();  // first call: boot, single-threaded
    if (!s_route_lock) s_route_lock = xSemaphoreCreateRecursiveMutex();
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000, // Main codec control clock (on-board traces)
        .mode = 1,                         // CPOL=0, CPHA=1 (Required for AIC3204)
        .spics_io_num = AIC_CS_PIN,
        .queue_size = 4,
        .cs_ena_pretrans = 2,              // Setup time
        .cs_ena_posttrans = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_USED, &devcfg, &s_spi[CODEC_MAIN]));

#ifdef CODEC_1_CS_PIN
    devcfg.clock_speed_hz = 10 * 1000 * 1000; // Off-board wiring margin
    devcfg.spics_io_num   = CODEC_1_CS_PIN;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_USED, &devcfg, &s_spi[CODEC_EXP1]));
    devcfg.spics_io_num   = CODEC_2_CS_PIN;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_USED, &devcfg, &s_spi[CODEC_EXP2]));
#endif
    ESP_LOGI(TAG, "AIC3204 Attached to SPI Bus");
}

// CS pin for a codec instance, so bring-up code can address the pins without
// duplicating the per-revision #ifdefs. Returns false if that instance has no
// CS on this board revision.
bool codec_cs_pin(codec_id_t id, int *pin)
{
    switch (id) {
        case CODEC_MAIN: *pin = AIC_CS_PIN; return true;
#ifdef CODEC_1_CS_PIN
        case CODEC_EXP1: *pin = CODEC_1_CS_PIN; return true;
        case CODEC_EXP2: *pin = CODEC_2_CS_PIN; return true;
#endif
        default: return false;
    }
}

// Park a CS pin at a static level for continuity checking with a DMM.
//
// Bring-up aid: every expansion-codec fault (no 3.3 V, open CS, open MISO,
// codec strapped to I2C, held reset) reads back identically as 0xFF over SPI,
// so SPI alone cannot localise them. A static level is measurable at both the
// ESP pin and the far side of J5 with an ordinary meter — no scope, no
// triggering on CS pulses.
//
// This detaches the pin from the SPI peripheral's CS routing; the bus stays
// broken for that codec until codec_spi_reattach() restores it.
bool codec_cs_park(codec_id_t id, int level)
{
    int pin;
    if (!codec_cs_pin(id, &pin)) return false;
    spi_lock();
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level ? 1 : 0);
    spi_unlock();
    return true;
}

// Detach and re-attach every codec device.
//
// Required after any temporary device has existed on a codec's CS pin:
// spi_bus_add_device() re-points that pin at the new device's hardware CS
// slot, and spi_bus_remove_device() does NOT give it back — the original
// handle survives but its CS never asserts again, so every subsequent read
// returns 0xFF while writes silently go nowhere. That failure mode is what
// made the `codec probe` diagnostic look like dead MISO hardware
// (2026-07-27); the probe was breaking the bus it was measuring.
//
// Control-path only — no audio task touches SPI, so this is safe to call
// from the console while the pipeline runs.
void codec_spi_reattach(void)
{
    spi_lock();   // no other task may use a handle while it is being replaced
    for (int id = 0; id < CODEC_COUNT; id++) {
        if (s_spi[id]) {
            spi_bus_remove_device(s_spi[id]);
            s_spi[id] = NULL;
        }
    }
    codec_init_spi();
    spi_unlock();
}

// Presence probe: write two distinct values to the page-select register and
// read each back. An absent board leaves MISO at its pull level (constant), so
// it cannot echo both values; a present AIC3204 echoes page-select exactly.
// Leaves the codec on page 0. Safe pre-reset and without MCLK (SPI control is
// static logic — proven by codec_test_read at boot).
static bool codec_probe(codec_id_t id)
{
    uint8_t v1 = 0, v2 = 0;
    spi_lock();
    bool ok = aic_write_byte(id, 0x00, 0x01) == ESP_OK &&
              aic_read_byte(id, 0x00, &v1)   == ESP_OK &&
              aic_write_byte(id, 0x00, 0x00) == ESP_OK &&
              aic_read_byte(id, 0x00, &v2)   == ESP_OK;
    spi_unlock();
    return ok && (v1 == 0x01) && (v2 == 0x00);
}

// Diagnostic re-probe (CLI `codec probe`): same sequence as codec_probe but
// exposes the raw readback bytes and never mutates s_present — presence and
// I2S geometry are boot-time decisions; a board detected here still needs a
// reboot to enter TDM. Raw-byte reading guide: FF/FF = MISO never driven
// (board power / CS routing / MISO wiring), 00/00 = MISO stuck low,
// wrong echo = SPI integrity/timing through J5, EE = SPI driver error.
bool codec_probe_raw(codec_id_t id, uint8_t *v1, uint8_t *v2)
{
    *v1 = *v2 = 0xEE;
    if (id < 0 || id >= CODEC_COUNT || !s_spi[id]) return false;
    bool ok = true;
    spi_lock();
    ok &= (aic_write_byte(id, 0x00, 0x01) == ESP_OK);
    ok &= (aic_read_byte(id, 0x00, v1)    == ESP_OK);
    ok &= (aic_write_byte(id, 0x00, 0x00) == ESP_OK);
    ok &= (aic_read_byte(id, 0x00, v2)    == ESP_OK);
    spi_unlock();
    return ok && (*v1 == 0x01) && (*v2 == 0x00);
}

// Raw readback at an explicit control clock (CLI `codec probe`): attaches a
// temporary device at `hz` on the codec's own CS and repeats the page-select
// round trip, reporting via *actual_khz the clock the SPI driver really
// produced (it divides 80 MHz, so requests are rounded down hard: 6 MHz ->
// 5 MHz, 14 MHz -> 13.3 MHz).
//
// Sweeping this is how the read ceiling gets MEASURED instead of assumed.
// Writes are unaffected by the clock in the range we use, which is why codec
// init and audio work fine even when reads fail — a full-duplex read has to
// survive the whole round trip (SCLK out through the GPIO matrix -> AIC3204
// SDOUT prop delay -> MISO back in) inside one half period, and nothing in
// this firmware depended on a read until the TDM presence probe.
// Two measurements, both 2026-07-27 on the Rev 2 bench, tell the story: with
// the MIS-PINNED expansion connector attached (pre-rework J5) the main codec
// echoed only at 1 MHz and returned FF/FF at 20 MHz; with J5 off/reworked,
// reads are clean at every clock 1-20 MHz. So there is no intrinsic
// read-timing ceiling below the AIC3204's own tsck 50 ns / 20 MHz limit —
// the apparent ceiling was bus loading from the mis-wired connector. (The
// `codec probe` guide text in cli_console.c states the same conclusion.)
bool codec_probe_raw_at(codec_id_t id, int hz, int *actual_khz,
                        uint8_t *v1, uint8_t *v2)
{
    *v1 = *v2 = 0xEE;
    if (actual_khz) *actual_khz = 0;
    int cs;
    if (!codec_cs_pin(id, &cs)) return false;
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz  = hz,
        .mode            = 1,
        .spics_io_num    = cs,
        .queue_size      = 1,
        .cs_ena_pretrans = 2,
        .cs_ena_posttrans= 1,
    };
    spi_device_handle_t dev;
    spi_lock();   // held across the temp device's life and the reattach below
    if (spi_bus_add_device(SPI_HOST_USED, &devcfg, &dev) != ESP_OK) { spi_unlock(); return false; }
    if (actual_khz) spi_device_get_actual_freq(dev, actual_khz);

    uint8_t seq[2][2] = { { 0x00 << 1, 0x01 }, { 0x00 << 1, 0x00 } };  // write page 1, then 0
    uint8_t *out[2] = { v1, v2 };
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        uint8_t rxw[2] = {0};
        spi_transaction_t tw = { .length = 16, .tx_buffer = seq[i], .rx_buffer = rxw };
        ok &= (spi_device_transmit(dev, &tw) == ESP_OK);
        uint8_t txr[2] = { (0x00 << 1) | 0x01, 0x00 }, rxr[2] = {0};
        spi_transaction_t tr = { .length = 16, .tx_buffer = txr, .rx_buffer = rxr };
        ok &= (spi_device_transmit(dev, &tr) == ESP_OK);
        if (ok) *out[i] = rxr[1];
    }
    spi_bus_remove_device(dev);
    // Restore the production handle's CS routing before returning: the
    // temporary device STEALS the pin's GPIO-matrix routing and remove_device
    // does not give it back — leaving it stolen made every later read on the
    // real handle return 0xFF, which was misdiagnosed as "MISO is dead" on
    // 2026-07-27. A public API must not preserve that footgun; the extra bus
    // churn is nothing on this diagnostic path.
    codec_spi_reattach();
    spi_unlock();
    // Leave the codec on page 0 regardless of outcome.
    return ok && (*v1 == 0x01) && (*v2 == 0x00);
}

// Boot-time main-codec probe result — for truthful REPORTING only (`status`);
// presence/behavior never depend on it (see the never-mark-absent policy).
static bool s_main_probe_ok = true;

int codec_probe_all(void)
{
#ifdef CODEC_1_CS_PIN
    s_present[CODEC_EXP1] = codec_probe(CODEC_EXP1);
    s_present[CODEC_EXP2] = codec_probe(CODEC_EXP2);
#endif
    // Main codec is load-bearing: probe for the log, but never mark it absent
    // (a false negative must not disable the whole audio path). The REPORT
    // still tells the truth, though — "main OK" was previously printed even
    // two lines after the FAILED error.
    s_main_probe_ok = codec_probe(CODEC_MAIN);
    bool main_ok = s_main_probe_ok;
    if (!main_ok) {
        ESP_LOGE(TAG, "Main codec probe FAILED — check SPI/power");
    }
    ESP_LOGI(TAG, "Codec probe: main %s, exp1 %s, exp2 %s",
             main_ok ? "OK" : "FAILED (continuing anyway)",
             s_present[CODEC_EXP1] ? "present" : "--",
             s_present[CODEC_EXP2] ? "present" : "--");
    return codec_count();
}

bool codec_main_probe_ok(void) { return s_main_probe_ok; }

bool codec_present(codec_id_t id)
{
    return (id >= 0 && id < CODEC_COUNT) ? s_present[id] : false;
}

int codec_count(void)
{
    int n = 0;
    for (int id = 0; id < CODEC_COUNT; id++) {
        if (s_present[id]) n++;
    }
    return n;
}

// --- Input routing: the LINE IN "no input" fix ---------------------------------
//
// THE FAULT (Rev A SN 0234 2026-09-28; also found in Rev 3 U1 DVT captures from
// 2026-08-10/12/20): the MICPGA gets stuck when it is connected to input pins
// whose 10 uF coupling caps sit far from the input common mode — pins held at
// ~0 V, the ADC digitising ~-70 dBFS noise plus a DC offset, the source ~66 dB
// down, EVERY register (routing, power, R36/R62 flags) reading correct — until
// a power cycle. At power-up every cap starts at 0 V, and unrouted pins float
// (SLAA557: "undefined"), so routing onto them is the trigger. On the live
// fault, un-routing + weakly driving the pins to CM for 3 s + re-routing
// recovered it; a 100 ms drive once produced the stuck state instead.
//
// THE FIX: P1 R58 (SLAA557 5.3.32: D7 IN1L, D6 IN1R, D5 IN2L, D4 IN2R, D3 IN3L,
// D2 IN3R — "weakly driven to common mode, use when not routing") holds every
// unrouted input at CM at all times, and no pin is routed until it has been
// held for s_precharge_ms. Held time is tracked per pin, so an ordinary
// mic<->diff switch (the new pins have been held all along) costs nothing;
// boot, wake from sleep and `codec recover` pay the full precharge. The input
// guard (input_guard.c) backs this up for anything that knocks already-routed
// pins off CM (source pops, overdrive, ESD).
#define R58_UNROUTED_DIFF   0xC0   // diff routes IN2/IN3: hold IN1L/IN1R
#define R58_UNROUTED_MIC    0x3C   // mic routes IN1: hold IN2L/IN2R/IN3L/IN3R
#define R58_ALL_INPUTS      0xFC
#define R58_ROUTED_MIC      R58_UNROUTED_DIFF   // the pins mic mode routes
#define R58_ROUTED_DIFF     R58_UNROUTED_MIC    // the pins diff mode routes

// 3 s is the only precharge proven on the live fault. The R58 drive's time
// constant is unmeasured — tune with `codec precharge <ms>` after measuring
// (scope the codec side of C19 rising from 0 V during `codec recover`).
#define PRECHARGE_DEFAULT_MS 3000
static volatile uint32_t s_precharge_ms = PRECHARGE_DEFAULT_MS;

// Per pin: when R58 started holding it at CM (esp_timer us; 0 = not held, or
// CM not trustworthy since). Index 0..5 = R58 D7..D2 = IN1L..IN3R.
static int64_t s_held_since[6];
static uint8_t s_r58;

// One routing sequence at a time (s_route_lock, created in codec_init_spi):
// the button, web, CLI, settings and guard tasks all route, and a sequence
// spans a multi-second wait. Sleep/wake/init/ADC-cycle/unmute hold it too —
// every writer of the registers a sequence snapshots and restores (P0 R81/R82)
// must, or a sequence can restore a stale value over it.
static volatile bool     s_routing;       // a sequence is running
static volatile uint32_t s_route_epoch;   // ++ after every sequence, re-init, sleep
static bool         s_routed_ok;          // a completed sequence routed s_routed_mode
static input_mode_t s_routed_mode;
static bool         s_asleep;             // between codec_sleep and codec_wake

static void route_lock(void)   { if (s_route_lock) xSemaphoreTakeRecursive(s_route_lock, portMAX_DELAY); }
static void route_unlock(void) { if (s_route_lock) xSemaphoreGiveRecursive(s_route_lock); }

static bool shadow_get(codec_id_t id, uint8_t page, uint8_t reg, uint8_t *val)
{
    if (!(s_shadow_valid[id][page][reg >> 5] & (1u << (reg & 31)))) return false;
    *val = s_shadow[id][page][reg];
    return true;
}

static void r58_set(uint8_t mask)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < 6; i++) {
        uint8_t bit = (uint8_t)(0x80 >> i);
        if (!(mask & bit))       s_held_since[i] = 0;
        else if (!(s_r58 & bit)) s_held_since[i] = now;   // newly held
    }
    s_r58 = mask;
    aic_write_page(CODEC_MAIN, 1, 58, mask);
}

// CM can't be trusted any more (soft reset, sleep): every pin starts over.
static void r58_forget(void)
{
    memset(s_held_since, 0, sizeof(s_held_since));
    s_r58 = 0;
}

// Charge time still missing on the pins in `routed`, against `precharge_ms`.
static uint32_t precharge_remaining_ms(uint8_t routed, uint32_t precharge_ms)
{
    int64_t now = esp_timer_get_time(), need = (int64_t)precharge_ms * 1000, left = 0;
    for (int i = 0; i < 6; i++) {
        if (!(routed & (0x80 >> i))) continue;
        int64_t held = s_held_since[i] ? now - s_held_since[i] : 0;
        if (need - held > left) left = need - held;
    }
    return (uint32_t)((left + 999) / 1000);
}

// MICBIAS + the four routing registers for one mode. MICBIAS R51 = 0x68
// (D5:D4=10, D3=1) is 2.075 V at the default 0.75 V CM, 2.5 V at 0.9 V CM
// (SLAA557 5.3.25 — it scales with the common mode).
static void write_routing(bool mic)
{
    aic_write_page(CODEC_MAIN, 1, 51, mic ? 0x68 : 0x00);
    if (mic) {
        aic_write_page(CODEC_MAIN, 1, 52, 0x40);   // IN1L -> Left P, 10k
        aic_write_page(CODEC_MAIN, 1, 55, 0x40);   // IN1R -> Right P, 10k
        aic_write_page(CODEC_MAIN, 1, 54, 0x01);   // Left M-terminal
        aic_write_page(CODEC_MAIN, 1, 57, 0x40);   // Right M-terminal
    } else {   // INPUT_LINE_DIFF — the only line mode (see input_mode_t)
        // Line jack J1 (schematic nets): LINE_IN_L -> IN2L, LINE_IN_R -> IN3R,
        // LINE_IN_SLEEVE -> both IN2R and IN3L, so each channel is measured
        // against the source's own ground and ground-loop noise cancels.
        // On Rev 3 / Rev A the PJ-3200 footprint puts the plug TIP on the pad
        // the schematic calls R, so tip lands on the RIGHT PGA here; rx_fill
        // swaps the pair back (LINE_IN_LR_SWAPPED in the board header).
        aic_write_page(CODEC_MAIN, 1, 52, 0x10);   // IN2L -> Left +, 10k
        aic_write_page(CODEC_MAIN, 1, 54, 0x10);   // IN2R -> Left -, 10k (CM paths off)
        aic_write_page(CODEC_MAIN, 1, 55, 0x04);   // IN3R -> Right +, 10k
        aic_write_page(CODEC_MAIN, 1, 57, 0x04);   // IN3L -> Right -, 10k (CM paths off)
    }
}

// The routing sequence (caller holds s_route_lock). ADC muted and off, nothing
// routed, every pin held at CM with the reference FORCED on (P1 R123 D2: with
// the on-demand setting CM only exists while some analog block is up — not
// the case on wake), wait out whatever of `precharge_ms` the pins about to be
// routed still lack, then route, keep the unrouted pins held, re-apply gains
// and put the ADC in the state the caller asks for.
static void inputs_route(uint32_t precharge_ms, bool adc_on_after, uint8_t r82_after)
{
    s_routing = true;
    bool mic = (g_state.input_mode == INPUT_MIC);

    aic_write_page(CODEC_MAIN, 0, 82, 0x88);       // ADC mute
    aic_write_page(CODEC_MAIN, 0, 81, 0x00);       // ADC off
    aic_write_page(CODEC_MAIN, 1, 52, 0x00);       // nothing routed...
    aic_write_page(CODEC_MAIN, 1, 54, 0x00);
    aic_write_page(CODEC_MAIN, 1, 55, 0x00);
    aic_write_page(CODEC_MAIN, 1, 57, 0x00);
    aic_write_page(CODEC_MAIN, 1, 123, 0x05);      // reference forced on (40 ms)
    r58_set(R58_ALL_INPUTS);                       // ...every pin held at CM
    // MICBIAS for the new mode now, so its turn-on transient settles through
    // the mic coupling caps during the wait rather than into a running ADC.
    aic_write_page(CODEC_MAIN, 1, 51, mic ? 0x68 : 0x00);

    uint32_t wait = precharge_remaining_ms(mic ? R58_ROUTED_MIC : R58_ROUTED_DIFF, precharge_ms);
    if (wait) {
        ESP_LOGI(TAG, "Input %s: precharging inputs to CM for %lu ms",
                 mic ? "MIC" : "LINE", (unsigned long)wait);
        vTaskDelay(pdMS_TO_TICKS(wait));
    } else {
        ESP_LOGI(TAG, "Input %s", mic ? "MIC" : "LINE");
    }

    r58_set(mic ? R58_UNROUTED_MIC : R58_UNROUTED_DIFF);   // release only the pins we route
    write_routing(mic);
    codec_apply_input_gain();
    codec_apply_output_gain();

    if (adc_on_after) {
        aic_write_page(CODEC_MAIN, 0, 81, 0xC0);   // ADC on
        vTaskDelay(pdMS_TO_TICKS(10));
        aic_write_page(CODEC_MAIN, 0, 82, r82_after);
    }
    aic_write_page(CODEC_MAIN, 1, 123, 0x01);      // back to on-demand, 40 ms

    s_routed_ok   = true;
    s_routed_mode = mic ? INPUT_MIC : INPUT_LINE_DIFF;
    s_route_epoch++;
    s_routing = false;
}

// ADC state to restore after a sequence started from normal running.
static void adc_state(bool *on, uint8_t *r82)
{
    uint8_t r81 = 0;
    *on = shadow_get(CODEC_MAIN, 0, 81, &r81) && r81 == 0xC0;
    if (!shadow_get(CODEC_MAIN, 0, 82, r82)) *r82 = 0x00;
}

// Apply settings from global state to hardware. Input ROUTING is main-codec
// only (the expansion boards' inputs are hard-wired to their mics); gains fan
// out below. Blocks for as long as the new pins still need precharge (boot:
// the full s_precharge_ms; a mode switch: usually nothing). While asleep only
// g_state counts — codec_wake routes from it.
void codec_apply_settings(void)
{
    route_lock();
    if (s_asleep) {
        // nothing: the codec is powered down and codec_wake re-routes from g_state
    } else if (s_routed_ok && s_routed_mode == g_state.input_mode) {
        // Same input, already routed by a completed sequence: gains only — a
        // redundant apply (settings load, web refresh) must not cost a
        // precharge dropout. A stuck input is `codec recover`'s (and the input
        // guard's) job.
        codec_apply_input_gain();
        codec_apply_output_gain();
    } else {
        bool on; uint8_t r82;
        adc_state(&on, &r82);
        inputs_route(s_precharge_ms, on, r82);
    }
    route_unlock();
}

// Forced sequence: the currently routed pins become newly held, so they get
// the full precharge before being routed again. `expected_epoch` (the guard's
// entry) aborts if anything else routed, re-initialised or slept since the
// caller looked; UINT32_MAX skips that check.
static bool inputs_recover(uint32_t precharge_ms, uint32_t expected_epoch)
{
    bool done = false;
    route_lock();
    if (!s_asleep && (expected_epoch == UINT32_MAX || expected_epoch == s_route_epoch)) {
        bool on; uint8_t r82;
        adc_state(&on, &r82);
        inputs_route(precharge_ms ? precharge_ms : s_precharge_ms, on, r82);
        done = true;
    }
    route_unlock();
    return done;
}

void codec_recover_inputs(uint32_t precharge_ms)      { inputs_recover(precharge_ms, UINT32_MAX); }
bool codec_recover_inputs_if(uint32_t expected_epoch) { return inputs_recover(0, expected_epoch); }

// The old re-select rung: rewrite MICBIAS + routing + gains for the current
// mode — no ADC cycle, no precharge, R58 untouched — so the recovery ladder
// (reroute -> adc -> reinit) still tells the layers apart.
void codec_reroute_inputs(void)
{
    route_lock();
    if (s_asleep) {
        // nothing: codec_wake re-routes from g_state
    } else if (!s_routed_ok || s_routed_mode != g_state.input_mode) {
        bool on; uint8_t r82;              // the mode changed underneath: those pins
        adc_state(&on, &r82);              // need their precharge, not a bare rewrite
        inputs_route(s_precharge_ms, on, r82);
    } else {
        write_routing(g_state.input_mode == INPUT_MIC);
        codec_apply_input_gain();
        codec_apply_output_gain();
        s_route_epoch++;
    }
    route_unlock();
}

void     codec_set_precharge_ms(uint32_t ms) { s_precharge_ms = ms; }
uint32_t codec_get_precharge_ms(void)        { return s_precharge_ms; }
uint32_t codec_input_epoch(void)             { return s_route_epoch; }
bool     codec_input_routing_busy(void)      { return s_routing; }

// Input gain: MICPGA left/right (P1 R59/60), 0.5 dB steps, 0..47.5 dB.
// Cheap to call alone for a gain-only change (the gain buttons auto-repeat;
// rewriting the whole routing block per 1 dB step would be ~12 SPI writes).
// Fans out to every active codec — expansion mic PGAs track the main gain.
void codec_apply_input_gain(void)
{
    int gain_reg = g_state.input_gain_db * 2;
    if (gain_reg < 0)  gain_reg = 0;
    if (gain_reg > 95) gain_reg = 95;   // register ceiling (+47.5 dB)
    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (id != CODEC_MAIN && !s_active[id]) continue;
        aic_write_page(id, 1, 59, (uint8_t)gain_reg); // Left PGA
        aic_write_page(id, 1, 60, (uint8_t)gain_reg); // Right PGA
    }
}

// Output driver gain: HPL/HPR (P1 R16/17) and LOL/LOR (P1 R18/19).
// D6=0 (unmuted), D5:D0 = gain as 6-bit two's complement, -6..+29 dB
// in 1 dB steps (values outside that range are reserved on the AIC3204).
// Main codec only — expansion output drivers are never powered.
void codec_apply_output_gain(void)
{
    int db = g_state.output_gain_db;
    if (db < CODEC_OUTPUT_GAIN_MIN_DB) db = CODEC_OUTPUT_GAIN_MIN_DB;
    if (db > CODEC_OUTPUT_GAIN_MAX_DB) db = CODEC_OUTPUT_GAIN_MAX_DB;
    uint8_t v = (uint8_t)(db & 0x3F);
    aic_write_page(CODEC_MAIN, 1, 16, v); // HPL
    aic_write_page(CODEC_MAIN, 1, 17, v); // HPR
    aic_write_page(CODEC_MAIN, 1, 18, v); // LOL
    aic_write_page(CODEC_MAIN, 1, 19, v); // LOR
}

// DAC digital volume (P0 R65/R66): 8-bit two's complement, 0.5 dB steps.
// The pot's volume target. It sits after the record taps, so recordings keep
// their level, and the DAC soft-steps each 0.5 dB, so a knob turn does not
// zipper. Held here so codec re-init and the post-sleep fade-in restore it.
static volatile int s_dac_vol = 0;   // 0.5 dB units

void codec_set_dac_volume(int half_db)
{
    if (half_db < CODEC_DAC_VOL_MIN) half_db = CODEC_DAC_VOL_MIN;
    if (half_db > CODEC_DAC_VOL_MAX) half_db = CODEC_DAC_VOL_MAX;
    s_dac_vol = half_db;
    aic_write_page(CODEC_MAIN, 0, 65, (uint8_t)half_db); // Left DAC volume
    aic_write_page(CODEC_MAIN, 0, 66, (uint8_t)half_db); // Right DAC volume
}

int codec_get_dac_volume(void) { return s_dac_vol; }

// Clock tree (Target: 48kHz fs). PLL output is 98.304 MHz in both modes:
//   std: MCLK 12.288 MHz, P=1, R=1, J=8  -> R5 = 0x91
//   tdm: MCLK 24.576 MHz, P=2, R=1, J=8  -> R5 = 0xA1 (PLL input after P
//        divider must stay <= 20 MHz, hence P=2 at the doubled MCLK)
// Every divider below the PLL is identical in both modes.
static void codec_init_clocks(codec_id_t id, bool tdm)
{
    aic_write_page(id, 0, 4, 0x03);              // clock mux: PLL_CLKIN=MCLK, CODEC_CLKIN=PLL_CLK
    aic_write_page(id, 0, 5, tdm ? 0xA1 : 0x91); // PLL power up + P/R (P per mode, R=1)
    aic_write_page(id, 0, 6, 0x08);              // PLL J = 8

    // Dividers: NDAC=8, MDAC=2, DOSR=128
    aic_write_page(id, 0, 11, 0x88);
    aic_write_page(id, 0, 12, 0x82);
    aic_write_page(id, 0, 14, 0x80); // DOSR = 128

    // ADC Dividers: NADC=8, MADC=2, AOSR=128
    aic_write_page(id, 0, 18, 0x88);
    aic_write_page(id, 0, 19, 0x82);
    aic_write_page(id, 0, 20, 0x80); // AOSR = 128
}

// Audio interface (P0 R27/R28).
//   std: I2S, 32-bit, DOUT always driven                       -> R27 = 0x30
//   tdm: DSP mode + word length + DOUT tri-state outside slots -> R27 = 0x71 (32-bit)
//        or 0x41 (16-bit); R28 = data-slot offset in BCLKs (0 / 2*bits / 4*bits
//        for main / exp1 / exp2 — the multi-codec shared-bus arrangement).
static void codec_init_interface(codec_id_t id, bool tdm, int slot_bits, uint8_t offset_bclks)
{
    if (!tdm) {
        aic_write_page(id, 0, 27, 0x30);
    } else {
        aic_write_page(id, 0, 27, (slot_bits == 16) ? 0x41 : 0x71);
        aic_write_page(id, 0, 28, offset_bclks);
    }
}

// Full-chip common mode, P1 R10 D6 (SLAA557 5.3.8). SLAA557 3.5: "For analog
// supply voltages below 1.8V, a common mode voltage of 0.75V must be used" —
// and this board runs AVDD from the codec's internal LDO at 1.72 V nominal
// (P1 R2 = 0x01; 1.77 V measured). Every revision ran 0.9 V until 2026-09-28,
// with the input stage that locks up (see "Input routing") being exactly the
// block that needs the headroom. (REF carries CM: Rev A SN 0234 reads 0.74 V
// on a DMM at 0.75 V CM; the earlier 0.83 V at 0.9 V was a scope reading
// within that instrument's error — no sign of a REF problem either way.)
// R10's other bits stay 0, so the HP and line-out
// common modes follow: expect input full scale 0.75 Vrms differential instead
// of 1.0 (line-in reads ~2.5 dB hotter), ~1.6 dB less output swing, and
// MICBIAS (R51 D5:D4=10) at 2.075 V instead of 2.5 V — electret operating
// point moves. All UNTESTED on hardware as of this change; the board tester's
// calibration must be redone with firmware that has it.
// `codec cm 0.9` restores the old setting for A/B tests (not saved).
static bool s_cm_075 = true;

static uint8_t cm_reg(void) { return s_cm_075 ? 0x40 : 0x00; }

void codec_set_cm_075(bool on) { s_cm_075 = on; }
bool codec_get_cm_075(void)    { return s_cm_075; }

static void codec_init_main(bool tdm, int slot_bits) {
    ESP_LOGI(TAG, "Starting Codec Initialization (%s)...", tdm ? "TDM" : "std I2S");

    // 1. Software Reset
    aic_write_page(CODEC_MAIN, 0, 0, 0x00);
    aic_write_page(CODEC_MAIN, 0, 1, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
    r58_forget();          // registers are back at defaults: nothing held, nothing routed
    s_routed_ok = false;

    // 2. Clock Configuration (48kHz fs; MCLK 12.288 MHz std / 24.576 MHz TDM)
    codec_init_clocks(CODEC_MAIN, tdm);

    // Interface: std = 32-bit I2S; TDM = DSP mode, slots 0-1 (offset base)
    codec_init_interface(CODEC_MAIN, tdm, slot_bits, s_tdm_base);

    // 3. Power & Routing (Page 1)
    aic_write_page(CODEC_MAIN, 0, 0, 0x01);
    aic_write_page(CODEC_MAIN, 1, 1, 0x08); // Disable weak AVDD connection
    aic_write_page(CODEC_MAIN, 1, 2, 0x01); // Enable Master Analog Power
    // Reference fast charge (40 ms) instead of the reset default "slowly" —
    // with 10 uF on REF the slow mode takes seconds, and a routing change in
    // that window met an unsettled input common mode. TI's scripts all write
    // this right here (SLAA557 4.0.1, `w 30 7b 01`).
    aic_write_page(CODEC_MAIN, 1, 123, 0x01);
    aic_write_page(CODEC_MAIN, 1, 10, cm_reg()); // full-chip common mode (see s_cm_075)
    // Analog input quick-charge: the reset value is not a valid setting
    // ("use one of the values below", SLAA557 5.3.39); 0x32 is TI's example.
    aic_write_page(CODEC_MAIN, 1, 71, 0x32);

    // Output Routing: DAC -> HPL/HPR & LOL/LOR
    aic_write_page(CODEC_MAIN, 1, 12, 0x08); // Left DAC -> HPL
    aic_write_page(CODEC_MAIN, 1, 13, 0x08); // Right DAC -> HPR
    aic_write_page(CODEC_MAIN, 1, 14, 0x08); // Left DAC -> LOL
    aic_write_page(CODEC_MAIN, 1, 15, 0x08); // Right DAC -> LOR

    aic_write_page(CODEC_MAIN, 1, 9, 0x3C); // Power UP all 4 output drivers

    s_active[CODEC_MAIN] = true;

    // Apply input routing + input/output gains for the current g_state
    // (mic/line/diff). Also unmutes the output drivers at the configured gain.
    codec_apply_settings();

    // 4. Final Enable (Page 0). R63 data-path mapping: cross in std (measured
    // TX-quirk compensation, see header comment), expected straight in TDM —
    // r63_on_value() tracks the mode and the `codec r63` measurement verb.
    aic_write_page(CODEC_MAIN, 0, 0, 0x00);
    aic_write_page(CODEC_MAIN, 0, 63, r63_on_value()); // Power L+R DAC
    aic_write_page(CODEC_MAIN, 0, 65, (uint8_t)s_dac_vol); // DAC volume (the soft reset
    aic_write_page(CODEC_MAIN, 0, 66, (uint8_t)s_dac_vol); // returned it to 0 dB)
    aic_write_page(CODEC_MAIN, 0, 64, 0x00); // Unmute DAC
    aic_write_page(CODEC_MAIN, 0, 81, 0xC0); // Power L+R ADC
    aic_write_page(CODEC_MAIN, 0, 82, 0x00); // Unmute ADC

    // Post-init readback sanity check (page-select register echoes what the
    // reset sequence left there) — a wiring/CS fault shows up here as a
    // mismatch instead of silently-dead audio.
    uint8_t rb = 0xEE;
    if (aic_read_page(CODEC_MAIN, 0, 0x00, &rb) == ESP_OK) {
        ESP_LOGI(TAG, "Codec Initialization Complete (reg0 readback 0x%02X)", rb);
    } else {
        ESP_LOGE(TAG, "Codec init readback failed — check SPI wiring");
    }
}

// Expansion boards are capture-only: ADC + MICBIAS + mic routing powered,
// DACs and output drivers left down (their outputs are unwired — a future
// mic+speaker expansion re-enables them here plus its TX slots).
static void codec_init_expansion(codec_id_t id, int slot_bits, uint8_t offset)
{
    // Software Reset
    aic_write_page(id, 0, 0, 0x00);
    aic_write_page(id, 0, 1, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));

    codec_init_clocks(id, true);
    codec_init_interface(id, true, slot_bits, offset);

    // Analog power block (Page 1), capture path only
    aic_write_page(id, 0, 0, 0x01);
    aic_write_page(id, 1, 1, 0x08);  // Disable weak AVDD connection
    aic_write_page(id, 1, 2, 0x01);  // Enable Master Analog Power
    aic_write_page(id, 1, 123, 0x01); // reference fast charge (see codec_init_main)
    aic_write_page(id, 1, 10, cm_reg()); // full-chip common mode (see s_cm_075)
    aic_write_page(id, 1, 9, 0x00);  // Output drivers stay OFF (explicit)

    // Mic inputs (mirror of the main codec's INPUT_MIC routing)
    aic_write_page(id, 1, 51, 0x68); // MICBIAS on (2.075 V at 0.75 V CM, 2.5 V at 0.9 V)
    aic_write_page(id, 1, 52, 0x40); // IN1L -> Left P (10k)
    aic_write_page(id, 1, 55, 0x40); // IN1R -> Right P (10k)
    aic_write_page(id, 1, 54, 0x01); // Left M-Terminal
    aic_write_page(id, 1, 57, 0x40); // Right M-Terminal
    aic_write_page(id, 1, 58, R58_UNROUTED_MIC); // hold unused IN2/IN3 at CM (see R58)
    // KNOWN GAP: the expansion mics above are routed straight onto caps that
    // start at 0 V (no precharge), on init and on wake, and the input guard
    // only watches the main codec. TDM is bench-only so far; revisit if an
    // expansion channel is ever seen stuck.
    aic_write_page(id, 1, 71, 0x32);             // valid quick-charge (see codec_init_main)

    // Final enable: ADC only, DACs stay down
    aic_write_page(id, 0, 0, 0x00);
    aic_write_page(id, 0, 63, CODEC_R63_DAC_OFF);
    aic_write_page(id, 0, 64, 0x0C); // DAC muted (belt and braces)
    aic_write_page(id, 0, 81, 0xC0); // Power L+R ADC
    aic_write_page(id, 0, 82, 0x00); // Unmute ADC

    s_active[id] = true;
    ESP_LOGI(TAG, "Expansion codec %d up (offset %u BCLKs)", (int)id, (unsigned)offset);
}

// Initialize every present codec for the chosen geometry. MCLK must already
// be running (I2S channels enabled) — the PLLs reference it.
// Slot pairs are assigned in order of presence (an expansion board on either
// CS gets the next free pair), so a lone board on CODEC_2_CS still lands on
// slots 2-3.
// --- Boot snapshot ------------------------------------------------------------
// The codec stays powered across an MCU reset (restart, watchdog, flashing), so
// its registers still hold the pre-reset state until codec_init_main's soft
// reset wipes them. Reading pages 0/1 first preserves that state — e.g. a
// dead-LINE-IN state — for `codec snapshot`. Costs 256 SPI reads at boot.
static uint8_t s_boot_snap[2][128];
static bool    s_boot_snap_ok;
static bool    s_adopted;

void codec_snapshot_boot(void)
{
    s_boot_snap_ok = true;
    for (int page = 0; page < 2 && s_boot_snap_ok; page++) {
        for (int reg = 0; reg < 128; reg++) {
            if (aic_read_page(CODEC_MAIN, (uint8_t)page, (uint8_t)reg, &s_boot_snap[page][reg]) != ESP_OK) {
                s_boot_snap_ok = false;
                break;
            }
        }
    }
    spi_lock(); aic_write_byte(CODEC_MAIN, 0x00, 0x00); spi_unlock();   // back to page 0
    if (!s_boot_snap_ok) { ESP_LOGW(TAG, "boot snapshot: SPI read failed"); return; }
    ESP_LOGI(TAG, "boot snapshot: P0 R81=%02X P1 R2=%02X R52=%02X R54=%02X R55=%02X R57=%02X -> %s",
             s_boot_snap[0][81], s_boot_snap[1][2], s_boot_snap[1][52], s_boot_snap[1][54],
             s_boot_snap[1][55], s_boot_snap[1][57],
             codec_boot_was_running() ? "codec was running before this boot (MCU-only reset)"
                                      : "fresh codec (power-on)");
}

bool codec_boot_was_running(void)
{
    return s_boot_snap_ok && s_boot_snap[0][81] == 0xC0 && s_boot_snap[1][2] == 0x01;
}

bool codec_boot_snapshot(uint8_t page, const uint8_t **regs)
{
    if (!s_boot_snap_ok || page > 1) return false;
    *regs = s_boot_snap[page];
    return true;
}

bool codec_was_adopted(void) { return s_adopted; }

void codec_init_all(bool tdm, int slot_bits)
{
    s_tdm_mode     = tdm;
    s_slot_bits    = slot_bits;
    s_r63_straight = tdm;   // expected per framing; lr_test measurement decides

#if CODEC_DIAG_ADOPT
    // DIAGNOSTIC BUILD ONLY: if the codec was already running (MCU-only reset,
    // e.g. flashing this build onto a board in the dead-input state), keep its
    // state instead of soft-resetting it, so recovery steps can be tried on the
    // live fault. g_state is set to mirror what the codec actually holds.
    if (!tdm && codec_boot_was_running()) {
        const uint8_t *p0 = s_boot_snap[0], *p1 = s_boot_snap[1];
        static const uint8_t own0[] = { 4, 5, 6, 11, 12, 14, 18, 19, 20, 27, 63, 64, 81, 82 };
        static const uint8_t own1[] = { 1, 2, 9, 10, 12, 13, 14, 15, 16, 17, 18, 19, 51, 52, 54, 55, 57, 59, 60 };
        for (size_t i = 0; i < sizeof(own0); i++) {       // shadow = snapshot, so `codec check`
            s_shadow[CODEC_MAIN][0][own0[i]] = p0[own0[i]]; // reports any change since boot
            s_shadow_valid[CODEC_MAIN][0][own0[i] >> 5] |= 1u << (own0[i] & 31);
        }
        for (size_t i = 0; i < sizeof(own1); i++) {
            s_shadow[CODEC_MAIN][1][own1[i]] = p1[own1[i]];
            s_shadow_valid[CODEC_MAIN][1][own1[i] >> 5] |= 1u << (own1[i] & 31);
        }
        s_r63_straight = (p0[63] == CODEC_R63_DAC_ON_STRAIGHT);
        s_active[CODEC_MAIN] = true;
        s_adopted = true;
        bool diff = p1[52] == 0x10 && p1[54] == 0x10 && p1[55] == 0x04 && p1[57] == 0x04;
        g_state.input_mode    = diff ? INPUT_LINE_DIFF : INPUT_MIC;
        g_state.input_gain_db = p1[59] / 2;
        int og = p1[16] & 0x3F;
        g_state.output_gain_db = (og & 0x20) ? og - 64 : og;
        s_routed_ok   = true;      // a same-input apply must not re-route the live fault
        s_routed_mode = g_state.input_mode;
        ESP_LOGW(TAG, "DIAG BUILD: ADOPTED the running codec (no soft reset, no init) — input %s",
                 diff ? "diff" : "mic/other");
        return;
    }
#endif

    route_lock();   // re-init (`codec reinit`, `codec cm`) must not interleave a routing sequence
    codec_init_main(tdm, slot_bits);

    if (tdm) {
        int pair = 1;
        for (codec_id_t id = CODEC_EXP1; id < CODEC_COUNT; id++) {
            if (!s_present[id]) continue;
            codec_init_expansion(id, slot_bits,
                                 (uint8_t)(s_tdm_base + pair * 2 * slot_bits));
            pair++;
        }
        codec_apply_input_gain();  // fan the mic gain out to the expansions
    }
    s_route_epoch++;   // the guard restarts its settle window after any re-init
    route_unlock();
}


// --- TDM bring-up measurement verbs (CLI `codec ...`) ------------------------

// Flip the main codec's R63 DAC data-path mapping live (playback leg of the
// lr_test protocol decides which is correct per interface mode).
void codec_dbg_r63(bool straight)
{
    s_r63_straight = straight;
    aic_write_page(CODEC_MAIN, 0, 63, r63_on_value());
}

// Rewrite every active codec's R28 data offset from a new base (RX/slot-map
// leg of the protocol — catches a uniform ±1-BCLK framing misalignment).
void codec_dbg_tdm_base(uint8_t base_bclks)
{
    s_tdm_base = base_bclks;
    if (!s_tdm_mode) return;
    aic_write_page(CODEC_MAIN, 0, 28, s_tdm_base);
    int pair = 1;
    for (codec_id_t id = CODEC_EXP1; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 0, 28, (uint8_t)(s_tdm_base + pair * 2 * s_slot_bits));
        pair++;
    }
}

// Power down everything that draws current while leaving every routing,
// volume, and interface register intact — the chip stays powered, so
// codec_wake() can restore without a full re-init.
// Must be called while I2S is still clocking so the soft-step mutes run.
void codec_sleep(void) {
    route_lock();   // never power down in the middle of a routing sequence
    // 1. Mute analog drivers (avoid pop) — main codec only has drivers up
    aic_write_page(CODEC_MAIN, 1, 16, 0x40); // HPL mute
    aic_write_page(CODEC_MAIN, 1, 17, 0x40); // HPR mute
    aic_write_page(CODEC_MAIN, 1, 18, 0x40); // LOL mute
    aic_write_page(CODEC_MAIN, 1, 19, 0x40); // LOR mute

    // 2. Digital mutes, then let soft-stepping settle
    aic_write_page(CODEC_MAIN, 0, 64, 0x0C); // L+R DAC mute
    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 0, 82, 0x88); // L+R ADC mute
    }
    vTaskDelay(pdMS_TO_TICKS(25));

    // 3. Per-codec power-down, leaf to root: MICBIAS, then converters (data-
    //    path bits retained; expansion DACs already off), then the clock tree.
    aic_write_page(CODEC_MAIN, 1, 9, 0x00);  // HPL/HPR/LOL/LOR drivers off (main only)
    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 1, 51, 0x00); // MICBIAS off

        aic_write_page(id, 0, 63, CODEC_R63_DAC_OFF);
        aic_write_page(id, 0, 81, 0x00); // L+R ADC off

        aic_write_page(id, 0, 19, 0x02); // MADC divider off
        aic_write_page(id, 0, 18, 0x08); // NADC divider off
        aic_write_page(id, 0, 12, 0x02); // MDAC divider off
        aic_write_page(id, 0, 11, 0x08); // NDAC divider off
        aic_write_page(id, 0, 5,  0x11); // PLL down (P=1, R=1 kept)
    }

    // With every analog block down the reference (and CM) may collapse, so
    // nothing held so far counts: codec_wake() precharges from scratch.
    r58_forget();
    s_routed_ok = false;
    s_asleep = true;
    s_route_epoch++;
    route_unlock();
    ESP_LOGI(TAG, "Codec powered down (registers retained)");
}

// Reverse of codec_sleep(). MCLK must already be running (PLL reference).
// Takes the full input precharge (s_precharge_ms) before the ADC comes back.
void codec_wake(void) {
    route_lock();
    s_asleep = false;
    r58_forget();   // whatever held the pins before (or during) sleep no longer counts
    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        // R5 retains the mode-correct P divider set at init (sleep wrote the
        // powered-down value with P=1; re-issue the init value here).
        aic_write_page(id, 0, 5, s_tdm_mode ? 0xA1 : 0x91);  // PLL up
    }
    vTaskDelay(pdMS_TO_TICKS(15)); // PLL lock

    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 0, 11, 0x88); // NDAC
        aic_write_page(id, 0, 12, 0x82); // MDAC
        aic_write_page(id, 0, 18, 0x88); // NADC
        aic_write_page(id, 0, 19, 0x82); // MADC
    }

    // DAC digital volume to minimum (-63.5 dB) BEFORE anything unmutes, so
    // the wake transition is silent; codec_fade_in() ramps it back up to the
    // set volume once live audio is flowing again.
    aic_write_page(CODEC_MAIN, 0, 65, 0x81); // Left DAC volume  = -63.5 dB
    aic_write_page(CODEC_MAIN, 0, 66, 0x81); // Right DAC volume = -63.5 dB

    aic_write_page(CODEC_MAIN, 0, 63, r63_on_value());
    aic_write_page(CODEC_MAIN, 1, 9, 0x3C);  // output drivers up (still muted)
    vTaskDelay(pdMS_TO_TICKS(30)); // HP driver soft power-up / depop

    // Main codec inputs: full precharge (sleep forgot the held state), then
    // route + MICBIAS per mode + gains (unmutes HP/LO at the configured driver
    // gain; the DAC volume is still at minimum) + ADC on, MUTED.
    inputs_route(s_precharge_ms, true, 0x88);

    // Expansion codecs are mic-only: ADC + MICBIAS always.
    for (codec_id_t id = CODEC_EXP1; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 0, 81, 0xC0); // L+R ADC on (muted since codec_sleep)
        aic_write_page(id, 1, 51, 0x68); // MICBIAS (2.075 V at 0.75 V CM)
    }
    aic_write_page(CODEC_MAIN, 0, 64, 0x00); // unmute DAC
    route_unlock();

    // ADC deliberately left MUTED: the MICBIAS power-on transient takes
    // hundreds of ms to settle through the mic coupling caps, and unmuting
    // now would feed that thump into the DSP delay lines (sounds like
    // feedback on wake). Call codec_unmute_adc() after a settle period.

    ESP_LOGI(TAG, "Codec restored from sleep (ADC muted, output at minimum)");
}

// Unmute the ADCs once the MICBIAS transient has settled after codec_wake().
void codec_unmute_adc(void) {
    route_lock();   // a sequence snapshots/restores R82: never unmute underneath one
    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 0, 82, 0x00);
    }
    route_unlock();
}

// Ramp the DAC digital volume from -63.5 dB up to the set volume (0 dB
// unless the pot moved it) at the rate of a full ramp over ~ramp_ms.
// Call after the audio pipeline is running so the ramp shapes live audio
// (the codec soft-steps each 0.5 dB increment, so the result is smooth).
// The target is re-read every step, so a knob turn mid-ramp still lands.
void codec_fade_in(uint32_t ramp_ms) {
    const int step = 2;                 // 1 dB per write (2 × 0.5 dB counts)
    const int steps = -CODEC_DAC_VOL_MIN / step + 1;
    uint32_t delay_ms = ramp_ms / steps;
    if (delay_ms == 0) delay_ms = 1;

    for (int v = CODEC_DAC_VOL_MIN; v < s_dac_vol; v += step) {
        aic_write_page(CODEC_MAIN, 0, 65, (uint8_t)v);
        aic_write_page(CODEC_MAIN, 0, 66, (uint8_t)v);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    aic_write_page(CODEC_MAIN, 0, 65, (uint8_t)s_dac_vol);
    aic_write_page(CODEC_MAIN, 0, 66, (uint8_t)s_dac_vol);
}

// --- Readback diagnostics + recovery verbs (CLI `codec rd|dump|check|...`) ---
// For the intermittent "LINE IN dead" investigation: `status` otherwise only
// reports what the firmware INTENDED (g_state); these read what the codec
// actually holds. Reading P0 R42/R44/R45 clears those sticky flags.

esp_err_t codec_read_reg(codec_id_t id, uint8_t page, uint8_t reg, uint8_t *val)
{
    if (id < 0 || id >= CODEC_COUNT || !s_spi[id]) return ESP_ERR_INVALID_ARG;
    return aic_read_page(id, page, reg, val);
}

int codec_check_shadow(codec_id_t id, codec_mismatch_fn report, void *ctx, int *mismatches)
{
    int checked = 0, bad = 0;
    if (mismatches) *mismatches = 0;
    if (id < 0 || id >= CODEC_COUNT || !s_spi[id]) return -1;
    for (int page = 0; page < 2; page++) {
        for (int reg = 1; reg < 128; reg++) {
            if (!(s_shadow_valid[id][page][reg >> 5] & (1u << (reg & 31)))) continue;
            uint8_t v;
            if (aic_read_page(id, (uint8_t)page, (uint8_t)reg, &v) != ESP_OK) return -1;
            checked++;
            if (v != s_shadow[id][page][reg]) {
                bad++;
                if (report) report((uint8_t)page, (uint8_t)reg, s_shadow[id][page][reg], v, ctx);
            }
        }
    }
    if (mismatches) *mismatches = bad;
    return checked;
}

uint32_t codec_spi_error_count(void) { return s_spi_errors; }

bool codec_is_active(codec_id_t id)
{
    return (id >= 0 && id < CODEC_COUNT) ? s_active[id] : false;
}

// Recovery rung: ADC power cycle, routing untouched.
void codec_adc_cycle(void)
{
    route_lock();
    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 0, 81, 0x00);   // L+R ADC off
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    for (codec_id_t id = CODEC_MAIN; id < CODEC_COUNT; id++) {
        if (!s_active[id]) continue;
        aic_write_page(id, 0, 81, 0xC0);   // L+R ADC on
        aic_write_page(id, 0, 82, 0x00);   // unmuted
    }
    s_route_epoch++;
    route_unlock();
}

// Recovery rung: soft reset + full init + re-apply g_state, same geometry as
// boot (MCLK keeps running, so the PLLs have their reference).
void codec_reinit(void)
{
    codec_init_all(s_tdm_mode, s_slot_bits);
}

#if CODEC_DIAG_ADOPT
// Raw register write (CLI `codec wr`, diagnostic builds only — a footgun on a
// teaching board). Shadowed like every other write.
void codec_write_reg(codec_id_t id, uint8_t page, uint8_t reg, uint8_t val)
{
    if (id < 0 || id >= CODEC_COUNT || !s_spi[id]) return;
    ESP_LOGW(TAG, "codec wr %d P%u R%u = 0x%02X", (int)id, (unsigned)page, (unsigned)reg, val);
    aic_write_page(id, page, reg, val);
}
#endif
