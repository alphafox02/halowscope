/*
 * Copyright 2026 CEMAXECUTER LLC
 */

#include "settings.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "halowscope"

struct request {
    char key[16];
    size_t len;
    uint8_t data[SETTINGS_MAX];
};

static QueueHandle_t queue;

static void writer(void *arg)
{
    static struct request r;
    for (;;) {
        if (xQueueReceive(queue, &r, portMAX_DELAY) != pdTRUE)
            continue;
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, r.key, r.data, r.len);
            if (err == ESP_OK)
                err = nvs_commit(h);
            nvs_close(h);
        }
        if (err != ESP_OK)
            ESP_LOGW(TAG, "saving %s: %s", r.key, esp_err_to_name(err));
    }
}

void settings_start(void)
{
    queue = xQueueCreate(2, sizeof(struct request));
    /* Internal RAM stack on purpose: this task writes the flash chip. */
    xTaskCreate(writer, "settings", 3072, NULL, 2, NULL);
}

void settings_save(const char *key, const void *data, size_t len)
{
    static struct request r;   /* callers are rare; the copy goes into the queue */
    if (!queue || len > SETTINGS_MAX)
        return;
    strlcpy(r.key, key, sizeof(r.key));
    r.len = len;
    memcpy(r.data, data, len);
    if (xQueueSend(queue, &r, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGW(TAG, "could not queue %s", key);
}
