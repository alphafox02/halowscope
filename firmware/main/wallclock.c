/*
 * Copyright 2026 CEMAXECUTER LLC
 */

#include "wallclock.h"

#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

static const char *TAG = "clock";

static volatile enum wallclock_source source;

static void synced(struct timeval *tv)
{
    if (source != CLOCK_NTP)
        ESP_LOGI(TAG, "time from NTP");
    source = CLOCK_NTP;
}

void wallclock_start(void)
{
    static bool started;
    if (started)
        return;
    started = true;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = synced;
    if (esp_netif_sntp_init(&config) != ESP_OK)
        ESP_LOGW(TAG, "SNTP did not start");
}

void wallclock_offer(double epoch_ms)
{
    if (source != CLOCK_NONE || epoch_ms < 1.6e12)
        return;
    struct timeval tv = { .tv_sec = (time_t)(epoch_ms / 1000), .tv_usec = (suseconds_t)((long long)epoch_ms % 1000 * 1000) };
    settimeofday(&tv, NULL);
    source = CLOCK_BROWSER;
    ESP_LOGI(TAG, "time from a browser until NTP answers");
}

enum wallclock_source wallclock_source(void) { return source; }

double wallclock_boot_epoch_ms(void)
{
    if (source == CLOCK_NONE)
        return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000 + tv.tv_usec / 1000.0 - esp_timer_get_time() / 1000.0;
}
