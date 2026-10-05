/*
 * Copyright 2026 CEMAXECUTER LLC
 */

#include "flasher.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_format.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

static const char *TAG = "flasher";

/* Internal RAM: flash writes from PSRAM go through a 32-byte bounce buffer. */
#define CHUNK 1024

static SemaphoreHandle_t lock;
static struct flasher_status state = { .percent = -1 };

/* Reading the slots maps flash, which is not allowed from a task whose stack
 * is in PSRAM (the web server's), so they are read at startup and after each
 * job, and the page gets this copy. */
static struct flasher_slot *slots, *slots_new;   /* FLASHER_SLOTS each, in PSRAM */
static unsigned slot_count;

static struct {
    enum { JOB_INSTALL, JOB_BOOT } kind;
    char path[sizeof(FLASHER_DIR) + FLASHER_NAME_MAX];
    const esp_partition_t *slot;
    bool keep;
} job;

static void set_state(bool busy, int percent, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_state(bool busy, int percent, const char *fmt, ...)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    state.busy = busy;
    state.percent = percent;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state.text, sizeof(state.text), fmt, ap);
    va_end(ap);
    xSemaphoreGive(lock);
}

void flasher_status(struct flasher_status *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = state;
    xSemaphoreGive(lock);
}

static void copy_app(struct flasher_app *a, const esp_app_desc_t *d)
{
    strlcpy(a->project, d->project_name, sizeof(a->project));
    strlcpy(a->version, d->version, sizeof(a->version));
    strlcpy(a->date, d->date, sizeof(a->date));
    strlcpy(a->idf, d->idf_ver, sizeof(a->idf));
}

/* ---- slots ------------------------------------------------------------------------- */

static const esp_partition_t *slot_by_label(const char *label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
}

static void read_slots(void)
{
    struct flasher_slot *out = slots_new;
    const esp_partition_t *running = esp_ota_get_running_partition(), *next = esp_ota_get_boot_partition();
    unsigned n = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it && n < FLASHER_SLOTS; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        struct flasher_slot *s = &out[n++];
        memset(s, 0, sizeof(*s));
        strlcpy(s->label, p->label, sizeof(s->label));
        s->size = p->size;
        s->running = p == running;
        s->next = p == next;
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
            s->has_app = true;
            copy_app(&s->app, &d);
        }
    }
    esp_partition_iterator_release(it);
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(slots, out, FLASHER_SLOTS * sizeof(*slots));
    slot_count = n;
    xSemaphoreGive(lock);
}

unsigned flasher_slots(struct flasher_slot *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned n = slot_count;
    memcpy(out, slots, FLASHER_SLOTS * sizeof(*slots));
    xSemaphoreGive(lock);
    return n;
}

/* ---- files on the card ------------------------------------------------------------- */

bool flasher_path(const char *name, char *out, unsigned len)
{
    size_t n = strlen(name);
    if (n < 5 || n >= FLASHER_NAME_MAX || strcasecmp(name + n - 4, ".bin") != 0 || name[0] == '.' ||
        strpbrk(name, "/\\:*?\"<>|"))
        return false;
    return (unsigned)snprintf(out, len, "%s/%s", FLASHER_DIR, name) < len;
}

/* Reads an image's headers: the image header, the first segment's header,
 * then the app description that starts that segment. */
static bool read_app(FILE *f, struct flasher_app *a)
{
    struct {
        esp_image_header_t image;
        esp_image_segment_header_t segment;
        esp_app_desc_t desc;
    } h;
    _Static_assert(sizeof(h) == sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t),
                   "the headers follow each other with no padding");
    if (fread(&h, sizeof(h), 1, f) != 1)
        return false;
    if (h.image.magic != ESP_IMAGE_HEADER_MAGIC || h.image.chip_id != ESP_CHIP_ID_ESP32S3 ||
        h.desc.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return false;
    h.desc.project_name[sizeof(h.desc.project_name) - 1] = 0;
    h.desc.version[sizeof(h.desc.version) - 1] = 0;
    h.desc.date[sizeof(h.desc.date) - 1] = 0;
    h.desc.idf_ver[sizeof(h.desc.idf_ver) - 1] = 0;
    copy_app(a, &h.desc);
    return true;
}

bool flasher_files(struct flasher_file *out, unsigned max, unsigned *count)
{
    *count = 0;
    struct storage_status st;
    storage_status(&st);
    if (!st.mounted)
        return false;
    DIR *dir = opendir(FLASHER_DIR);
    if (!dir)
        return true;   /* no folder yet: no files */
    struct dirent *d;
    char path[sizeof(FLASHER_DIR) + FLASHER_NAME_MAX];
    while ((d = readdir(dir)) && *count < max) {
        if (d->d_type != DT_REG || !flasher_path(d->d_name, path, sizeof(path)))
            continue;
        struct flasher_file *ff = &out[(*count)++];
        memset(ff, 0, sizeof(*ff));
        strlcpy(ff->name, d->d_name, sizeof(ff->name));
        struct stat s;
        if (stat(path, &s) == 0)
            ff->size = s.st_size;
        FILE *f = fopen(path, "rb");
        if (f) {
            ff->ok = read_app(f, &ff->app);
            fclose(f);
        }
    }
    closedir(dir);
    return true;
}

bool flasher_remove(const char *name)
{
    char path[sizeof(FLASHER_DIR) + FLASHER_NAME_MAX];
    return flasher_path(name, path, sizeof(path)) && unlink(path) == 0;
}

/* ---- the work, on a task with its stack in internal RAM (it writes flash) --------------- */

static void install(void)
{
    const esp_partition_t *slot = job.slot;
    const char *name = strrchr(job.path, '/') + 1;
    FILE *f = fopen(job.path, "rb");
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    struct stat s;
    esp_ota_handle_t h = 0;
    esp_err_t err = ESP_OK;

    if (!f || !buf || fstat(fileno(f), &s) != 0) {
        set_state(false, -1, "Could not read %s.", name);
        goto out;
    }
    if ((uint32_t)s.st_size > slot->size) {
        set_state(false, -1, "%s does not fit in %s.", name, slot->label);
        goto out;
    }
    set_state(true, 0, "Erasing %s for %s...", slot->label, name);
    err = esp_ota_begin(slot, s.st_size, &h);
    for (size_t done = 0; err == ESP_OK && done < (size_t)s.st_size;) {
        size_t n = fread(buf, 1, CHUNK, f);
        if (n == 0) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = esp_ota_write(h, buf, n);
        done += n;
        set_state(true, (int)(done * 100 / s.st_size), "Writing %s into %s...", name, slot->label);
    }
    if (h) {
        if (err == ESP_OK)
            err = esp_ota_end(h);   /* checks the whole image */
        else
            esp_ota_abort(h);
    }
    if (err == ESP_OK) {
        set_state(false, -1, "%s is in %s.", name, slot->label);
        ESP_LOGI(TAG, "%s written into %s", name, slot->label);
    } else {
        set_state(false, -1, "Writing %s into %s failed: %s.", name, slot->label,
                  err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a valid image" : esp_err_to_name(err));
    }
out:
    if (f)
        fclose(f);
    free(buf);
}

/* esp_ota_set_boot_partition() marks its choice as new: the bootloader starts
 * it once, and goes back to the previous choice after the next reset unless
 * the app confirms itself. Keeping it means marking it confirmed here. */
static esp_err_t confirm_choice(void)
{
    const esp_partition_t *otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!otadata)
        return ESP_ERR_NOT_FOUND;
    esp_ota_select_entry_t e[2];
    int active = -1;
    for (int i = 0; i < 2; i++) {
        if (esp_partition_read(otadata, i * otadata->erase_size, &e[i], sizeof(e[i])) != ESP_OK)
            return ESP_FAIL;
        bool valid = e[i].ota_seq != UINT32_MAX && e[i].ota_state != ESP_OTA_IMG_INVALID &&
                     e[i].ota_state != ESP_OTA_IMG_ABORTED &&
                     e[i].crc == esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&e[i].ota_seq, 4);
        if (valid && (active < 0 || e[i].ota_seq > e[active].ota_seq))
            active = i;
    }
    if (active < 0)
        return ESP_ERR_NOT_FOUND;
    e[active].ota_state = ESP_OTA_IMG_VALID;
    esp_err_t err = esp_partition_erase_range(otadata, active * otadata->erase_size, otadata->erase_size);
    if (err == ESP_OK)
        err = esp_partition_write(otadata, active * otadata->erase_size, &e[active], sizeof(e[active]));
    return err;
}

/* Restarts so that the next firmware finds the board as at power on.
 * esp_restart() resets only the CPUs: GPIO interrupt settings survive it,
 * and the HaLow chip, still running, holds its IRQ line up; firmware that
 * does not expect that (the stock firmware) dies of an interrupt storm. So:
 * GPIO interrupts off, the HaLow chip held in reset, then a whole-system reset. */
static void restart_clean(void)
{
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++)
        if (GPIO_IS_VALID_GPIO(pin))
            gpio_intr_disable(pin);
    gpio_set_direction(CONFIG_MM_RESET_N, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_MM_RESET_N, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_rom_software_reset_system();
}

static void boot(void)
{
    const esp_partition_t *slot = job.slot;
    set_state(true, -1, "Checking %s...", slot->label);
    esp_err_t err = esp_ota_set_boot_partition(slot);   /* checks the whole image; factory: clears the choice */
    if (err == ESP_OK && job.keep && slot->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY)
        err = confirm_choice();
    if (err != ESP_OK) {
        set_state(false, -1, "Cannot start %s: %s.", slot->label,
                  err == ESP_ERR_OTA_VALIDATE_FAILED ? "no valid image in it" : esp_err_to_name(err));
        return;
    }
    set_state(true, -1, "Restarting into %s%s.", slot->label,
              slot->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ? "" : job.keep ? " (kept)" : " until the next restart");
    ESP_LOGW(TAG, "restarting into %s%s", slot->label, job.keep ? ", kept" : "");
    vTaskDelay(pdMS_TO_TICKS(1500));   /* lets the page hear it */
    restart_clean();
}

static void worker(void *arg)
{
    if (job.kind == JOB_INSTALL)
        install();
    else
        boot();
    read_slots();
    vTaskDelete(NULL);
}

static bool start_job(void)
{
    /* Internal RAM stack on purpose: this task writes the flash chip. It
     * exists only while a job runs. */
    if (xTaskCreate(worker, "flasher", 4096, NULL, 2, NULL) != pdPASS) {
        set_state(false, -1, "Not enough memory to start.");
        return false;
    }
    return true;
}

static bool claim(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool free = !state.busy;
    if (free) {
        state.busy = true;
        state.percent = -1;
        state.text[0] = 0;
    }
    xSemaphoreGive(lock);
    return free;
}

bool flasher_install(const char *name, const char *label)
{
    const esp_partition_t *slot = slot_by_label(label);
    if (!claim())
        return false;
    if (!slot || slot->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY || slot == esp_ota_get_running_partition()) {
        set_state(false, -1, "%s cannot be written: it is %s.", label,
                  !slot ? "not a slot" : slot == esp_ota_get_running_partition() ? "running" : "HaLowScope's own");
        return false;
    }
    if (!flasher_path(name, job.path, sizeof(job.path))) {
        set_state(false, -1, "Not a firmware file name: %s.", name);
        return false;
    }
    job.kind = JOB_INSTALL;
    job.slot = slot;
    return start_job();
}

bool flasher_boot(const char *label, bool keep)
{
    const esp_partition_t *slot = slot_by_label(label);
    if (!claim())
        return false;
    if (!slot) {
        set_state(false, -1, "%s is not a slot.", label);
        return false;
    }
    job.kind = JOB_BOOT;
    job.slot = slot;
    job.keep = keep;
    return start_job();
}

void flasher_start(void)
{
    lock = xSemaphoreCreateMutex();
    slots = heap_caps_calloc(2 * FLASHER_SLOTS, sizeof(*slots), MALLOC_CAP_SPIRAM);
    slots_new = slots + FLASHER_SLOTS;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t s;
    if (esp_ota_get_state_partition(running, &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY) {
        /* A HaLowScope started as a trial got this far: keep it. */
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "confirmed %s: %s", running->label, esp_err_to_name(err));
    }
    read_slots();
    ESP_LOGI(TAG, "running from %s", running->label);
}
