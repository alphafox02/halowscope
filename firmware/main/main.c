/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * HaLowScope: a spectrum analyzer reached over Wi-Fi HaLow.
 */

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "halowscope.h"
#include "spectrum.h"
#include "storage.h"
#include "history.h"
#include "archive.h"
#include "detector.h"
#include "settings.h"
#include "outputs.h"

static const char *TAG = "halowscope";

void app_main(void)
{
    /* A software restart (esp_restart, AT+RST, a crash) keeps the GPIO
     * interrupt settings of the run before. The HaLow chip's IRQ line is a
     * level interrupt, still asserted, and would fire endlessly as soon as
     * the GPIO interrupt service starts, before its handler is back. */
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++)
        if (GPIO_IS_VALID_GPIO(pin))
            gpio_intr_disable(pin);

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
    settings_start();
    history_start();
    detector_start();
    outputs_start();
    spectrum_start();
    storage_start();
    archive_start();
    ESP_LOGI(TAG, "Ready");
}
