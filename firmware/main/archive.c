/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * The spectrum history's archive on the TF card.
 *
 * Two archives, both of fixed-size records in time order, so that a time can
 * be found by binary search instead of reading a file from its start:
 *
 *   fine/YYYYMMDD/HH.dat   every line the history stores (about 7 a second)
 *   coarse/YYYYMMDD.dat    one line every COARSE_MS, max-hold, COARSE_BINS
 *
 * A record is a struct line (see history_line.h) cut to its slot size, with
 * t_us holding wall-clock microseconds since 1970 (UTC; file names too) and
 * pad set to RECORD_MARK. A partial record at the end of a file, as a power
 * cut can leave, is ignored. Lines are archived only while the wall-clock
 * time is known. When the card runs low, the oldest hours of the fine
 * archive go first.
 */

#include "archive.h"

#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "storage.h"
#include "wallclock.h"

static const char *TAG = "archive";

#define ROOT STORAGE_ROOT "/halowscope"
#define FINE_DIR ROOT "/fine"
#define COARSE_DIR ROOT "/coarse"
#define COARSE_BINS 512u
#define COARSE_MS 2000
#define HEADER offsetof(struct line, code)
#define FINE_SLOT (HEADER + LINE_BINS)
#define COARSE_SLOT (HEADER + COARSE_BINS)
#define RECORD_MARK 0xA5
#define POOL_LINES 192         /* about 27 s of lines, for when reads keep the card busy */
#define KEEP_FREE_MB 1024
#define READ_BLOCK 16          /* records per read */

static bool running;
static struct line *pool;      /* lines waiting for the writer */
static unsigned pool_next;
static QueueHandle_t queue;
static unsigned dropped;
static volatile double oldest_epoch_ms = INFINITY;

/* ---- names ------------------------------------------------------------------ */

static void day_of(double epoch_ms, struct tm *tm)
{
    time_t t = (time_t)(epoch_ms / 1000);
    gmtime_r(&t, tm);
}

static void fine_path(char *buf, size_t len, double epoch_ms)
{
    struct tm tm;
    day_of(epoch_ms, &tm);
    snprintf(buf, len, FINE_DIR "/%04d%02d%02d/%02d.dat", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
}

static void coarse_path(char *buf, size_t len, double epoch_ms)
{
    struct tm tm;
    day_of(epoch_ms, &tm);
    snprintf(buf, len, COARSE_DIR "/%04d%02d%02d.dat", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* ---- writing ------------------------------------------------------------------ */

struct sink {
    FILE *f;
    char path[64];
    size_t slot;
};

/* Opens (creating directories) the file for a path, closing the previous one. */
static FILE *sink_for(struct sink *s, const char *path)
{
    if (s->f && strcmp(s->path, path) == 0)
        return s->f;
    if (s->f)
        fclose(s->f);
    s->f = NULL;
    char dir[64];
    strlcpy(dir, path, sizeof(dir));
    for (char *p = dir + strlen(ROOT) + 1; (p = strchr(p, '/')); p++) {
        *p = 0;
        mkdir(dir, 0775);
        *p = '/';
    }
    s->f = fopen(path, "ab");
    if (!s->f) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return NULL;
    }
    strlcpy(s->path, path, sizeof(s->path));
    /* Skip a partial record left by a power cut, so slots stay aligned. */
    long size = ftell(s->f);
    if (size % (long)s->slot) {
        static const uint8_t zero[64];
        size_t fill = s->slot - size % s->slot;
        while (fill) {
            size_t n = fill < sizeof(zero) ? fill : sizeof(zero);
            fwrite(zero, 1, n, s->f);
            fill -= n;
        }
    }
    if (!isfinite(oldest_epoch_ms))
        oldest_epoch_ms = NAN;   /* found again on the next query */
    return s->f;
}

static void put(struct sink *s, const struct line *l, double epoch_ms, const char *path)
{
    FILE *f = sink_for(s, path);
    if (!f)
        return;
    struct line_header head;
    memcpy(&head, l, HEADER);
    head.t_us = (int64_t)(epoch_ms * 1000);
    head.pad = RECORD_MARK;
    fwrite(&head, 1, HEADER, f);
    fwrite(l->code, 1, l->bins, f);
    static const uint8_t zero[64];
    size_t fill = s->slot - HEADER - l->bins;
    while (fill) {
        size_t n = fill < sizeof(zero) ? fill : sizeof(zero);
        fwrite(zero, 1, n, f);
        fill -= n;
    }
}

/* Coarse line being built. */
static float coarse_acc[COARSE_BINS];
static struct line coarse_line;
static double coarse_epoch;
static bool coarse_have;

static void coarse_flush(struct sink *s)
{
    if (!coarse_have)
        return;
    float low = INFINITY, high = -INFINITY;
    for (unsigned i = 0; i < coarse_line.bins; i++)
        if (isfinite(coarse_acc[i])) {
            low = fminf(low, coarse_acc[i]);
            high = fmaxf(high, coarse_acc[i]);
        }
    coarse_line.base = isfinite(low) ? fmaxf(low, high - 254 * LINE_STEP_DB) : 0;
    for (unsigned i = 0; i < coarse_line.bins; i++) {
        if (!isfinite(coarse_acc[i])) {
            coarse_line.code[i] = 0;
            continue;
        }
        float c = roundf((coarse_acc[i] - coarse_line.base) / LINE_STEP_DB) + 1;
        coarse_line.code[i] = c < 1 ? 1 : c > 255 ? 255 : (uint8_t)c;
    }
    char path[64];
    coarse_path(path, sizeof(path), coarse_epoch);
    put(s, &coarse_line, coarse_epoch, path);
    coarse_have = false;
}

static void coarse_add(struct sink *s, const struct line *l, double epoch_ms)
{
    unsigned f = (l->bins + COARSE_BINS - 1) / COARSE_BINS;
    unsigned out = (l->bins + f - 1) / f;
    double stop = l->start_hz + (double)out * f * (l->stop_hz - l->start_hz) / l->bins;
    if (coarse_have && (epoch_ms - coarse_epoch >= COARSE_MS || out != coarse_line.bins ||
                        l->start_hz != coarse_line.start_hz || stop != coarse_line.stop_hz))
        coarse_flush(s);
    if (!coarse_have) {
        coarse_have = true;
        coarse_epoch = epoch_ms;
        coarse_line.bins = out;
        coarse_line.start_hz = l->start_hz;
        coarse_line.stop_hz = stop;
        coarse_line.peak = l->peak;
        for (unsigned j = 0; j < out; j++)
            coarse_acc[j] = -INFINITY;
    }
    for (unsigned j = 0; j < out; j++)
        for (unsigned i = j * f; i < (j + 1) * f && i < l->bins; i++)
            if (l->code[i]) {
                float v = l->base + (l->code[i] - 1) * LINE_STEP_DB;
                if (v > coarse_acc[j])
                    coarse_acc[j] = v;
            }
}

/* Deletes the oldest hours of the fine archive while the card is short of space. */
static int oldest_entry(const char *dir, char *name, size_t len)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (!found || strcmp(e->d_name, name) < 0) {
            strlcpy(name, e->d_name, len);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static void keep_space(struct sink *fine)
{
    for (int pass = 0; pass < 24; pass++) {
        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info(STORAGE_ROOT, &total, &free_bytes) != ESP_OK || free_bytes >> 20 >= KEEP_FREE_MB)
            return;
        char day[32], hour[32], path[96];
        if (!oldest_entry(FINE_DIR, day, sizeof(day)))
            return;
        snprintf(path, sizeof(path), FINE_DIR "/%s", day);
        if (!oldest_entry(path, hour, sizeof(hour))) {
            rmdir(path);
            continue;
        }
        snprintf(path, sizeof(path), FINE_DIR "/%s/%s", day, hour);
        if (fine->f && strcmp(fine->path, path) == 0)
            return;                            /* only the file being written is left */
        ESP_LOGI(TAG, "card low on space: removing %s", path);
        unlink(path);
        oldest_epoch_ms = NAN;
    }
}

static void writer(void *arg)
{
    struct sink fine = { .slot = FINE_SLOT }, coarse = { .slot = COARSE_SLOT };
    int64_t last_sync = esp_timer_get_time(), last_space = 0;
    for (;;) {
        unsigned idx;
        if (xQueueReceive(queue, &idx, pdMS_TO_TICKS(1000)) == pdTRUE) {
            const struct line *l = &pool[idx];
            double epoch = l->t_us / 1000.0 + wallclock_boot_epoch_ms();
            char path[64];
            fine_path(path, sizeof(path), epoch);
            put(&fine, l, epoch, path);
            coarse_add(&coarse, l, epoch);
        }
        int64_t now = esp_timer_get_time();
        /* Commit to the card every few seconds: a power cut loses at most that. */
        if (now - last_sync > 5000000) {
            last_sync = now;
            for (struct sink *s = &fine; s; s = s == &fine ? &coarse : NULL)
                if (s->f) {
                    fflush(s->f);
                    fsync(fileno(s->f));
                }
        }
        if (now - last_space > 60000000) {
            last_space = now;
            keep_space(&fine);
        }
    }
}

void archive_start(void)
{
    struct storage_status st;
    storage_status(&st);
    if (!st.mounted)
        return;
    pool = heap_caps_malloc(POOL_LINES * sizeof(struct line), MALLOC_CAP_SPIRAM);
    queue = xQueueCreate(POOL_LINES - 2, sizeof(unsigned));
    if (!pool || !queue)
        return;
    mkdir(ROOT, 0775);
    mkdir(FINE_DIR, 0775);
    mkdir(COARSE_DIR, 0775);
    oldest_epoch_ms = NAN;
    running = true;
    /* Stack in PSRAM: this task only touches the card, never the flash chip.
     * Above the history reader's priority: recording must not lose lines,
     * while a reader can wait. */
    xTaskCreateWithCaps(writer, "archive", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "archiving to %s", ROOT);
}

void archive_line(const struct line *l)
{
    if (!running || wallclock_source() == CLOCK_NONE)
        return;
    /* Pool slots outnumber queue entries by two, so a queued slot is never
     * reused before the writer has taken it. */
    unsigned idx = pool_next;
    memcpy(&pool[idx], l, HEADER + l->bins);
    if (xQueueSend(queue, &idx, 0) == pdTRUE)
        pool_next = (pool_next + 1) % POOL_LINES;
    else
        dropped++;
}

bool archive_running(void) { return running && wallclock_source() != CLOCK_NONE; }
unsigned archive_dropped(void) { return dropped; }

/* ---- reading -------------------------------------------------------------------- */

static bool read_record(FILE *f, size_t slot, long k, struct line_header *head)
{
    return fseek(f, k * (long)slot, SEEK_SET) == 0 && fread(head, 1, HEADER, f) == HEADER &&
           head->pad == RECORD_MARK;
}

static double first_epoch(const char *path, size_t slot)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return INFINITY;
    struct line_header head;
    double t = read_record(f, slot, 0, &head) ? head.t_us / 1000.0 : INFINITY;
    fclose(f);
    return t;
}

bool archive_oldest_ms(double *ms)
{
    double boot = wallclock_boot_epoch_ms();
    if (!running || !boot)
        return false;
    if (isnan(oldest_epoch_ms)) {
        char name[32], sub[32], path[96];
        double oldest = INFINITY;
        if (oldest_entry(COARSE_DIR, name, sizeof(name))) {
            snprintf(path, sizeof(path), COARSE_DIR "/%s", name);
            oldest = first_epoch(path, COARSE_SLOT);
        }
        if (oldest_entry(FINE_DIR, name, sizeof(name))) {
            snprintf(path, sizeof(path), FINE_DIR "/%s", name);
            if (oldest_entry(path, sub, sizeof(sub))) {
                snprintf(path, sizeof(path), FINE_DIR "/%s/%s", name, sub);
                oldest = fmin(oldest, first_epoch(path, FINE_SLOT));
            }
        }
        oldest_epoch_ms = oldest;
    }
    if (!isfinite(oldest_epoch_ms))
        return false;
    *ms = oldest_epoch_ms - boot;
    return true;
}

/* Merges one file's records with times in [bottom, top] (wall clock), newest
 * first. Returns false once records older than the window were reached. */
static bool merge_file(const char *path, size_t slot, double bottom, double top, double boot,
                       const struct rows_request *r, uint8_t *block, double *newest)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return true;
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / (long)slot;
    struct line_header head;
    /* The last record at or before the top of the window. */
    long lo = 0, hi = n - 1, k = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (!read_record(f, slot, mid, &head)) {
            hi = mid - 1;         /* a gap (unwritten slot): look earlier */
            continue;
        }
        if (head.t_us / 1000.0 <= top) {
            k = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    bool more = true;
    while (k >= 0 && more) {
        long first = k - READ_BLOCK + 1 < 0 ? 0 : k - READ_BLOCK + 1;
        long count = k - first + 1;
        if (fseek(f, first * (long)slot, SEEK_SET) != 0 || fread(block, slot, count, f) != (size_t)count)
            break;
        for (long i = count - 1; i >= 0; i--) {
            const struct line *l = (const struct line *)(block + i * slot);
            if (l->pad != RECORD_MARK)
                continue;
            double t = l->t_us / 1000.0;
            if (t < bottom) {
                more = false;
                break;
            }
            if (history_merge(r, l, t - boot) == 0 && t - boot > *newest)
                *newest = t - boot;
        }
        k = first - 1;
    }
    fclose(f);
    return more;
}

double archive_rows(const struct rows_request *r, bool coarse, double before_ms)
{
    double boot = wallclock_boot_epoch_ms();
    if (!running || !boot)
        return -INFINITY;
    double top = fmin(r->top_ms, before_ms) + boot, bottom = r->top_ms - r->rows * r->row_ms + boot;
    if (top <= bottom)
        return -INFINITY;
    size_t slot = coarse ? COARSE_SLOT : FINE_SLOT;
    uint8_t *block = heap_caps_malloc(READ_BLOCK * slot, MALLOC_CAP_SPIRAM);
    if (!block)
        return -INFINITY;
    double newest = -INFINITY;
    /* Files from the one holding `top` back to the one holding `bottom`. */
    double step = coarse ? 86400000.0 : 3600000.0;
    char path[64], last[64] = "";
    for (double t = top; t > bottom - step; t -= step) {
        double at = fmax(t, bottom);
        if (coarse)
            coarse_path(path, sizeof(path), at);
        else
            fine_path(path, sizeof(path), at);
        if (strcmp(path, last) == 0)
            continue;
        strlcpy(last, path, sizeof(last));
        if (!merge_file(path, slot, bottom, top, boot, r, block, &newest))
            break;
    }
    heap_caps_free(block);
    return newest;
}
