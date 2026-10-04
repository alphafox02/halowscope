/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Receiver bring-up, settings and one-shot capture under ESP-IDF.
 *
 * Adapted from eSpDR (https://github.com/h0m3us3r/eSpDR and
 * github.com/alphafox02/eSpDR, 0BSD): esp32s3/src/radio.c, snapshot.c and
 * board.h. eSpDR runs bare metal; here ESP-IDF's PHY driver does the
 * power-up calibration, the analog registers go through ESP-IDF's locked
 * regi2c access, and the capture bank is reserved from the heap.
 *
 * After calibration everything is done directly: the RF PLL is tuned by
 * software, the Wi-Fi AGC is disabled, the receive gain is forced, and the
 * analog gain stages are owned through the PBUS interface. No transmit path
 * is ever enabled. The ESP32-S3's Wi-Fi stack is never started.
 */

#include "radio.h"

#include <math.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_phy_init.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_rom_sys.h"
#include "hal/clk_gate_ll.h"
#include "heap_memory_layout.h"
#include "soc/system_reg.h"

static const char *TAG = "radio";

#define REG(address) (*(volatile uint32_t *)(address))

/* Capture bank 0 of the four 64 KiB banks the sample writer can target. */
#define CAPTURE_BANK_BASE 0x3FCB0000u
#define CAPTURE_BANK_BYTES 0x10000u
SOC_RESERVE_MEMORY_REGION(CAPTURE_BANK_BASE, CAPTURE_BANK_BASE + CAPTURE_BANK_BYTES, halowscope_capture);

/* ADC sample dump engine. */
#define DUMP_CTRL_REG 0x60033D5Cu        /* bit 31 run, bits 0..15 ring length */
#define DUMP_WRITE_INDEX_REG 0x60033D60u
#define DUMP_CONFIG_REG 0x60033D90u
#define DUMP_BANK_SELECT_REG 0x600C101Cu /* bits 0..3 route the writer to bank 0..3 */
#define DUMP_CTRL_RUN 0x80000000u
#define DUMP_CTRL_CIRCULAR 0x00024000u   /* circular 16384-pair ring, IQ source 0 */
#define DUMP_CTRL_16MSPS 0x00010000u     /* dump clock 16 Msps instead of 80 */
#define DUMP_CONFIG_IQ 0x000C2040u

/* Baseband, AGC and front-end control. */
#define BB_ENABLE_REG 0x6002600Cu        /* bit 1 BB enable, bits 2..3 width, bit 28 MAC BB */
#define AGC_CTRL_REG 0x6001C01Cu
#define AGC_GAIN_FORCE_REG 0x6001C02Cu   /* bits 24..30 gain selector, bit 23 force */
#define AGC_DISABLE_REG 0x6001C034u
#define AGC_RX_FORCE_REG 0x6001C080u
#define IQ_CORRECTION_REG 0x6000607Cu    /* receive I/Q amplitude and phase correction */
#define FE_WIDTH_REG 0x60006100u         /* bits 16..21 analog channel width */
#define PBUS_CTRL_REG 0x60006104u        /* bit 0 software owner, bit 1 write strobe */
#define PBUS_MODE_REG 0x6000610Cu
#define PBUS_STATUS_REG 0x60006110u      /* bit 31 busy, bits 14..15 RX force */
#define PBUS_BB_GAIN_REG 0x60006118u     /* bits 9..17 current baseband gain */
#define PBUS_RF_GAIN_REG 0x6000611Cu     /* bits 18..26 current RF gain */
#define RFPLL_OWNER_REG 0x6000E0C4u      /* bit 25 software RFPLL control */

/* Analog (regi2c) blocks. */
#define I2C_RFPLL 0x62
#define I2C_SDM 0x63
#define I2C_BB_FILTER 0x67

#define REFERENCE_HZ 40000000u
#define PBUS_TIMEOUT_CYCLES 24000u
#define IQ_FIELDS 0x1FFF0000u  /* I/Q correction: amplitude 20:16, phase 26:21, mode 28:27 */
#define IQ_MANUAL 0x08000000u  /* bit 27 set, bit 28 clear: the fields apply */

#define RING_MASK (RADIO_RING_PAIRS - 1u)
/* 16384 pairs take 1.024 ms at 16 Msps and 0.2 ms at 80 Msps. */
#define SNAPSHOT_RUN_US_80M 400u
#define SNAPSHOT_RUN_US_16M 1300u
#define SNAPSHOT_SETTLE_US 20u

extern unsigned rom_pbus_rd(unsigned block, unsigned index);

static struct radio_settings settings = {
    .lo_hz = 2440000000u, .rate = RADIO_RATE_80M, .width = 40, .filter = 0, .gain = 60,
    .rf_gain = RADIO_AUTO, .bb_gain = RADIO_AUTO,
    .dc = {RADIO_AUTO, RADIO_AUTO, RADIO_AUTO, RADIO_AUTO}, .iq = RADIO_AUTO,
};

static struct {
    unsigned status;
    uint32_t lo_hz;
    unsigned pll_cap, pll_first, pll_length;
    bool owned;                     /* gain stages held through the PBUS */
    uint32_t pbus_ctrl, pbus_mode;  /* before taking them */
    uint32_t iq_register;           /* before a manual I/Q correction */
    unsigned rf_gain, bb_gain;
    unsigned dc[4], dc_hardware[4];
} receiver = { .status = RADIO_NOT_STARTED };

/* DC offset registers 0..3 as PBUS block and index. */
static const uint8_t dc_block[4] = {3, 3, 2, 2}, dc_index[4] = {1, 2, 1, 2};

static void delay_us(uint32_t us) { esp_rom_delay_us(us); }

static void memory_barrier(void) { __asm__ volatile("memw" ::: "memory"); }

/* ---- analog register access ---------------------------------------------- */

static uint8_t analog_read(uint8_t block, uint8_t reg) { return regi2c_ctrl_read_reg(block, 1, reg); }

static void analog_write(uint8_t block, uint8_t reg, uint8_t value)
{
    regi2c_ctrl_write_reg(block, 1, reg, value);
}

static void analog_write_bits(uint8_t block, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old = analog_read(block, reg);
    analog_write(block, reg, (uint8_t)((old & ~mask) | (value & mask)));
}

/* ---- PHY power-up and calibration ------------------------------------------ */

static void power_up_phy(void)
{
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(PHY_MODEM_WIFI);

    /* The PHY and RNG clocks stay on: the dump registers stop responding
     * without them. The sample dump engine also needs the Wi-Fi MAC clock
     * (bit 6), which the public clock-gate mask does not include. */
    periph_ll_enable_clk_clear_rst(PERIPH_RNG_MODULE);
    periph_ll_wifi_module_enable_clk_clear_rst();
    REG(SYSTEM_WIFI_CLK_EN_REG) |= 1u << 6;
}

/* ---- RF PLL ------------------------------------------------------------------ */

static void set_pll_capacitor(unsigned cap)
{
    analog_write(I2C_RFPLL, 1, (uint8_t)cap);
    analog_write_bits(I2C_RFPLL, 2, 0x10, (uint8_t)((cap >> 8) << 4));
}

static void set_pll_manual_capacitor(bool manual)
{
    analog_write_bits(I2C_RFPLL, 11, 0x40, manual ? 0x40 : 0);
}

/*
 * Capacitor codes found so far, one slot per 10 MHz of LO. The code falls
 * steadily with frequency (about 0.7 per MHz), so a nearby result predicts
 * a new one closely enough to scan a narrow window instead of all 512.
 */
#define CAP_SLOTS 60
#define CAP_SLOT_BASE_HZ 2200000000u
#define CAP_NARROW 24
#define CAP_PER_MHZ (-0.73)
static struct { uint32_t lo_hz; int16_t cap; } cap_cache[CAP_SLOTS];

static int predict_cap(uint32_t lo_hz)
{
    int best = -1;
    uint32_t best_distance = UINT32_MAX;
    for (int i = 0; i < CAP_SLOTS; i++) {
        if (!cap_cache[i].lo_hz)
            continue;
        uint32_t d = cap_cache[i].lo_hz > lo_hz ? cap_cache[i].lo_hz - lo_hz : lo_hz - cap_cache[i].lo_hz;
        if (d < best_distance) {
            best_distance = d;
            best = i;
        }
    }
    if (best < 0 || best_distance > 100000000u)
        return -1;
    double mhz = ((double)lo_hz - cap_cache[best].lo_hz) / 1e6;
    return cap_cache[best].cap + (int)lround(CAP_PER_MHZ * mhz);
}

/* Longest run of locked codes in [first, last]; false if none. */
static bool scan_caps(unsigned first, unsigned last, unsigned *run_first, unsigned *run_length)
{
    unsigned run_start = 0, run = 0, best_start = 0, best_length = 0;
    for (unsigned cap = first; cap <= last; cap++) {
        set_pll_capacitor(cap);
        delay_us(20);
        bool locked = ((analog_read(I2C_RFPLL, 12) >> 2) & 3) == 0;
        if (!locked) {
            run = 0;
            continue;
        }
        if (run++ == 0)
            run_start = cap;
        if (run > best_length) {
            best_start = run_start;
            best_length = run;
        }
    }
    *run_first = best_start;
    *run_length = best_length;
    return best_length > 0;
}

/*
 * Programs the sigma-delta word, waits for the PLL's own calibration, then
 * scans the VCO capacitor codes and pins the capacitor to the middle of the
 * longest run of codes the PLL reports as locked. With a prediction from
 * earlier tunes only a window around it is scanned; a run that reaches the
 * window's edge falls back to the full scan of all 512 codes.
 */
static bool tune_pll(uint32_t lo_hz)
{
    /* LO = 3/4 * reference * (32 + word / 2^16), to the nearest word */
    uint64_t scaled = ((uint64_t)lo_hz * 4 * 65536 + 3ull * REFERENCE_HZ / 2) / (3ull * REFERENCE_HZ);
    uint32_t word = (uint32_t)(scaled - 32 * 65536);
    receiver.lo_hz = (uint32_t)((3ull * REFERENCE_HZ * scaled + 2 * 65536) / (4 * 65536));

    REG(RFPLL_OWNER_REG) |= 1u << 25;
    set_pll_manual_capacitor(false);
    analog_write(I2C_SDM, 0, 0x07);
    analog_write(I2C_SDM, 3, (uint8_t)(word >> 16));
    analog_write(I2C_SDM, 4, (uint8_t)(word >> 8));
    analog_write(I2C_SDM, 5, (uint8_t)word);
    analog_write(I2C_SDM, 0, 0x17);

    /* Restart calibration and wait for it to report done. */
    analog_write_bits(I2C_RFPLL, 0, 0x40, 0x40);
    analog_write_bits(I2C_RFPLL, 0, 0x20, 0x00);
    analog_write_bits(I2C_RFPLL, 0, 0x20, 0x20);
    analog_write_bits(I2C_RFPLL, 0, 0x40, 0x00);
    bool calibrated = false;
    for (unsigned poll = 0; poll < 100 && !calibrated; poll++) {
        delay_us(20);
        calibrated = analog_read(I2C_RFPLL, 7) & 2;
    }
    if (!calibrated)
        return false;
    delay_us(5);

    uint8_t saved_low = analog_read(I2C_RFPLL, 1);
    uint8_t saved_high = analog_read(I2C_RFPLL, 2);
    uint8_t saved_mode = analog_read(I2C_RFPLL, 11);
    set_pll_manual_capacitor(true);
    unsigned best_start = 0, best_length = 0;
    bool found = false;
    int predicted = predict_cap(lo_hz);
    if (predicted >= 0) {
        unsigned first = predicted > CAP_NARROW ? predicted - CAP_NARROW : 0;
        unsigned last = predicted + CAP_NARROW < 511 ? predicted + CAP_NARROW : 511;
        /* A run cut off by the window (not by the ends of the code range)
         * may continue outside it. */
        found = scan_caps(first, last, &best_start, &best_length) && (best_start > first || first == 0) &&
                (best_start + best_length - 1 < last || last == 511);
    }
    if (!found)
        found = scan_caps(0, 511, &best_start, &best_length);
    analog_write(I2C_RFPLL, 1, saved_low);
    analog_write_bits(I2C_RFPLL, 2, 0x10, saved_high);
    analog_write_bits(I2C_RFPLL, 11, 0x40, saved_mode);
    if (!found)
        return false;

    receiver.pll_first = best_start;
    receiver.pll_length = best_length;
    receiver.pll_cap = best_start + (best_length - 1) / 2;
    set_pll_capacitor(receiver.pll_cap);
    set_pll_manual_capacitor(true);
    if (lo_hz >= CAP_SLOT_BASE_HZ) {
        unsigned slot = (lo_hz - CAP_SLOT_BASE_HZ) / 10000000u;
        if (slot < CAP_SLOTS) {
            cap_cache[slot].lo_hz = lo_hz;
            cap_cache[slot].cap = receiver.pll_cap;
        }
    }
    return true;
}

/* ---- receive path -------------------------------------------------------------- */

/* Write one analog register through the PBUS interface. */
static bool pbus_write(unsigned block, unsigned index, unsigned value)
{
    uint32_t fields = ((value & 511u) << 6) | ((block & 15u) << 2) | ((index & 3u) << 15);
    REG(PBUS_CTRL_REG) = (REG(PBUS_CTRL_REG) & 0xFFFE0001u) | (fields & 0x1FFFCu) | 2u;
    uint32_t start = esp_cpu_get_cycle_count();
    while (REG(PBUS_STATUS_REG) & 0x80000000u) {
        if (esp_cpu_get_cycle_count() - start > PBUS_TIMEOUT_CYCLES) {
            REG(PBUS_CTRL_REG) &= ~2u;
            return false;
        }
    }
    REG(PBUS_CTRL_REG) &= ~2u;
    return true;
}

static void set_width(unsigned mhz)
{
    bool wide = mhz == 40;
    REG(FE_WIDTH_REG) = (REG(FE_WIDTH_REG) & ~0x003F0000u) | (wide ? 0x00120000u : 0);
    REG(BB_ENABLE_REG) = (REG(BB_ENABLE_REG) & ~0xCu) | (wide ? 0x4u : 0);
}

static uint32_t dump_control(void)
{
    return DUMP_CTRL_CIRCULAR | (settings.rate == RADIO_RATE_16M ? DUMP_CTRL_16MSPS : 0);
}

/* Dump engine stopped, RX forces and baseband off. */
static void park_receiver(void)
{
    REG(DUMP_CTRL_REG) &= ~DUMP_CTRL_RUN;
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    REG(AGC_RX_FORCE_REG) &= ~0xC1u;
    REG(PBUS_STATUS_REG) &= ~0xCF00u;
    REG(BB_ENABLE_REG) &= ~2u;
}

/* Parks the receiver and hands the gain stages, DC offsets and I/Q
 * correction back to the hardware as configure_receiver found them. */
static bool release_receiver(void)
{
    park_receiver();
    if (!receiver.owned)
        return true;
    bool ok = true;
    for (unsigned r = 0; r < 4; r++)
        if (receiver.dc[r] != receiver.dc_hardware[r])
            ok = pbus_write(dc_block[r], dc_index[r], receiver.dc_hardware[r]) && ok;
    ok = pbus_write(0, 1, 0) && pbus_write(1, 1, 0) && pbus_write(1, 2, 0) && ok;
    REG(PBUS_CTRL_REG) = receiver.pbus_ctrl;
    REG(PBUS_MODE_REG) = receiver.pbus_mode;
    REG(IQ_CORRECTION_REG) = receiver.iq_register;
    receiver.owned = false;
    return ok;
}

static bool configure_receiver(void)
{
    park_receiver();

    /* The width selects an analog RC bank when the baseband is enabled, so
     * it is programmed before the enable edge. */
    set_width(settings.width);

    /* Enable the baseband, disable the Wi-Fi AGC and force the RX gain. */
    REG(BB_ENABLE_REG) |= 0x10000000u;
    REG(BB_ENABLE_REG) &= ~2u;
    delay_us(1);
    REG(BB_ENABLE_REG) |= 2u;
    REG(AGC_CTRL_REG) = (REG(AGC_CTRL_REG) & 0xFF00FFFFu) | 0x007F0000u;
    REG(AGC_DISABLE_REG) |= 0x80u;
    REG(AGC_RX_FORCE_REG) |= 1u;
    REG(AGC_GAIN_FORCE_REG) =
        (REG(AGC_GAIN_FORCE_REG) & 0x007FFFFFu) | (settings.gain << 24) | 0x00800000u;
    REG(PBUS_STATUS_REG) |= 0xC000u;

    /* Baseband RC filter: registers 6/7 serve 40 MHz, 4/5 serve 20 MHz. */
    set_width(settings.width);
    unsigned filter = settings.width == 40 ? 6 : 4;
    analog_write(I2C_BB_FILTER, filter, settings.filter & 63);
    analog_write(I2C_BB_FILTER, filter + 1, (settings.filter >> 8) & 63);

    /* Let the forced gain settle, capture the gain stages it selected, then
     * take PBUS ownership and hold them there with the baseband disabled. */
    delay_us(100);
    unsigned bb = (REG(PBUS_BB_GAIN_REG) >> 9) & 511;
    unsigned rf = (REG(PBUS_RF_GAIN_REG) >> 18) & 511;
    if (settings.bb_gain != RADIO_AUTO)
        bb = 0x180 | settings.bb_gain;
    if (settings.rf_gain != RADIO_AUTO)
        rf = settings.rf_gain;
    receiver.pbus_ctrl = REG(PBUS_CTRL_REG);
    receiver.pbus_mode = REG(PBUS_MODE_REG);
    receiver.iq_register = REG(IQ_CORRECTION_REG);
    for (unsigned r = 0; r < 4; r++)
        receiver.dc[r] = receiver.dc_hardware[r] = 0;
    REG(PBUS_MODE_REG) &= ~0x08000000u;
    REG(PBUS_CTRL_REG) |= 1u;
    receiver.owned = true;
    REG(BB_ENABLE_REG) &= ~2u;

    /* Receive-only power configuration: both TX groups stay off. */
    bool ok = pbus_write(4, 1, 0) && pbus_write(5, 1, 0) && pbus_write(0, 1, 0x184) &&
              pbus_write(1, 1, 0x189) && pbus_write(1, 2, rf) && pbus_write(0, 1, bb);
    receiver.rf_gain = rf;
    receiver.bb_gain = bb;
    for (unsigned r = 0; r < 4 && ok; r++) {
        receiver.dc[r] = receiver.dc_hardware[r] = rom_pbus_rd(dc_block[r], dc_index[r]) & 511;
        if (settings.dc[r] != RADIO_AUTO && settings.dc[r] != receiver.dc[r]) {
            ok = pbus_write(dc_block[r], dc_index[r], settings.dc[r]);
            receiver.dc[r] = settings.dc[r];
        }
    }
    if (settings.iq != RADIO_AUTO)
        REG(IQ_CORRECTION_REG) = (receiver.iq_register & ~IQ_FIELDS) | IQ_MANUAL |
                                 ((settings.iq & 31u) << 16) | (((settings.iq >> 8) & 63u) << 21);
    if (!ok)
        return false;

    REG(DUMP_CONFIG_REG) = DUMP_CONFIG_IQ;
    delay_us(100);
    REG(DUMP_CTRL_REG) = dump_control();
    return true;
}

static unsigned reconfigure(bool retune)
{
    if (!release_receiver())
        return RADIO_PBUS_FAILED;
    if (retune && !tune_pll(settings.lo_hz))
        return RADIO_PLL_FAILED;
    if (!configure_receiver())
        return RADIO_PBUS_FAILED;
    return RADIO_OK;
}

unsigned radio_init(void)
{
    /* The heap reservation only holds if static data ends below the bank. */
    extern char _heap_start;
    if ((uintptr_t)&_heap_start > CAPTURE_BANK_BASE) {
        ESP_LOGE(TAG, "static data reaches the capture bank (heap starts at %p)", &_heap_start);
        return receiver.status = RADIO_PHY_FAILED;
    }
    power_up_phy();
    receiver.status = reconfigure(true);
    ESP_LOGI(TAG, "radio %u, LO %lu Hz, PLL capacitor %u (locks %u..%u)", receiver.status,
             (unsigned long)receiver.lo_hz, receiver.pll_cap, receiver.pll_first,
             receiver.pll_first + receiver.pll_length - 1);
    return receiver.status;
}

static bool valid(const struct radio_settings *s)
{
    if (s->lo_hz < RADIO_LO_MIN_HZ || s->lo_hz > RADIO_LO_MAX_HZ)
        return false;
    if (s->rate > RADIO_RATE_16M || (s->width != 20 && s->width != 40))
        return false;
    if ((s->filter & ~0x3F3Fu) || s->gain > 127)
        return false;
    if (s->rf_gain > 511 && s->rf_gain != RADIO_AUTO)
        return false;
    if (s->bb_gain > 127 && s->bb_gain != RADIO_AUTO)
        return false;
    for (unsigned r = 0; r < 4; r++)
        if (s->dc[r] > 511 && s->dc[r] != RADIO_AUTO)
            return false;
    if ((s->iq & ~0x3F1Fu) && s->iq != RADIO_AUTO)
        return false;
    return true;
}

unsigned radio_apply(const struct radio_settings *s)
{
    if (receiver.status == RADIO_PHY_FAILED || receiver.status == RADIO_NOT_STARTED)
        return receiver.status;
    if (!valid(s))
        return RADIO_PBUS_FAILED;
    struct radio_settings previous = settings;
    settings = *s;
    /* A retune also follows a failed one, whose PLL state is unknown. */
    bool retune = settings.lo_hz != previous.lo_hz || receiver.status == RADIO_PLL_FAILED;
    receiver.status = reconfigure(retune);
    if (receiver.status != RADIO_OK) {
        unsigned failed = receiver.status;
        settings = previous;
        receiver.status = reconfigure(true);
        return failed;
    }
    return RADIO_OK;
}

unsigned radio_tune(uint32_t lo_hz)
{
    struct radio_settings s = settings;
    s.lo_hz = lo_hz;
    return radio_apply(&s);
}

const struct radio_settings *radio_settings(void) { return &settings; }

uint32_t radio_sample_rate(void) { return settings.rate == RADIO_RATE_16M ? 16000000u : 80000000u; }

void radio_state(struct radio_state *out)
{
    uint32_t iq = REG(IQ_CORRECTION_REG);
    out->status = receiver.status;
    out->lo_hz = receiver.lo_hz;
    out->pll_cap = receiver.pll_cap;
    out->pll_first = receiver.pll_first;
    out->pll_length = receiver.pll_length;
    out->rf_gain = receiver.rf_gain;
    out->bb_gain = receiver.bb_gain & 127;
    memcpy(out->dc, receiver.dc, sizeof(out->dc));
    out->iq_amplitude = (iq >> 16) & 31;
    out->iq_phase = (iq >> 21) & 63;
    out->automatic = (settings.rf_gain == RADIO_AUTO ? 1 : 0) | (settings.bb_gain == RADIO_AUTO ? 2 : 0) |
                     (settings.iq == RADIO_AUTO ? 64 : 0);
    for (unsigned r = 0; r < 4; r++)
        if (settings.dc[r] == RADIO_AUTO)
            out->automatic |= 4u << r;
}

/* ---- one-shot capture ------------------------------------------------------------ */

const uint32_t *radio_snapshot(unsigned *first)
{
    volatile uint32_t *ring = (volatile uint32_t *)CAPTURE_BANK_BASE;
    uint32_t control = dump_control();

    if (receiver.status != RADIO_OK)
        return NULL;
    /* Two words that the writer must overwrite, as a sign that it ran. */
    ring[0] = ring[RADIO_RING_PAIRS / 2] = 0;
    memory_barrier();

    REG(DUMP_CTRL_REG) = control;
    REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | 1u;
    memory_barrier();
    REG(DUMP_CTRL_REG) = control | DUMP_CTRL_RUN;
    memory_barrier();
    delay_us(settings.rate == RADIO_RATE_16M ? SNAPSHOT_RUN_US_16M : SNAPSHOT_RUN_US_80M);

    /* The writer advances a few pairs between this read and the stop; the
     * seam guard skips that region. */
    uint32_t stop_index = REG(DUMP_WRITE_INDEX_REG) & RING_MASK;
    REG(DUMP_CTRL_REG) = control;
    memory_barrier();
    delay_us(SNAPSHOT_SETTLE_US);
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    memory_barrier();

    if (ring[0] == 0 && ring[RADIO_RING_PAIRS / 2] == 0)
        return NULL;
    *first = (stop_index + RADIO_SEAM_GUARD) & RING_MASK;
    return (const uint32_t *)CAPTURE_BANK_BASE;
}
