/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Saving settings to NVS from any task. Writing the flash chip is not
 * allowed from a task whose stack is in PSRAM (the web server's is), so
 * saves are handed to a small task with its stack in internal RAM.
 */

#pragma once

#include <stddef.h>

#define SETTINGS_MAX 512

void settings_start(void);
/* Queues a copy of `data` (at most SETTINGS_MAX bytes) to be saved under `key`. */
void settings_save(const char *key, const void *data, size_t len);
