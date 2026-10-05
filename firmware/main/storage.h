/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * The TF (microSD) card.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_ROOT "/sdcard"

struct storage_status {
    bool mounted;
    char name[8];          /* card's product name */
    uint32_t size_mb;      /* card capacity */
    uint32_t total_mb, free_mb;  /* filesystem */
    uint32_t clock_khz;
    /* Last self-test: 1 MB written, read back and compared, then removed. */
    bool tested, test_ok;
    uint32_t write_kbps, read_kbps;
};

/* Mounts the card if one is present (never formats it), then runs the
 * self-test in the background. */
void storage_start(void);
void storage_status(struct storage_status *out);

/* Erases the whole card: one partition across all of it, FAT32. */
esp_err_t storage_format(void);
