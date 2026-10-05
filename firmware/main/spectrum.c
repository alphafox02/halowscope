/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Spectrum engine. One task owns the radio: it applies settings between
 * frames, takes snapshots, runs the FFTs and hands each finished frame (dB
 * per bin over a frequency range) to the web server.
 *
 * Fixed mode: every frame averages (or peak-holds) the FFTs of as many
 * snapshots as fit in the frame time. Sweep mode: the LO steps across the
 * range, the central part of each capture is kept, and one frame covers
 * the whole sweep.
 *
 * Levels are dBFS: a full-scale complex tone reads 0 dB in its bin.
 */

#include "spectrum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#include "radio.h"
#include "web.h"

static const char *TAG = "spectrum";

#define MAX_FFT 8192u
#define RING_MASK (RADIO_RING_PAIRS - 1u)
#define MAX_SWEEP_BINS 65536u
/* Part of each capture a sweep keeps, as a fraction of the sample rate:
 * the analog filter rolls off towards the edges at 80 Msps. */
#define SWEEP_KEEP 0.75
/* Bins either side of DC replaced by their neighbours when notching. */
#define DC_NOTCH_HZ 150000.0

const char *const spectrum_window_names[WINDOW_COUNT] = {"hann", "blackman-harris", "rectangular"};
/* Equivalent noise bandwidth in bins. */
const float spectrum_window_bandwidth[WINDOW_COUNT] = {1.50f, 2.00f, 1.00f};

enum request_kind { REQ_SET, REQ_FFT, REQ_SWEEP, REQ_RUN };

struct request {
    enum request_kind kind;
    union {
        struct { char name[12]; char value[28]; } set;
        struct fft_config fft;
        struct sweep_config sweep;
        bool run;
    };
};

static QueueHandle_t requests;
static SemaphoreHandle_t status_lock;
static struct spectrum_status status = {
    .running = true,
    .fft = { .size = 2048, .window = WINDOW_BLACKMAN_HARRIS, .rate = 20, .averaging = 0, .peak = false, .dc = true },
    .sweep = { .on = false, .start_hz = 2400000000u, .stop_hz = 2500000000u },
};

/* Stage timing, CPU cycles summed over the current frame. */
static struct { uint32_t unpack, window, fft, bitrev, power, capture; } cycles;
#define CYCLES_PER_US 240.0f

/* Work buffers: the FFT's own in internal RAM, the rest where it fits. */
static float *fft_buf;    /* 2 * MAX_FFT, complex interleaved */
static float *window;     /* window_size */
static float *power;      /* window_size, accumulated linear power in FFT bin order */
static float window_sum;
static unsigned window_size, window_kind = WINDOW_COUNT;
/* Frame being built, dB per output bin; PSRAM for long sweeps. */
static float *line;
/* The LO of fixed mode, restored when a sweep ends. */
static uint32_t fixed_lo;

/* ---- requests ---------------------------------------------------------------- */

static void post(const struct request *r)
{
    if (requests && xQueueSend(requests, r, 0) != pdTRUE)
        web_notice("Busy: setting dropped, try again.");
}

void spectrum_set(const char *name, const char *value)
{
    struct request r = { .kind = REQ_SET };
    strlcpy(r.set.name, name, sizeof(r.set.name));
    strlcpy(r.set.value, value, sizeof(r.set.value));
    post(&r);
}

void spectrum_set_fft(const struct fft_config *c)
{
    struct request r = { .kind = REQ_FFT, .fft = *c };
    post(&r);
}

void spectrum_set_sweep(const struct sweep_config *c)
{
    struct request r = { .kind = REQ_SWEEP, .sweep = *c };
    post(&r);
}

void spectrum_run(bool on)
{
    struct request r = { .kind = REQ_RUN, .run = on };
    post(&r);
}

void spectrum_status(struct spectrum_status *out)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(status_lock);
}

/* Parses "auto" or a number into a radio setting. */
static bool number_or_auto(const char *v, unsigned *out)
{
    if (strcmp(v, "auto") == 0) {
        *out = RADIO_AUTO;
        return true;
    }
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < 0)
        return false;
    *out = (unsigned)n;
    return true;
}

static const char *const radio_errors[] = {
    [RADIO_OK] = "", [RADIO_PHY_FAILED] = "PHY calibration failed",
    [RADIO_PLL_FAILED] = "PLL did not lock", [RADIO_PBUS_FAILED] = "setting rejected or PBUS failed",
    [RADIO_NOT_STARTED] = "radio not started",
};

static void apply_set(const char *name, const char *value)
{
    struct radio_settings s = *radio_settings();
    unsigned n = 0;
    bool ok = true;

    if (strcmp(name, "lo") == 0) {
        double hz = strtod(value, NULL);
        ok = hz >= RADIO_LO_MIN_HZ && hz <= RADIO_LO_MAX_HZ;
        s.lo_hz = (uint32_t)llround(hz);
        /* Tuning to a frequency leaves a sweep for the fixed view there. */
        xSemaphoreTake(status_lock, portMAX_DELAY);
        status.sweep.on = false;
        xSemaphoreGive(status_lock);
    } else if (strcmp(name, "rate") == 0) {
        ok = number_or_auto(value, &n) && (n == 80 || n == 16);
        s.rate = n == 16 ? RADIO_RATE_16M : RADIO_RATE_80M;
        /* 16 Msps is the 80 Msps capture decimated by five with no digital
         * filter, so the analog filter has to keep out what would fold in:
         * code 54 (eSpDR's measurement) for 16 Msps, the widest for 80. */
        s.filter = n == 16 ? (54u | 54u << 8) : 0;
    } else if (strcmp(name, "width") == 0) {
        ok = number_or_auto(value, &n) && (n == 20 || n == 40);
        s.width = n;
    } else if (strcmp(name, "filter") == 0) {
        unsigned a, b;
        ok = sscanf(value, "%u,%u", &a, &b) == 2 && a <= 63 && b <= 63;
        s.filter = a | b << 8;
    } else if (strcmp(name, "gain") == 0) {
        ok = number_or_auto(value, &n) && n <= 127;
        s.gain = n;
    } else if (strcmp(name, "rf") == 0) {
        ok = number_or_auto(value, &s.rf_gain);
    } else if (strcmp(name, "bb") == 0) {
        ok = number_or_auto(value, &s.bb_gain);
    } else if (strncmp(name, "dc", 2) == 0 && name[2] >= '0' && name[2] <= '3' && !name[3]) {
        ok = number_or_auto(value, &s.dc[name[2] - '0']);
    } else if (strcmp(name, "iq") == 0) {
        int a, p;
        if (strcmp(value, "auto") == 0) {
            s.iq = RADIO_AUTO;
        } else {
            ok = sscanf(value, "%d,%d", &a, &p) == 2 && a >= -16 && a <= 15 && p >= -32 && p <= 31;
            s.iq = (unsigned)(a & 31) | (unsigned)(p & 63) << 8;
        }
    } else {
        web_notice("Unknown setting.");
        return;
    }
    if (!ok) {
        web_notice("Value out of range.");
        return;
    }
    int64_t t = esp_timer_get_time();
    unsigned result = radio_apply(&s);
    if (strcmp(name, "lo") == 0) {
        if (result == RADIO_OK)
            fixed_lo = s.lo_hz;
        xSemaphoreTake(status_lock, portMAX_DELAY);
        status.retune_us = (uint32_t)(esp_timer_get_time() - t);
        xSemaphoreGive(status_lock);
    }
    if (result != RADIO_OK) {
        char text[64];
        snprintf(text, sizeof(text), "Receiver: %s.", radio_errors[result < 5 ? result : 3]);
        web_notice(text);
    }
}

static void handle_requests(void)
{
    struct request r;
    while (xQueueReceive(requests, &r, 0) == pdTRUE) {
        switch (r.kind) {
        case REQ_SET:
            apply_set(r.set.name, r.set.value);
            break;
        case REQ_FFT:
            xSemaphoreTake(status_lock, portMAX_DELAY);
            status.fft = r.fft;
            xSemaphoreGive(status_lock);
            break;
        case REQ_SWEEP:
            xSemaphoreTake(status_lock, portMAX_DELAY);
            status.sweep = r.sweep;
            xSemaphoreGive(status_lock);
            break;
        case REQ_RUN:
            xSemaphoreTake(status_lock, portMAX_DELAY);
            status.running = r.run;
            xSemaphoreGive(status_lock);
            break;
        }
    }
}

/* ---- FFT ------------------------------------------------------------------------- */

/* The window and the power accumulator are sized for the FFT in use:
 * internal RAM when there is room, PSRAM otherwise. */
static float *buffer_for(float *old, unsigned n)
{
    heap_caps_free(old);
    float *p = heap_caps_aligned_alloc(16, n * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p || heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 40000) {
        heap_caps_free(p);
        p = heap_caps_aligned_alloc(16, n * sizeof(float), MALLOC_CAP_SPIRAM);
    }
    return p;
}

static void prepare_window(unsigned n, unsigned kind)
{
    if (n == window_size && kind == window_kind)
        return;
    if (n != window_size) {
        window = buffer_for(window, n);
        power = buffer_for(power, n);
    }
    if (kind == WINDOW_HANN)
        dsps_wind_hann_f32(window, n);
    else if (kind == WINDOW_BLACKMAN_HARRIS)
        dsps_wind_blackman_harris_f32(window, n);
    else
        for (unsigned i = 0; i < n; i++)
            window[i] = 1.0f;
    window_sum = 0;
    for (unsigned i = 0; i < n; i++)
        window_sum += window[i];
    window_size = n;
    window_kind = kind;
}

/*
 * FFTs of `blocks` consecutive n-pair blocks of a snapshot, added to (or
 * max-held in) power[]. Returns the number of FFTs done.
 */
static unsigned analyse(const uint32_t *ring, unsigned first, unsigned n, unsigned blocks, bool peak, bool dc)
{
    for (unsigned b = 0; b < blocks; b++) {
        unsigned at = first + b * n;
        float mean_i = 0, mean_q = 0;
        uint32_t c0 = esp_cpu_get_cycle_count();
        for (unsigned k = 0; k < n; k++) {
            uint32_t w = ring[(at + k) & RING_MASK];
            float i = (float)((int)((w & 1023u) ^ 512u) - 512);
            float q = (float)((int)(((w >> 10) & 1023u) ^ 512u) - 512);
            fft_buf[2 * k] = i;
            fft_buf[2 * k + 1] = q;
            mean_i += i;
            mean_q += q;
        }
        if (!dc)
            mean_i = mean_q = 0;
        mean_i /= (float)n;
        mean_q /= (float)n;
        uint32_t c1 = esp_cpu_get_cycle_count();
        for (unsigned k = 0; k < n; k++) {
            fft_buf[2 * k] = (fft_buf[2 * k] - mean_i) * window[k];
            fft_buf[2 * k + 1] = (fft_buf[2 * k + 1] - mean_q) * window[k];
        }
        uint32_t c2 = esp_cpu_get_cycle_count();
        dsps_fft2r_fc32(fft_buf, n);
        uint32_t c3 = esp_cpu_get_cycle_count();
        dsps_bit_rev_fc32(fft_buf, n);
        uint32_t c4 = esp_cpu_get_cycle_count();
        if (peak) {
            for (unsigned k = 0; k < n; k++) {
                float p = fft_buf[2 * k] * fft_buf[2 * k] + fft_buf[2 * k + 1] * fft_buf[2 * k + 1];
                if (p > power[k])
                    power[k] = p;
            }
        } else {
            for (unsigned k = 0; k < n; k++)
                power[k] += fft_buf[2 * k] * fft_buf[2 * k] + fft_buf[2 * k + 1] * fft_buf[2 * k + 1];
        }
        uint32_t c5 = esp_cpu_get_cycle_count();
        cycles.unpack += c1 - c0;
        cycles.window += c2 - c1;
        cycles.fft += c3 - c2;
        cycles.bitrev += c4 - c3;
        cycles.power += c5 - c4;
    }
    return blocks;
}

/*
 * Writes bins [from, to) of the LO-centred spectrum, in ascending RF
 * order, as dB into out[]. Output bin j is RF = LO - rate/2 + j * rate/n;
 * with the LO-minus-RF convention that is FFT bin (n/2 - j) mod n.
 */
/*
 * 10 log10(x) for x > 0, from the float's exponent and a degree-4 polynomial
 * for log2 of its mantissa: within 0.001 dB, far finer than the 0.5 dB steps
 * the page shows, and several times faster than log10f.
 */
static inline float fast_db(float x)
{
    union { float f; uint32_t u; } v = { .f = x };
    float e = (float)((int)((v.u >> 23) & 255) - 127);
    v.u = (v.u & 0x7FFFFFu) | 0x3F800000u;
    float m = v.f;
    float l = (((-0.07914958f * m + 0.62880993f) * m - 2.08104467f) * m + 4.02835512f) * m - 2.49676657f;
    return 3.01029996f * (e + l);
}

static void to_db(float *out, unsigned n, unsigned from, unsigned to, unsigned ffts, bool peak, bool notch,
                  double rate)
{
    /* Full-scale complex tone: amplitude 512 in I and Q. */
    float scale = 1.0f / (512.0f * 512.0f * window_sum * window_sum);
    if (!peak && ffts)
        scale /= (float)ffts;
    float offset = 10.0f * log10f(scale);
    for (unsigned j = from; j < to; j++) {
        float p = power[(n / 2 - j) & (n - 1)];
        out[j - from] = p > 0 ? fast_db(p) + offset : -200.0f;
    }
    if (!notch)
        return;
    /* Replace the bins around DC (output bin n/2) by the level just outside. */
    unsigned half = (unsigned)ceil(DC_NOTCH_HZ / (rate / n));
    unsigned lo = n / 2 - half - 1, hi = n / 2 + half + 1;
    if (lo < from || hi >= to)
        return;
    float edge = fminf(out[lo - from], out[hi - from]);
    for (unsigned j = lo + 1; j < hi; j++)
        out[j - from] = edge;
}

/* ---- frames ------------------------------------------------------------------------ */

static float fps_estimate;

static void count_frame(int64_t *last)
{
    int64_t now = esp_timer_get_time();
    if (*last) {
        float f = 1e6f / (float)(now - *last);
        fps_estimate = fps_estimate ? 0.8f * fps_estimate + 0.2f * f : f;
    }
    *last = now;
}

/* One fixed-mode frame. */
static void fixed_frame(const struct fft_config *c, int64_t *last_frame)
{
    unsigned n = c->size;
    double rate = radio_sample_rate();
    int64_t start = esp_timer_get_time(), deadline = start + 1000000 / c->rate;
    unsigned ffts = 0, snapshots = 0, failures = 0;
    uint32_t snapshot_us = 0;

    memset(power, 0, n * sizeof(float));
    memset(&cycles, 0, sizeof(cycles));
    do {
        unsigned first;
        int64_t t = esp_timer_get_time();
        uint32_t c0 = esp_cpu_get_cycle_count();
        const uint32_t *ring = radio_snapshot(&first);
        cycles.capture += esp_cpu_get_cycle_count() - c0;
        if (!ring) {
            failures++;
            vTaskDelay(1);
            continue;
        }
        unsigned blocks = RADIO_SNAPSHOT_PAIRS / n;
        if (c->averaging && blocks > c->averaging - ffts)
            blocks = c->averaging - ffts;
        ffts += analyse(ring, first, n, blocks, c->peak, c->dc);
        snapshots++;
        snapshot_us = (uint32_t)(esp_timer_get_time() - t);
        /* Leave time for HaLow, lwIP and the web server. */
        vTaskDelay(1);
    } while ((c->averaging == 0 || ffts < c->averaging) && esp_timer_get_time() < deadline && failures < 8);

    int64_t work_end = esp_timer_get_time(), t_db = 0, t_pub = 0;
    if (ffts) {
        to_db(line, n, 0, n, ffts, c->peak, c->dc, rate);
        t_db = esp_timer_get_time();
        double lo = radio_settings()->lo_hz;
        web_publish(line, n, lo - rate / 2, lo + rate / 2, c->peak);
        t_pub = esp_timer_get_time();
        count_frame(last_frame);
    }
    int64_t busy = esp_timer_get_time() - start;

    /* Pace the frame first, so the coverage covers the whole frame interval,
     * including the wait when a fixed averaging count finished early. */
    int64_t left = deadline - esp_timer_get_time();
    if (left > 1000)
        vTaskDelay(pdMS_TO_TICKS(left / 1000));

    int64_t elapsed = esp_timer_get_time() - start;
    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.centre_hz = radio_settings()->lo_hz;
    status.span_hz = rate;
    status.fps = fps_estimate;
    status.coverage = elapsed > 0 ? (float)(ffts * n) / (float)(rate * elapsed / 1e6) : 0;
    status.snapshot_us = snapshot_us;
    status.failures += failures;
    if (ffts) {
        float per = CYCLES_PER_US * ffts;
        status.profile.unpack = cycles.unpack / per;
        status.profile.window = cycles.window / per;
        status.profile.fft = cycles.fft / per;
        status.profile.bitrev = cycles.bitrev / per;
        status.profile.power = cycles.power / per;
        status.profile.capture = cycles.capture / CYCLES_PER_US;
        status.profile.to_db = (float)(t_db - work_end);
        status.profile.publish = (float)(t_pub - t_db);
        status.profile.frame = (float)busy;
        status.profile.ffts = ffts;
        status.profile.snapshots = snapshots;
    }
    xSemaphoreGive(status_lock);
}

/* One sweep: LO steps across [start, stop], one frame for all of it. */
static void sweep_frame(const struct fft_config *c, const struct sweep_config *sw, int64_t *last_frame)
{
    double rate = radio_sample_rate();
    unsigned n = c->size;
    double bin_hz = rate / n;
    unsigned keep = (unsigned)(SWEEP_KEEP * n) & ~1u;    /* bins kept per step, centred */
    double step = keep * bin_hz;
    double start = sw->start_hz, stop = sw->stop_hz;
    if (stop - start < step)
        stop = start + step;
    unsigned steps = (unsigned)ceil((stop - start) / step);
    unsigned total = steps * keep;
    if (total > MAX_SWEEP_BINS) {
        web_notice("Sweep too fine: use a smaller FFT size or a narrower range.");
        xSemaphoreTake(status_lock, portMAX_DELAY);
        status.sweep.on = false;
        xSemaphoreGive(status_lock);
        return;
    }
    int64_t began = esp_timer_get_time();
    uint32_t retune_us = 0;
    unsigned failures = 0;

    for (unsigned s = 0; s < steps; s++) {
        /* The LO sits at the middle of the kept bins of this step. */
        double lo = start + (s + 0.5) * step;
        if (lo < RADIO_LO_MIN_HZ)
            lo = RADIO_LO_MIN_HZ;
        if (lo > RADIO_LO_MAX_HZ)
            lo = RADIO_LO_MAX_HZ;
        int64_t t = esp_timer_get_time();
        if (radio_tune((uint32_t)lo) != RADIO_OK) {
            for (unsigned j = 0; j < keep; j++)
                line[s * keep + j] = -200.0f;
            continue;
        }
        retune_us = (uint32_t)(esp_timer_get_time() - t);

        memset(power, 0, n * sizeof(float));
        unsigned first, ffts = 0;
        const uint32_t *ring = radio_snapshot(&first);
        if (ring) {
            unsigned blocks = RADIO_SNAPSHOT_PAIRS / n;
            if (c->averaging && blocks > c->averaging)
                blocks = c->averaging;
            ffts = analyse(ring, first, n, blocks, c->peak, true);
        } else {
            failures++;
        }
        if (ffts)
            to_db(line + s * keep, n, n / 2 - keep / 2, n / 2 + keep / 2, ffts, c->peak, true, rate);
        else
            for (unsigned j = 0; j < keep; j++)
                line[s * keep + j] = -200.0f;
        vTaskDelay(1);

        /* A setting or a stop arriving mid-sweep ends this sweep early. */
        if (uxQueueMessagesWaiting(requests))
            return;
    }
    web_publish(line, total, start, start + total * bin_hz, c->peak);
    count_frame(last_frame);

    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.centre_hz = start + total * bin_hz / 2;
    status.span_hz = total * bin_hz;
    status.fps = fps_estimate;
    status.coverage = 0;
    status.retune_us = retune_us;
    status.sweep_ms = (uint32_t)((esp_timer_get_time() - began) / 1000);
    status.failures += failures;
    xSemaphoreGive(status_lock);
}

static void spectrum_task(void *arg)
{
    int64_t last_frame = 0;
    bool was_sweeping = false;
    fixed_lo = radio_settings()->lo_hz;

    for (;;) {
        handle_requests();
        struct spectrum_status s;
        spectrum_status(&s);
        if (!s.running) {
            fps_estimate = 0;
            last_frame = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        prepare_window(s.fft.size, s.fft.window);
        if (s.sweep.on) {
            was_sweeping = true;
            sweep_frame(&s.fft, &s.sweep, &last_frame);
        } else {
            /* Back from a sweep: return to the fixed-mode LO (a tune request,
             * which also ends a sweep, sets a new one). */
            if (was_sweeping && radio_settings()->lo_hz != fixed_lo)
                radio_tune(fixed_lo);
            was_sweeping = false;
            fixed_frame(&s.fft, &last_frame);
        }
    }
}

void spectrum_start(void)
{
    status_lock = xSemaphoreCreateMutex();
    requests = xQueueCreate(16, sizeof(struct request));
    fft_buf = heap_caps_aligned_alloc(16, 2 * MAX_FFT * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    line = heap_caps_malloc(MAX_SWEEP_BINS * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!fft_buf || !line) {
        ESP_LOGE(TAG, "out of memory (largest internal block %u bytes)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    if (dsps_fft2r_init_fc32(NULL, MAX_FFT) != ESP_OK) {
        ESP_LOGE(TAG, "FFT init failed");
        return;
    }
    if (radio_init() != RADIO_OK)
        ESP_LOGE(TAG, "radio failed to start; the page will show no data");
    xTaskCreatePinnedToCore(spectrum_task, "spectrum", 6144, NULL, 3, NULL, 1);
}
