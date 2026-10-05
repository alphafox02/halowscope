/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * A stored spectrum line, shared by the history in PSRAM and its archive on
 * the TF card.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LINE_BINS 2048u
#define LINE_STEP_DB 0.5f   /* code 0: no data; code c: base + (c - 1) * LINE_STEP_DB */

struct line {
    int64_t t_us;           /* microseconds since boot */
    double start_hz, stop_hz;
    float base;
    uint16_t bins;
    uint8_t peak, pad;
    uint8_t code[LINE_BINS];
};

/* The fields before code[], for reading a record's header alone: a whole
 * struct line is too big for a task's stack. */
struct line_header {
    int64_t t_us;
    double start_hz, stop_hz;
    float base;
    uint16_t bins;
    uint8_t peak, pad;
};
_Static_assert(sizeof(struct line_header) == __builtin_offsetof(struct line, code),
               "struct line_header must match the start of struct line");

/* A request for rows, as history_rows() takes it. */
struct rows_request {
    double top_ms, row_ms;  /* ms since boot */
    unsigned rows, bins;
    double start_hz, stop_hz;
    float *out;             /* rows x bins */
    uint16_t *count;        /* for averages, or NULL for max-hold */
};

/*
 * Adds a line recorded at t_ms (since boot) to the rows it falls in.
 * Returns -1 if it is newer than the window, 1 if older (callers going from
 * new to old can stop there), 0 otherwise.
 */
int history_merge(const struct rows_request *r, const struct line *l, double t_ms);
