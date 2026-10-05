/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * The TF (microSD) card, on the wiring the stock firmware uses: SDMMC host,
 * 1-bit bus, CLK GPIO15, CMD GPIO16, D0 GPIO11, internal pull-ups.
 */

#include "storage.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

#define TEST_DIR STORAGE_ROOT "/halowscope"
#define TEST_FILE TEST_DIR "/selftest.bin"
#define TEST_BYTES (1024 * 1024)
#define TEST_CHUNK 4096

static sdmmc_card_t *card;
static struct storage_status state;

static void refresh_space(void)
{
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(STORAGE_ROOT, &total, &free_bytes) == ESP_OK) {
        state.total_mb = (uint32_t)(total >> 20);
        state.free_mb = (uint32_t)(free_bytes >> 20);
    }
}

/* Writes TEST_BYTES of a known pattern, reads them back, compares, removes. */
static void self_test(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(10000));   /* let HaLow and the spectrum start first */
    uint8_t *buf = malloc(TEST_CHUNK);
    bool ok = buf != NULL;
    int64_t t0 = 0, t1 = 0, t2 = 0;

    if (mkdir(TEST_DIR, 0775) != 0 && errno != EEXIST)
        ESP_LOGW(TAG, "mkdir %s: %s", TEST_DIR, strerror(errno));
    FILE *f = ok ? fopen(TEST_FILE, "wb") : NULL;
    if (ok && !f)
        ESP_LOGW(TAG, "open %s for writing: %s", TEST_FILE, strerror(errno));
    ok = ok && f;
    if (ok) {
        t0 = esp_timer_get_time();
        for (unsigned n = 0; n < TEST_BYTES / TEST_CHUNK && ok; n++) {
            for (unsigned i = 0; i < TEST_CHUNK; i++)
                buf[i] = (uint8_t)(n * 31 + i * 7);
            ok = fwrite(buf, 1, TEST_CHUNK, f) == TEST_CHUNK;
        }
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
        fclose(f);
        t1 = esp_timer_get_time();
    }
    f = ok ? fopen(TEST_FILE, "rb") : NULL;
    ok = ok && f;
    if (ok) {
        for (unsigned n = 0; n < TEST_BYTES / TEST_CHUNK && ok; n++) {
            ok = fread(buf, 1, TEST_CHUNK, f) == TEST_CHUNK;
            for (unsigned i = 0; i < TEST_CHUNK && ok; i++)
                ok = buf[i] == (uint8_t)(n * 31 + i * 7);
        }
        fclose(f);
        t2 = esp_timer_get_time();
    }
    unlink(TEST_FILE);
    rmdir(TEST_DIR);
    free(buf);

    state.tested = true;
    state.test_ok = ok;
    if (ok) {
        /* KiB per second, from microseconds. */
        state.write_kbps = (uint32_t)((uint64_t)TEST_BYTES / 1024 * 1000000 / (t1 - t0));
        state.read_kbps = (uint32_t)((uint64_t)TEST_BYTES / 1024 * 1000000 / (t2 - t1));
    }
    refresh_space();
    ESP_LOGI(TAG, "self-test %s: write %lu KB/s, read %lu KB/s", ok ? "passed" : "FAILED",
             (unsigned long)state.write_kbps, (unsigned long)state.read_kbps);
    vTaskDelete(NULL);
}

void storage_start(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 10000;   /* as the stock firmware; plenty for this use */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_15;
    slot.cmd = GPIO_NUM_16;
    slot.d0 = GPIO_NUM_11;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,   /* never touch a card we cannot read */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_ROOT, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card mounted (%s)", esp_err_to_name(err));
        return;
    }
    sdmmc_card_print_info(stdout, card);
    state.mounted = true;
    memcpy(state.name, card->cid.name, sizeof(card->cid.name));
    state.name[sizeof(state.name) - 1] = 0;
    state.size_mb = (uint32_t)((uint64_t)card->csd.capacity * card->csd.sector_size >> 20);
    state.clock_khz = card->real_freq_khz;
    refresh_space();
    xTaskCreate(self_test, "sdtest", 4096, NULL, 1, NULL);
}

void storage_status(struct storage_status *out) { *out = state; }

esp_err_t storage_format(void)
{
    if (!card)
        return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "formatting the whole card");
    esp_err_t err = esp_vfs_fat_sdcard_format(STORAGE_ROOT, card);
    refresh_space();
    ESP_LOGI(TAG, "format %s: %lu MB free of %lu MB", esp_err_to_name(err),
             (unsigned long)state.free_mb, (unsigned long)state.total_mb);
    return err;
}
