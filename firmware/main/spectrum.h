/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Spectrum engine: snapshots from the radio, FFTs, averaging, sweeps.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum spectrum_window { WINDOW_HANN, WINDOW_BLACKMAN_HARRIS, WINDOW_RECTANGULAR, WINDOW_COUNT };

struct fft_config {
    unsigned size;       /* 512..8192 */
    unsigned window;     /* enum spectrum_window */
    unsigned rate;       /* frames per second wanted, 1..100 */
    unsigned averaging;  /* FFTs per frame; 0: as many as the frame time allows */
    bool peak;           /* peak detector instead of the average */
    bool dc;             /* remove DC: subtract the mean and notch the centre */
};

struct sweep_config {
    bool on;
    uint32_t start_hz, stop_hz;
};

struct spectrum_status {
    bool running;
    struct fft_config fft;
    struct sweep_config sweep;
    /* The span the frames cover: the receiver's band, or the sweep. */
    double centre_hz, span_hz;
    float fps;            /* frames produced per second */
    float coverage;       /* share of the samples analysed (fixed mode) */
    uint32_t snapshot_us; /* time for one snapshot and its FFTs */
    uint32_t retune_us;   /* last LO change */
    uint32_t sweep_ms;    /* last complete sweep */
    unsigned failures;    /* snapshots where the writer did not run */
    /* Where the time goes, from the last fixed-mode frame: microseconds per
     * FFT block for each stage, and per frame for the rest. */
    struct {
        float unpack, window, fft, bitrev, power;   /* per FFT block */
        float capture, to_db, publish, frame;       /* per frame */
        unsigned ffts, snapshots;
    } profile;
};

/* Calibrates the radio and starts the spectrum task. */
void spectrum_start(void);

/* Requests from the web page, applied by the spectrum task between frames. */
void spectrum_set(const char *name, const char *value);  /* receiver: lo, rate, gain, ... */
void spectrum_set_fft(const struct fft_config *c);
void spectrum_set_sweep(const struct sweep_config *c);
void spectrum_run(bool on);

void spectrum_status(struct spectrum_status *out);
extern const char *const spectrum_window_names[WINDOW_COUNT];
extern const float spectrum_window_bandwidth[WINDOW_COUNT];
