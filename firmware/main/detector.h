/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Detector: finds new, sustained, wideband emitters (video links, jammers)
 * in the spectrum lines the history stores, whether or not anyone is
 * watching, and reports them as events.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "history_line.h"

#define DETECTOR_RULES 4

struct detector_rule {
    bool on;
    char name[24];
    float start_mhz, stop_mhz;
    float threshold_db;   /* above the learned floor */
    float min_bw_mhz;     /* occupied width at least this */
    float min_s;          /* present this long ... */
};

struct detector_config {
    bool on;
    float duty;           /* ... in at least this share of lines (0..1) */
    float learn_min;      /* a signal present this long becomes the new normal */
    float end_s;          /* gone this long ends an event */
    float cooldown_s;     /* an event that returns within this reopens */
    struct detector_rule rule[DETECTOR_RULES];
};

/* What an event looks like, from its shape and behaviour (no trained model:
 * explicit rules, see classify() in detector.c). */
enum detector_type { TYPE_UNCLASSIFIED, TYPE_WIFI, TYPE_WIDE, TYPE_WANDERING, TYPE_STEADY, TYPE_COUNT };

/* Measured over an event's life; logged with it, as data for better rules
 * or a trained model later. */
struct detector_features {
    float center_mhz, center_sd_mhz;   /* mean and spread of the centre */
    float width_mhz, width_sd_mhz;     /* mean and spread of the occupied width */
    float duty;                        /* share of lines it was present in */
    float wifi_share;                  /* share of sightings shaped like a Wi-Fi channel */
    uint32_t samples;                  /* sightings */
};

struct detector_event {
    uint32_t id;
    uint8_t rule;
    uint8_t type;               /* enum detector_type */
    bool active;
    double start_ms, last_ms;   /* since boot */
    float lo_mhz, hi_mhz;       /* occupied range when strongest */
    float peak_dbfs, excess_db; /* strongest level, and its height over the floor */
    struct detector_features f;
};

enum detector_report { REPORT_START, REPORT_UPDATE, REPORT_END };

/* Loads the configuration (or the defaults) and starts the reporter. */
void detector_start(void);

/* Feeds a stored line (called by the history for every line it keeps). */
void detector_line(const struct line *l);

void detector_get_config(struct detector_config *out);
/* Validates, applies and saves a configuration; false if it was refused. */
bool detector_set_config(const struct detector_config *c);

/* Up to `max` most recent events, newest first; returns how many. */
unsigned detector_events(struct detector_event *out, unsigned max);
unsigned detector_active(void);
const char *detector_rule_name(unsigned rule);
const char *detector_type_name(unsigned type);

/* Called by the reporter for each event change, outside the spectrum task:
 * the web page and the outputs hook in here. */
typedef void (*detector_sink)(enum detector_report kind, const struct detector_event *e);
void detector_add_sink(detector_sink sink);
