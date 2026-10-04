/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * The ESP32-S3's own 2.4 GHz receiver as an IQ source. Adapted from eSpDR
 * (https://github.com/h0m3us3r/eSpDR and github.com/alphafox02/eSpDR,
 * 0BSD): esp32s3/src/radio.h, board.h and protocol/control.h.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RADIO_LO_MIN_HZ 2210000000u /* the RF PLL locks from about 2207 to 2795 MHz */
#define RADIO_LO_MAX_HZ 2790000000u
#define RADIO_AUTO 0xFFFFu          /* rf_gain, bb_gain, iq: as the hardware selects */

enum radio_rate { RADIO_RATE_80M = 0, RADIO_RATE_16M = 1 };

enum radio_status {
    RADIO_OK = 0,
    RADIO_PHY_FAILED,
    RADIO_PLL_FAILED,
    RADIO_PBUS_FAILED,
    RADIO_NOT_STARTED,
};

/* Requested receiver settings. */
struct radio_settings {
    uint32_t lo_hz;
    unsigned rate;      /* enum radio_rate */
    unsigned width;     /* analog channel width, MHz: 20 or 40 */
    unsigned filter;    /* baseband RC filter codes: first | second << 8, each 0..63 */
    unsigned gain;      /* gain-table index 0..127 (not dB) */
    unsigned rf_gain;   /* RF stage word 0..511, or RADIO_AUTO */
    unsigned bb_gain;   /* baseband stage 0..127, or RADIO_AUTO */
    unsigned dc[4];     /* DC offset codes 0..511, or RADIO_AUTO */
    unsigned iq;        /* amplitude | phase << 8, or RADIO_AUTO */
};

/* The receiver as configured. */
struct radio_state {
    unsigned status;            /* enum radio_status */
    uint32_t lo_hz;             /* exact LO */
    unsigned pll_cap, pll_first, pll_length;
    unsigned rf_gain, bb_gain;  /* stage words in effect */
    unsigned dc[4];
    unsigned iq_amplitude, iq_phase;
    unsigned automatic;         /* RF 1, BB 2, DC 4 << register, IQ 64 */
};

/* Calibrates the PHY and configures the receiver with the defaults. */
unsigned radio_init(void);

/* Applies new settings (retuning only when the LO changed). On failure the
 * previous settings are restored. Returns enum radio_status. */
unsigned radio_apply(const struct radio_settings *s);

/* Retunes the LO only, keeping the other settings. */
unsigned radio_tune(uint32_t lo_hz);

const struct radio_settings *radio_settings(void);
void radio_state(struct radio_state *out);
uint32_t radio_sample_rate(void);

/*
 * One-shot capture: runs the sample writer into the capture bank long
 * enough to overwrite its whole ring, then stops it. Returns the ring and
 * sets *first to the index of the oldest usable pair; the
 * RADIO_SNAPSHOT_PAIRS pairs from there (wrapping at RADIO_RING_PAIRS) are
 * in time order. Each word holds I in bits 0..9 and Q in bits 10..19, two's
 * complement, with the LO-minus-RF convention. Returns NULL if the writer
 * did not run.
 */
#define RADIO_RING_PAIRS 16384u
#define RADIO_SEAM_GUARD 32u
#define RADIO_SNAPSHOT_PAIRS (RADIO_RING_PAIRS - 2u * RADIO_SEAM_GUARD)
const uint32_t *radio_snapshot(unsigned *first);
