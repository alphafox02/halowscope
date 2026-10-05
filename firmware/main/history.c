/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Spectrum history in PSRAM.
 *
 * Frames from the spectrum task are merged by max-hold into one line per
 * LINE_MS (so a short burst survives), pooled to at most LINE_BINS bins by
 * maximum, and stored as one byte per bin in a ring of fixed-size slots,
 * newest overwriting oldest. Each line keeps its own frequency range, so
 * retunes and sweeps mix freely. Reads merge lines into the rows a screen
 * asks for, again by max-hold.
 */

#include "history.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "history";

#define LINE_BINS 2048u
#define LINE_MS 100
#define RING_BYTES (6u * 1024u * 1024u)
#define STEP_DB 0.5f      /* code 0: no data; code c: base + (c - 1) * STEP_DB */

struct line {
    int64_t t_us;
    double start_hz, stop_hz;
    float base;
    uint16_t bins;
    uint8_t peak, pad;
    uint8_t code[LINE_BINS];
};

static struct line *ring;
static unsigned capacity, head, count;    /* head: next slot to write */
static SemaphoreHandle_t lock;

/* The line being built from frames within LINE_MS of each other. */
static float *acc;
static unsigned acc_bins;
static double acc_start, acc_stop;
static int64_t acc_t;
static bool acc_peak, acc_have;

void history_start(void)
{
    lock = xSemaphoreCreateMutex();
    capacity = RING_BYTES / sizeof(struct line);
    ring = heap_caps_malloc((size_t)capacity * sizeof(struct line), MALLOC_CAP_SPIRAM);
    acc = heap_caps_malloc(LINE_BINS * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!ring || !acc) {
        ESP_LOGE(TAG, "no PSRAM for the history");
        capacity = 0;
        return;
    }
    ESP_LOGI(TAG, "%u lines of up to %u bins, one per %d ms at most", capacity, LINE_BINS, LINE_MS);
}

/* Quantizes the pending line into the next slot. */
static void flush(void)
{
    float low = INFINITY, high = -INFINITY;
    for (unsigned i = 0; i < acc_bins; i++) {
        low = fminf(low, acc[i]);
        high = fmaxf(high, acc[i]);
    }
    float base = fmaxf(low, high - 254 * STEP_DB);
    xSemaphoreTake(lock, portMAX_DELAY);
    struct line *l = &ring[head];
    l->t_us = acc_t;
    l->start_hz = acc_start;
    l->stop_hz = acc_stop;
    l->base = base;
    l->bins = acc_bins;
    l->peak = acc_peak;
    for (unsigned i = 0; i < acc_bins; i++) {
        float c = roundf((acc[i] - base) / STEP_DB) + 1;
        l->code[i] = c < 1 ? 1 : c > 255 ? 255 : (uint8_t)c;
    }
    head = (head + 1) % capacity;
    if (count < capacity)
        count++;
    xSemaphoreGive(lock);
    acc_have = false;
}

void history_record(const float *db, unsigned bins, double start_hz, double stop_hz, bool peak)
{
    if (!capacity || !bins)
        return;
    /* Pool to at most LINE_BINS bins by maximum. */
    unsigned f = (bins + LINE_BINS - 1) / LINE_BINS;
    unsigned out = (bins + f - 1) / f;
    double stop = start_hz + (double)out * f * (stop_hz - start_hz) / bins;
    int64_t now = esp_timer_get_time();

    if (acc_have && (now - acc_t >= LINE_MS * 1000 || out != acc_bins || start_hz != acc_start ||
                     stop != acc_stop || peak != acc_peak))
        flush();
    bool fresh = !acc_have;
    for (unsigned j = 0; j < out; j++) {
        float v = db[j * f];
        for (unsigned i = j * f + 1; i < (j + 1) * f && i < bins; i++)
            v = fmaxf(v, db[i]);
        acc[j] = fresh ? v : fmaxf(acc[j], v);
    }
    if (fresh) {
        acc_have = true;
        acc_t = now;
        acc_bins = out;
        acc_start = start_hz;
        acc_stop = stop;
        acc_peak = peak;
    }
}

void history_span(struct history_span *out)
{
    memset(out, 0, sizeof(*out));
    out->capacity = capacity;
    out->line_ms = LINE_MS;
    if (!capacity)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    out->lines = count;
    if (count) {
        out->newest_ms = ring[(head + capacity - 1) % capacity].t_us / 1000.0;
        out->oldest_ms = ring[(head + capacity - count) % capacity].t_us / 1000.0;
    }
    xSemaphoreGive(lock);
}

bool history_rows(double top_ms, double row_ms, unsigned rows, double start_hz, double stop_hz,
                  unsigned bins, bool mean, float *out)
{
    for (unsigned i = 0; i < rows * bins; i++)
        out[i] = NAN;
    if (!capacity || !rows || !bins || row_ms <= 0 || stop_hz <= start_hz)
        return false;
    uint16_t *n = mean ? heap_caps_calloc((size_t)rows * bins, sizeof(uint16_t), MALLOC_CAP_SPIRAM) : NULL;
    if (mean && !n)
        return false;

    /* Only the index is read under the lock; a slot that is overwritten while
     * we read it is the oldest one, at the far end of any window. */
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned h = head, c = count;
    xSemaphoreGive(lock);

    double out_bw = (stop_hz - start_hz) / bins;
    for (unsigned k = 0; k < c; k++) {
        const struct line *l = &ring[(h + capacity - 1 - k) % capacity];
        double t = l->t_us / 1000.0;
        if (t > top_ms)
            continue;
        unsigned r = (unsigned)((top_ms - t) / row_ms);
        if (r >= rows)
            break;                       /* lines only get older from here */
        double line_bw = (l->stop_hz - l->start_hz) / l->bins;
        /* Line bins per output bin, and where output bin 0 starts, in line bins. */
        float per = (float)(out_bw / line_bw);
        float first = (float)((start_hz - l->start_hz) / line_bw);
        float *row = out + (size_t)r * bins;
        uint16_t *rn = n ? n + (size_t)r * bins : NULL;
        for (unsigned j = 0; j < bins; j++) {
            float x0 = first + j * per, x1 = x0 + per;
            if (x1 <= 0 || x0 >= l->bins)
                continue;
            int i0 = x0 < 0 ? 0 : (int)x0, i1 = (int)x1;
            if (x1 > (float)i1)
                i1++;
            if (i1 > l->bins)
                i1 = l->bins;
            if (i1 <= i0)
                i1 = i0 + 1;
            uint8_t m = 0;
            for (int i = i0; i < i1; i++)
                if (l->code[i] > m)
                    m = l->code[i];
            if (!m)
                continue;
            float v = l->base + (m - 1) * STEP_DB;
            if (rn) {
                row[j] = rn[j] ? row[j] + v : v;
                rn[j]++;
            } else if (isnan(row[j]) || v > row[j]) {
                row[j] = v;
            }
        }
    }
    if (n) {
        for (unsigned i = 0; i < rows * bins; i++)
            if (n[i])
                out[i] /= n[i];
        heap_caps_free(n);
    }
    return c > 0;
}
