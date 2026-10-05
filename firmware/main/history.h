/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Spectrum history: the last several minutes of spectrum lines, kept in
 * PSRAM whether or not anyone is watching, so the page can scroll back.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Times are milliseconds since boot, the clock the page already syncs to. */

void history_start(void);

/* Records a finished spectrum frame (dB per bin over [start_hz, stop_hz)).
 * Frames closer together than a history line are merged by max-hold. */
void history_record(const float *db, unsigned bins, double start_hz, double stop_hz, bool peak);

struct history_span {
    double oldest_ms, newest_ms;
    unsigned lines, capacity;   /* lines stored, and the most it can hold */
    float line_ms;              /* shortest interval between stored lines */
};
void history_span(struct history_span *out);

/*
 * Fills rows x bins of dB values for a time window, newest row first: row r
 * covers (top_ms - (r + 1) * row_ms, top_ms - r * row_ms], each bin is the
 * maximum over the frequencies it covers in [start_hz, stop_hz) and, over
 * the lines in its time, the maximum (or with `mean`, the average: steadier
 * for an overview of a long stretch). NAN where nothing was recorded.
 * Returns false if there is no history.
 */
bool history_rows(double top_ms, double row_ms, unsigned rows, double start_hz, double stop_hz,
                  unsigned bins, bool mean, float *out);
