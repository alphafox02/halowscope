/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * The spectrum history's archive on the TF card, for hours and days.
 */

#pragma once

#include <stdbool.h>
#include "history_line.h"

/* Starts the writer if a card is mounted. Lines are archived while the
 * wall-clock time is known, as the archive is indexed by it. */
void archive_start(void);

/* Queues a stored line for the card; never blocks. */
void archive_line(const struct line *l);

/* Whether lines are being archived, and how many could not be (card behind). */
bool archive_running(void);
unsigned archive_dropped(void);

/* The oldest archived time, in ms since this boot (negative: before it). */
bool archive_oldest_ms(double *ms);

/*
 * Merges archived lines into a rows request, newest first: from the coarse
 * archive (one line every few seconds) or the fine one (every stored line),
 * only lines older than `before_ms` (ms since boot). Returns the newest time
 * the archive covered, or -INFINITY if it had nothing for the window.
 */
double archive_rows(const struct rows_request *r, bool coarse, double before_ms);
