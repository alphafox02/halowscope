/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Wall-clock time: from NTP when the network reaches the internet, else
 * from the first browser that connects, else none (times since boot).
 */

#pragma once

#include <stdint.h>

enum wallclock_source { CLOCK_NONE, CLOCK_BROWSER, CLOCK_NTP };

/* Starts NTP; call once the network is up. */
void wallclock_start(void);

/* A browser's clock (ms since 1970), used only while nothing better is set. */
void wallclock_offer(double epoch_ms);

enum wallclock_source wallclock_source(void);

/* Milliseconds since 1970 at boot (0 if unknown): wall time = this + ms since boot. */
double wallclock_boot_epoch_ms(void);
