/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Detector for new, sustained, wideband emitters.
 *
 * Every stored history line (about 7 a second, live view or sweep) is laid
 * onto a fixed grid of GRID_STEP cells across the tunable range, so what
 * it learns holds across retunes. Each cell learns its usual level: a slow
 * average, in dB, of its lines. A busy access point's channel is usually
 * busy, so it is normal there; what stands out is a level above what that
 * frequency usually shows. A signal that stays for `learn_min` minutes
 * becomes the new normal. For WARMUP_S after start the cells learn quickly
 * and no events are raised, as everything on the air would look new.
 *
 * For each rule, runs of cells more than `threshold_db` above their usual level
 * and at least `min_bw_mhz` wide are candidates. A candidate that keeps
 * coming back (in `duty` of the lines) for `min_s` opens an event; one gone
 * for `end_s` closes it; one that returns within `cooldown_s` reopens it.
 * Ordinary Wi-Fi traffic is bursty and narrow-in-time, so it rarely holds
 * the duty; a video link or a jammer is on all the time.
 *
 * Event changes are queued to a reporter task, which logs them to the TF
 * card and hands them to the web page and the outputs, so nothing slow
 * happens in the spectrum task.
 */

#include "detector.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage.h"
#include "wallclock.h"

static const char *TAG = "detector";

#define GRID_START_HZ 2180e6
#define GRID_STEP_HZ 250e3
#define GRID_CELLS 2560           /* to 2820 MHz */
#define TRACKS 16
#define EVENTS 100
#define LINES_PER_S 7.0f          /* nominal, for the learning rate */
#define WARMUP_S 60               /* learning before any event */
#define WARMUP_RATE 0.05f         /* share of the difference learned per line meanwhile */
#define RUN_GAP 2                 /* quiet cells allowed inside a run */
#define UPDATE_EVERY_MS 10000     /* reports while an event lasts */
#define LOG_PATH STORAGE_ROOT "/halowscope/events.jsonl"
#define NVS_NAMESPACE "halowscope"
#define NVS_KEY "detector"
#define CONFIG_VERSION 1

struct track {
    bool used, hit;
    uint8_t rule;
    float lo, hi, peak, excess;   /* MHz, dBFS, dB */
    double first_ms, last_ms;
    unsigned seen, total;
    int event;                    /* index in events[], or -1 */
    double reported_ms;
};

struct report {
    enum detector_report kind;
    struct detector_event e;
};

static struct detector_config config;
static SemaphoreHandle_t config_lock, events_lock;
static float *floor_db, *excess, *level;   /* GRID_CELLS each; floor_db: the usual level */
static double started_ms = -1;
static uint8_t *covered;
static struct track tracks[TRACKS];
static struct detector_event *events;      /* ring of EVENTS */
static unsigned events_head, events_count;
static uint32_t next_id = 1;
static QueueHandle_t reports;
static detector_sink sinks[4];

static const struct detector_config defaults = {
    .on = true, .duty = 0.7f, .learn_min = 5, .end_s = 3, .cooldown_s = 30,
    .rule = {
        { true, "2.4 GHz video / jammer", 2400, 2483.5f, 10, 5, 2 },
        { false, "2.3 GHz band", 2300, 2400, 10, 5, 2 },
        { false, "2.5-2.7 GHz", 2496, 2690, 10, 5, 2 },
        { false, "Custom", 2400, 2500, 10, 5, 2 },
    },
};

/* ---- configuration ------------------------------------------------------------ */

static bool valid(const struct detector_config *c)
{
    if (!(c->duty >= 0.1f && c->duty <= 1) || !(c->learn_min >= 0.5f && c->learn_min <= 1440) ||
        !(c->end_s >= 0.5f && c->end_s <= 120) || !(c->cooldown_s >= 0 && c->cooldown_s <= 3600))
        return false;
    for (unsigned r = 0; r < DETECTOR_RULES; r++) {
        const struct detector_rule *u = &c->rule[r];
        if (!(u->start_mhz >= 2180 && u->stop_mhz <= 2820 && u->stop_mhz - u->start_mhz >= 1) ||
            !(u->threshold_db >= 3 && u->threshold_db <= 60) || !(u->min_bw_mhz >= 0.25f && u->min_bw_mhz <= 200) ||
            !(u->min_s >= 0.2f && u->min_s <= 600) || memchr(u->name, 0, sizeof(u->name)) == NULL)
            return false;
    }
    return true;
}

static void load_config(void)
{
    struct { uint32_t version; struct detector_config c; } saved;
    size_t len = sizeof(saved);
    nvs_handle_t h;
    config = defaults;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return;
    if (nvs_get_blob(h, NVS_KEY, &saved, &len) == ESP_OK && len == sizeof(saved) &&
        saved.version == CONFIG_VERSION && valid(&saved.c))
        config = saved.c;
    nvs_close(h);
}

void detector_get_config(struct detector_config *out)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    *out = config;
    xSemaphoreGive(config_lock);
}

bool detector_set_config(const struct detector_config *c)
{
    if (!valid(c))
        return false;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    config = *c;
    xSemaphoreGive(config_lock);
    struct { uint32_t version; struct detector_config c; } saved = { CONFIG_VERSION, *c };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, &saved, sizeof(saved));
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

const char *detector_rule_name(unsigned rule)
{
    return rule < DETECTOR_RULES ? config.rule[rule].name : "";
}

/* ---- events ---------------------------------------------------------------------- */

static void report(enum detector_report kind, const struct detector_event *e)
{
    struct report r = { kind, *e };
    if (reports && xQueueSend(reports, &r, 0) != pdTRUE)
        ESP_LOGW(TAG, "report queue full");
}

static int open_event(struct track *t, double now)
{
    xSemaphoreTake(events_lock, portMAX_DELAY);
    /* The same emitter back within the cooldown: reopen its event. */
    for (unsigned k = 0; k < events_count; k++) {
        unsigned i = (events_head + EVENTS - 1 - k) % EVENTS;
        struct detector_event *e = &events[i];
        if (!e->active && e->rule == t->rule && now - e->last_ms < config.cooldown_s * 1000 &&
            t->lo < e->hi_mhz && t->hi > e->lo_mhz) {
            e->active = true;
            e->last_ms = now;
            xSemaphoreGive(events_lock);
            report(REPORT_UPDATE, e);
            return (int)i;
        }
    }
    unsigned i = events_head;
    events[i] = (struct detector_event){
        .id = next_id++, .rule = t->rule, .active = true, .start_ms = t->first_ms, .last_ms = now,
        .lo_mhz = t->lo, .hi_mhz = t->hi, .peak_dbfs = t->peak, .excess_db = t->excess,
    };
    events_head = (events_head + 1) % EVENTS;
    if (events_count < EVENTS)
        events_count++;
    xSemaphoreGive(events_lock);
    report(REPORT_START, &events[i]);
    return (int)i;
}

unsigned detector_events(struct detector_event *out, unsigned max)
{
    xSemaphoreTake(events_lock, portMAX_DELAY);
    unsigned n = events_count < max ? events_count : max;
    for (unsigned k = 0; k < n; k++)
        out[k] = events[(events_head + EVENTS - 1 - k) % EVENTS];
    xSemaphoreGive(events_lock);
    return n;
}

unsigned detector_active(void)
{
    unsigned n = 0;
    xSemaphoreTake(events_lock, portMAX_DELAY);
    for (unsigned k = 0; k < events_count; k++)
        n += events[k].active;
    xSemaphoreGive(events_lock);
    return n;
}

/* ---- detection ---------------------------------------------------------------------- */

static int cell_of(double hz)
{
    return (int)floor((hz - GRID_START_HZ) / GRID_STEP_HZ);
}

static double hz_of(int cell)
{
    return GRID_START_HZ + cell * GRID_STEP_HZ;
}

void detector_line(const struct line *l)
{
    if (!floor_db || !l->bins)
        return;
    struct detector_config c;
    detector_get_config(&c);
    if (!c.on)
        return;
    double now = l->t_us / 1000.0;
    if (started_ms < 0)
        started_ms = now;
    bool warming = now - started_ms < WARMUP_S * 1000;
    float rate = warming ? WARMUP_RATE : 1.0f / (c.learn_min * 60 * LINES_PER_S);

    /* Lay the line onto the grid and update each cell's floor. */
    int c0 = (int)ceil((l->start_hz - GRID_START_HZ) / GRID_STEP_HZ);
    int c1 = cell_of(l->stop_hz) - 1;
    if (c0 < 0)
        c0 = 0;
    if (c1 >= GRID_CELLS)
        c1 = GRID_CELLS - 1;
    memset(covered, 0, GRID_CELLS);
    float per = (float)(GRID_STEP_HZ / ((l->stop_hz - l->start_hz) / l->bins));
    float first = (float)((GRID_START_HZ - l->start_hz) / ((l->stop_hz - l->start_hz) / l->bins));
    for (int k = c0; k <= c1; k++) {
        float x0 = first + k * per, x1 = x0 + per;
        int i0 = x0 < 0 ? 0 : (int)x0, i1 = (int)ceilf(x1);
        if (i1 > l->bins)
            i1 = l->bins;
        if (i1 <= i0)
            i1 = i0 + 1;
        if (i0 >= l->bins)
            continue;
        uint8_t m = 0;
        for (int i = i0; i < i1; i++)
            if (l->code[i] > m)
                m = l->code[i];
        if (!m)
            continue;
        float v = l->base + (m - 1) * LINE_STEP_DB;
        if (isnan(floor_db[k]))
            floor_db[k] = v;
        excess[k] = v - floor_db[k];
        level[k] = v;
        floor_db[k] += (v - floor_db[k]) * rate;
        covered[k] = 1;
    }

    if (warming)
        return;

    /* Candidates: runs of hot cells, rule by rule. */
    for (unsigned t = 0; t < TRACKS; t++)
        tracks[t].hit = false;
    for (unsigned r = 0; r < DETECTOR_RULES; r++) {
        const struct detector_rule *u = &c.rule[r];
        if (!u->on)
            continue;
        int a = cell_of(u->start_mhz * 1e6), b = cell_of(u->stop_mhz * 1e6) - 1;
        if (a < c0)
            a = c0;
        if (b > c1)
            b = c1;
        if (a > b)
            continue;                  /* this rule's band is not in view */
        for (unsigned t = 0; t < TRACKS; t++)
            if (tracks[t].used && tracks[t].rule == r)
                tracks[t].total++;
        for (int k = a; k <= b;) {
            if (!covered[k] || excess[k] < u->threshold_db) {
                k++;
                continue;
            }
            int s = k, e = k, gap = 0;
            float peak = level[k], exc = excess[k];
            for (k++; k <= b && gap <= RUN_GAP; k++) {
                if (covered[k] && excess[k] >= u->threshold_db) {
                    e = k;
                    gap = 0;
                    peak = fmaxf(peak, level[k]);
                    exc = fmaxf(exc, excess[k]);
                } else {
                    gap++;
                }
            }
            float lo = (float)(hz_of(s) / 1e6), hi = (float)(hz_of(e + 1) / 1e6);
            if (hi - lo < u->min_bw_mhz)
                continue;
            struct track *m = NULL, *free_t = NULL;
            for (unsigned t = 0; t < TRACKS && !m; t++) {
                if (!tracks[t].used)
                    free_t = free_t ? free_t : &tracks[t];
                else if (tracks[t].rule == r && lo < tracks[t].hi && hi > tracks[t].lo)
                    m = &tracks[t];
            }
            if (!m) {
                if (!free_t)
                    continue;          /* too many at once; the strongest are already tracked */
                m = free_t;
                *m = (struct track){ .used = true, .rule = r, .first_ms = now, .total = 1, .event = -1,
                                     .lo = lo, .hi = hi, .peak = -INFINITY, .excess = -INFINITY };
            }
            if (!m->hit)
                m->seen++;
            m->hit = true;
            m->last_ms = now;
            if (exc >= m->excess) {    /* keep the range seen at its strongest */
                m->excess = exc;
                m->lo = lo;
                m->hi = hi;
            }
            m->peak = fmaxf(m->peak, peak);
        }
    }

    /* Open, update and close events. */
    for (unsigned t = 0; t < TRACKS; t++) {
        struct track *m = &tracks[t];
        if (!m->used)
            continue;
        const struct detector_rule *u = &c.rule[m->rule];
        if (now - m->last_ms > c.end_s * 1000 || !u->on) {
            if (m->event >= 0) {
                xSemaphoreTake(events_lock, portMAX_DELAY);
                struct detector_event *e = &events[m->event];
                e->active = false;
                struct detector_event copy = *e;
                xSemaphoreGive(events_lock);
                report(REPORT_END, &copy);
            }
            m->used = false;
            continue;
        }
        if (m->event < 0) {
            if (now - m->first_ms >= u->min_s * 1000 && m->seen >= c.duty * m->total) {
                m->event = open_event(m, now);
                m->reported_ms = now;
            } else if (now - m->first_ms > 10 * u->min_s * 1000) {
                /* Not steady enough: look at a fresh window. */
                m->first_ms = now;
                m->seen = m->total = 0;
            }
            continue;
        }
        xSemaphoreTake(events_lock, portMAX_DELAY);
        struct detector_event *e = &events[m->event];
        e->last_ms = m->last_ms;
        e->peak_dbfs = fmaxf(e->peak_dbfs, m->peak);
        if (m->excess > e->excess_db) {
            e->excess_db = m->excess;
            e->lo_mhz = m->lo;
            e->hi_mhz = m->hi;
        }
        struct detector_event copy = *e;
        xSemaphoreGive(events_lock);
        if (now - m->reported_ms >= UPDATE_EVERY_MS) {
            m->reported_ms = now;
            report(REPORT_UPDATE, &copy);
        }
    }
}

/* ---- reporting ------------------------------------------------------------------------ */

void detector_add_sink(detector_sink sink)
{
    for (unsigned i = 0; i < sizeof(sinks) / sizeof(sinks[0]); i++)
        if (!sinks[i]) {
            sinks[i] = sink;
            return;
        }
}

static void utc(double board_ms, char *buf, size_t len)
{
    double boot = wallclock_boot_epoch_ms();
    if (!boot) {
        buf[0] = 0;
        return;
    }
    double ms = boot + board_ms;
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)fmod(ms, 1000));
}

static void reporter(void *arg)
{
    static const char *const kinds[] = { "start", "update", "end" };
    for (;;) {
        struct report r;
        if (xQueueReceive(reports, &r, portMAX_DELAY) != pdTRUE)
            continue;
        const struct detector_event *e = &r.e;
        ESP_LOGI(TAG, "event %lu %s: %.1f-%.1f MHz, peak %.1f dBFS, %.1f dB over the floor, %.1f s",
                 (unsigned long)e->id, kinds[r.kind], e->lo_mhz, e->hi_mhz, e->peak_dbfs, e->excess_db,
                 (e->last_ms - e->start_ms) / 1000);
        struct storage_status st;
        storage_status(&st);
        if (st.mounted) {
            char start[64], last[64];
            utc(e->start_ms, start, sizeof(start));
            utc(e->last_ms, last, sizeof(last));
            FILE *f = fopen(LOG_PATH, "a");
            if (f) {
                fprintf(f, "{\"id\":%lu,\"kind\":\"%s\",\"rule\":\"%s\",\"start\":\"%s\",\"last\":\"%s\","
                           "\"duration_s\":%.1f,\"lo_mhz\":%.2f,\"hi_mhz\":%.2f,\"peak_dbfs\":%.1f,\"excess_db\":%.1f}\n",
                        (unsigned long)e->id, kinds[r.kind], detector_rule_name(e->rule), start, last,
                        (e->last_ms - e->start_ms) / 1000, e->lo_mhz, e->hi_mhz, e->peak_dbfs, e->excess_db);
                fclose(f);
            }
        }
        for (unsigned i = 0; i < sizeof(sinks) / sizeof(sinks[0]); i++)
            if (sinks[i])
                sinks[i](r.kind, e);
    }
}

void detector_start(void)
{
    config_lock = xSemaphoreCreateMutex();
    events_lock = xSemaphoreCreateMutex();
    load_config();
    floor_db = heap_caps_malloc(GRID_CELLS * sizeof(float), MALLOC_CAP_SPIRAM);
    excess = heap_caps_malloc(GRID_CELLS * sizeof(float), MALLOC_CAP_SPIRAM);
    level = heap_caps_malloc(GRID_CELLS * sizeof(float), MALLOC_CAP_SPIRAM);
    covered = heap_caps_malloc(GRID_CELLS, MALLOC_CAP_SPIRAM);
    events = heap_caps_calloc(EVENTS, sizeof(struct detector_event), MALLOC_CAP_SPIRAM);
    reports = xQueueCreate(16, sizeof(struct report));
    if (!floor_db || !excess || !level || !covered || !events || !reports) {
        ESP_LOGE(TAG, "out of memory");
        floor_db = NULL;
        return;
    }
    for (unsigned k = 0; k < GRID_CELLS; k++)
        floor_db[k] = NAN;
    /* Stack in PSRAM: the reporter writes the card and the network, never flash. */
    xTaskCreateWithCaps(reporter, "detector", 4096, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "%s; rule 1: %s", config.on ? "on" : "off", config.rule[0].name);
}
