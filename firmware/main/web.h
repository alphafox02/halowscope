/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Web server: the page, and its WebSocket for spectrum frames and control.
 */

#pragma once

#include <stdbool.h>

/* A finished spectrum frame: dB per bin, bins evenly spread over
 * [start_hz, stop_hz). Fitted to the browser's view and sent, or merged
 * into the next frame while the link is busy. */
void web_publish(const float *db, unsigned bins, double start_hz, double stop_hz, bool peak);

/* A short message for the browser to show. */
void web_notice(const char *text);
