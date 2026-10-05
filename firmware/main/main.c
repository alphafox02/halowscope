/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * HaLowScope: a spectrum analyzer reached over Wi-Fi HaLow.
 */

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "halowscope.h"
#include "spectrum.h"
#include "storage.h"
#include "history.h"
#include "archive.h"

static const char *TAG = "halowscope";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* Do not erase: the partition holds the stored HaLow network. */
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    hs_console_start();
    hs_link_start();
    hs_web_start();
    history_start();
    spectrum_start();
    storage_start();
    archive_start();
    ESP_LOGI(TAG, "Ready");
}
