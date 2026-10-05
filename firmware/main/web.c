/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * HTTP server on the HaLow interface: the page (gzipped in flash) and one
 * WebSocket at /ws.
 *
 * The WebSocket speaks the protocol of eSpDR's `iqstream serve` page
 * (https://github.com/h0m3us3r/eSpDR, 0BSD), whose frontend this firmware
 * serves in adapted form:
 *
 *   browser -> board (JSON text)
 *     view   {start, stop, bins, fps}  range and resolution on screen
 *     set    {v: "name=value"}         receiver setting
 *     fft    {size, window, rate, averaging, peak, dc}  (changed fields)
 *     sweep  {on, start, stop}
 *     run    {v: bool}
 *     ping   {c}                       answered by pong {c, s}
 *     ack    {seq}                     frame received
 *     clock  {ms}                      the browser's clock, used without NTP
 *     detector {config?}               sets the detector (if given); answered
 *                                      by detector {config, events}
 *     outputs {config?}                sets the outputs (if given); answered by
 *                                      outputs {config} (never the password)
 *     history  {id, top, row_ms, rows, start, stop, bins}  past rows
 *     overview {id, rows, groups, start, stop}  the whole history, averaged
 *   board -> browser
 *     hello, status, notice, pong (JSON), and spectrum frames (binary):
 *       u8 1, u8 flags (1: peak detector), u16 bins, u32 sequence,
 *       f64 start Hz, f64 stop Hz, f32 base dB, f32 step dB, u32 frames
 *       merged, u32 0, f64 time (ms), then one byte per bin:
 *       dB = base + code * step.
 *     History rows (type 2) and the overview (type 3), in chunks:
 *       u8 type, u8 flags (1: last chunk), u16 bins, u32 id, f64 start Hz,
 *       f64 stop Hz, f32 base dB, f32 step dB, u16 first row, u16 rows in
 *       this chunk, u16 rows in all, u16 0, f64 top (ms), f64 ms per row,
 *       then rows x bins bytes, newest row first: 0 for no data, otherwise
 *       dB = base + (code - 1) * step.
 *
 * One browser at a time: a new connection takes over and the old one is
 * closed with code 4001. At most two frames are in flight; frames produced
 * meanwhile are merged (maximum per bin) into the next one, so a slow
 * HaLow link shows short signals rather than falling behind.
 */

#include "web.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "halowscope.h"
#include "radio.h"
#include "spectrum.h"
#include "storage.h"
#include "history.h"
#include "wallclock.h"
#include "archive.h"
#include "detector.h"
#include "outputs.h"

static const char *TAG = "web";

#define MAX_BINS 4096u
#define MIN_BINS 16u
#define MAX_IN_FLIGHT 2u
#define STALL_US 3000000
#define FRAME_HEADER 48u
#define DB_STEP 0.5f

static httpd_handle_t server;
static SemaphoreHandle_t lock;
static int client_fd = -1;

static struct {
    double start, stop;  /* 0, 0: whole span */
    unsigned bins;
    unsigned fps;        /* 0: unlimited */
} view;

/* Frame waiting for the link, at view resolution. */
static struct {
    bool ready, peak;
    double start, stop;
    unsigned bins, merged;
    float *db;
} pending;
static unsigned in_flight;
static uint32_t sequence;
static int64_t last_send_us;

static double now_ms(void) { return esp_timer_get_time() / 1000.0; }

/* ---- sending --------------------------------------------------------------- */

struct outgoing {
    int fd;
    httpd_ws_type_t type;
    SemaphoreHandle_t sent;   /* given once sent, if set */
    size_t len;
    uint8_t data[];
};

static void send_work(void *arg)
{
    struct outgoing *o = arg;
    httpd_ws_frame_t f = { .final = true, .type = o->type, .payload = o->data, .len = o->len };
    bool failed = false;
    if (o->fd == client_fd || o->type == HTTPD_WS_TYPE_CLOSE)
        failed = httpd_ws_send_frame_async(server, o->fd, &f) != ESP_OK;
    /* A send that failed part way leaves the stream out of step: close it, and
     * the page reconnects, rather than going on with garbage. */
    if (o->type == HTTPD_WS_TYPE_CLOSE || failed) {
        if (failed)
            ESP_LOGW(TAG, "send failed, closing the connection");
        httpd_sess_trigger_close(server, o->fd);
    }
    if (o->sent)
        xSemaphoreGive(o->sent);
    free(o);
}

static bool queue_frame_signal(int fd, httpd_ws_type_t type, const void *data, size_t len, SemaphoreHandle_t sent)
{
    struct outgoing *o = malloc(sizeof(*o) + len);
    if (!o)
        return false;
    o->fd = fd;
    o->type = type;
    o->sent = sent;
    o->len = len;
    memcpy(o->data, data, len);
    if (httpd_queue_work(server, send_work, o) != ESP_OK) {
        free(o);
        return false;
    }
    return true;
}

static void queue_frame(int fd, httpd_ws_type_t type, const void *data, size_t len)
{
    queue_frame_signal(fd, type, data, len, NULL);
}

static void send_text(int fd, const char *text)
{
    if (fd >= 0)
        queue_frame(fd, HTTPD_WS_TYPE_TEXT, text, strlen(text));
}

static void send_json(int fd, cJSON *j)
{
    char *text = cJSON_PrintUnformatted(j);
    if (text) {
        send_text(fd, text);
        free(text);
    }
    cJSON_Delete(j);
}

void web_notice(const char *text)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "notice");
    cJSON_AddStringToObject(j, "text", text);
    send_json(client_fd, j);
}

/* Quantizes the pending frame and queues it. Called with the lock held. */
static void try_send(void)
{
    int64_t now = esp_timer_get_time();
    if (in_flight && now - last_send_us > STALL_US)
        in_flight = 0;  /* acknowledgements lost: start over */
    if (!pending.ready || client_fd < 0 || in_flight >= MAX_IN_FLIGHT)
        return;
    if (view.fps && now - last_send_us < 1000000 / view.fps)
        return;

    unsigned bins = pending.bins;
    float low = INFINITY, high = -INFINITY;
    for (unsigned i = 0; i < bins; i++) {
        low = fminf(low, pending.db[i]);
        high = fmaxf(high, pending.db[i]);
    }
    float base = fmaxf(low, high - 255 * DB_STEP);
    uint8_t *frame = malloc(FRAME_HEADER + bins);
    if (!frame)
        return;
    uint32_t seq = ++sequence;
    uint32_t zero = 0;
    float step = DB_STEP;
    double t = now_ms();
    frame[0] = 1;
    frame[1] = pending.peak ? 1 : 0;
    frame[2] = bins & 255;
    frame[3] = bins >> 8;
    memcpy(frame + 4, &seq, 4);
    memcpy(frame + 8, &pending.start, 8);
    memcpy(frame + 16, &pending.stop, 8);
    memcpy(frame + 24, &base, 4);
    memcpy(frame + 28, &step, 4);
    memcpy(frame + 32, &pending.merged, 4);
    memcpy(frame + 36, &zero, 4);
    memcpy(frame + 40, &t, 8);
    for (unsigned i = 0; i < bins; i++) {
        float code = roundf((pending.db[i] - base) / DB_STEP);
        frame[FRAME_HEADER + i] = code < 0 ? 0 : code > 255 ? 255 : (uint8_t)code;
    }
    queue_frame(client_fd, HTTPD_WS_TYPE_BINARY, frame, FRAME_HEADER + bins);
    free(frame);
    pending.ready = false;
    in_flight++;
    last_send_us = now;
}

/*
 * Resamples a frame onto the view: each output bin takes the maximum of the
 * input bins under it, or interpolates when the view is finer than the input.
 */
void web_publish(const float *db, unsigned bins, double start_hz, double stop_hz, bool peak)
{
    if (client_fd < 0 || !bins)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    double a = view.start, b = view.stop;
    if (b <= a || b <= start_hz || a >= stop_hz) {
        a = start_hz;
        b = stop_hz;
    }
    a = fmax(a, start_hz);
    b = fmin(b, stop_hz);
    unsigned out = view.bins ? view.bins : (bins < MAX_BINS ? bins : MAX_BINS);
    /* Positions in input bins, worked out in single precision from offsets
     * taken once in double: the S3 has no double-precision FPU. */
    float in_per_out = (float)((b - a) / out / ((stop_hz - start_hz) / bins));
    float first = (float)((a - start_hz) / ((stop_hz - start_hz) / bins));

    bool same = pending.ready && pending.bins == out && pending.start == a && pending.stop == b &&
                pending.peak == peak;
    for (unsigned j = 0; j < out; j++) {
        float x0 = first + j * in_per_out, x1 = x0 + in_per_out;
        float v;
        if (in_per_out >= 1.0f) {
            int i0 = (int)x0, i1 = (int)x1;
            if (x1 > (float)i1)
                i1++;
            if (i0 < 0)
                i0 = 0;
            if (i1 > (int)bins)
                i1 = bins;
            v = db[i0 < (int)bins ? i0 : bins - 1];
            for (int i = i0 + 1; i < i1; i++)
                if (db[i] > v)
                    v = db[i];
        } else {
            float x = (x0 + x1) * 0.5f - 0.5f;
            if (x < 0)
                x = 0;
            if (x > bins - 1)
                x = bins - 1;
            unsigned i = (unsigned)x;
            float f = x - i;
            v = i + 1 < bins ? db[i] * (1 - f) + db[i + 1] * f : db[i];
        }
        pending.db[j] = same && pending.db[j] > v ? pending.db[j] : v;
    }
    pending.merged = same ? pending.merged + 1 : 1;
    pending.ready = true;
    pending.peak = peak;
    pending.bins = out;
    pending.start = a;
    pending.stop = b;
    try_send();
    xSemaphoreGive(lock);
}

/* ---- status ------------------------------------------------------------------- */

static cJSON *int_array(const int *v, unsigned n)
{
    return cJSON_CreateIntArray(v, (int)n);
}

static void send_status(int fd)
{
    struct spectrum_status s;
    struct radio_state r;
    const struct radio_settings *rs = radio_settings();
    char ip[16];

    spectrum_status(&s);
    radio_state(&r);
    hs_link_ip(ip, sizeof(ip));

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "status");
    cJSON_AddBoolToObject(j, "running", s.running);
    const char *state = r.status != RADIO_OK ? "error" : !s.running ? "stopped" : s.sweep.on ? "sweeping" : "running";
    cJSON_AddStringToObject(j, "state", state);
    static const char *const radio_text[] = {"", "PHY calibration failed", "PLL did not lock",
                                             "receiver configuration failed", "radio not started"};
    cJSON_AddStringToObject(j, "message", r.status < 5 ? radio_text[r.status] : "");

    cJSON *rx = cJSON_AddObjectToObject(j, "receiver");
    double rate = rs->rate == RADIO_RATE_16M ? 16e6 : 80e6;
    if (s.sweep.on) {
        cJSON_AddNumberToObject(rx, "lo", (s.sweep.start_hz + (double)s.sweep.stop_hz) / 2);
        cJSON_AddNumberToObject(rx, "sample_rate", (double)s.sweep.stop_hz - s.sweep.start_hz);
    } else {
        cJSON_AddNumberToObject(rx, "lo", r.lo_hz);
        cJSON_AddNumberToObject(rx, "sample_rate", rate);
    }
    cJSON_AddNumberToObject(rx, "exact_lo", r.lo_hz);
    cJSON_AddNumberToObject(rx, "rate", rs->rate == RADIO_RATE_16M ? 16 : 80);
    cJSON_AddNumberToObject(rx, "width", rs->width);
    int filter[2] = {rs->filter & 63, (rs->filter >> 8) & 63};
    cJSON_AddItemToObject(rx, "filter", int_array(filter, 2));
    cJSON_AddNumberToObject(rx, "gain", rs->gain);
    cJSON_AddNumberToObject(rx, "rf", r.rf_gain);
    cJSON_AddNumberToObject(rx, "bb", r.bb_gain);
    cJSON_AddNumberToObject(rx, "auto", r.automatic);
    int dc[4] = {r.dc[0], r.dc[1], r.dc[2], r.dc[3]};
    cJSON_AddItemToObject(rx, "dc", int_array(dc, 4));
    int iq[2] = {(int)(r.iq_amplitude ^ 16) - 16, (int)(r.iq_phase ^ 32) - 32};
    cJSON_AddItemToObject(rx, "iq", int_array(iq, 2));
    int pll[3] = {r.pll_cap, r.pll_first, r.pll_length};
    cJSON_AddItemToObject(rx, "pll", int_array(pll, 3));

    cJSON *f = cJSON_AddObjectToObject(j, "fft");
    cJSON_AddNumberToObject(f, "size", s.fft.size);
    cJSON_AddStringToObject(f, "window", spectrum_window_names[s.fft.window]);
    cJSON_AddNumberToObject(f, "rate", s.fft.rate);
    cJSON_AddNumberToObject(f, "averaging", s.fft.averaging);
    cJSON_AddBoolToObject(f, "peak", s.fft.peak);
    cJSON_AddBoolToObject(f, "dc", s.fft.dc);

    cJSON *sp = cJSON_AddObjectToObject(j, "spectrum");
    cJSON_AddNumberToObject(sp, "fps", s.fps);
    cJSON_AddNumberToObject(sp, "coverage", s.coverage);
    cJSON_AddNumberToObject(sp, "snapshot_us", s.snapshot_us);
    cJSON_AddNumberToObject(sp, "retune_us", s.retune_us);
    cJSON_AddNumberToObject(sp, "sweep_ms", s.sweep_ms);
    cJSON_AddNumberToObject(sp, "failures", s.failures);

    cJSON *pr = cJSON_AddObjectToObject(j, "profile");
    cJSON_AddNumberToObject(pr, "unpack_us", s.profile.unpack);
    cJSON_AddNumberToObject(pr, "window_us", s.profile.window);
    cJSON_AddNumberToObject(pr, "fft_us", s.profile.fft);
    cJSON_AddNumberToObject(pr, "bitrev_us", s.profile.bitrev);
    cJSON_AddNumberToObject(pr, "power_us", s.profile.power);
    cJSON_AddNumberToObject(pr, "capture_us", s.profile.capture);
    cJSON_AddNumberToObject(pr, "to_db_us", s.profile.to_db);
    cJSON_AddNumberToObject(pr, "publish_us", s.profile.publish);
    cJSON_AddNumberToObject(pr, "busy_us", s.profile.frame);
    cJSON_AddNumberToObject(pr, "ffts", s.profile.ffts);
    cJSON_AddNumberToObject(pr, "snapshots", s.profile.snapshots);

    cJSON *sw = cJSON_AddObjectToObject(j, "sweep");
    cJSON_AddBoolToObject(sw, "on", s.sweep.on);
    cJSON_AddNumberToObject(sw, "start", s.sweep.start_hz);
    cJSON_AddNumberToObject(sw, "stop", s.sweep.stop_hz);

    struct storage_status st;
    storage_status(&st);
    cJSON *sd = cJSON_AddObjectToObject(j, "card");
    cJSON_AddBoolToObject(sd, "mounted", st.mounted);
    if (st.mounted) {
        cJSON_AddStringToObject(sd, "name", st.name);
        cJSON_AddNumberToObject(sd, "size_mb", st.size_mb);
        cJSON_AddNumberToObject(sd, "total_mb", st.total_mb);
        cJSON_AddNumberToObject(sd, "free_mb", st.free_mb);
        cJSON_AddNumberToObject(sd, "clock_khz", st.clock_khz);
        cJSON_AddBoolToObject(sd, "tested", st.tested);
        cJSON_AddBoolToObject(sd, "test_ok", st.test_ok);
        cJSON_AddNumberToObject(sd, "write_kbps", st.write_kbps);
        cJSON_AddNumberToObject(sd, "read_kbps", st.read_kbps);
        cJSON_AddBoolToObject(sd, "archiving", archive_running());
        cJSON_AddNumberToObject(sd, "dropped", archive_dropped());
    }

    struct history_span hs;
    history_span(&hs);
    cJSON *hi = cJSON_AddObjectToObject(j, "history");
    cJSON_AddNumberToObject(hi, "oldest", hs.oldest_ms);
    cJSON_AddNumberToObject(hi, "newest", hs.newest_ms);
    cJSON_AddNumberToObject(hi, "lines", hs.lines);
    cJSON_AddNumberToObject(hi, "capacity", hs.capacity);
    cJSON_AddNumberToObject(hi, "line_ms", hs.line_ms);

    cJSON *ck = cJSON_AddObjectToObject(j, "clock");
    static const char *const sources[] = {"", "browser", "ntp"};
    cJSON_AddStringToObject(ck, "source", sources[wallclock_source()]);
    cJSON_AddNumberToObject(ck, "boot_epoch", wallclock_boot_epoch_ms());

    struct detector_config detc;
    detector_get_config(&detc);
    cJSON *de = cJSON_AddObjectToObject(j, "detector");
    cJSON_AddBoolToObject(de, "on", detc.on);
    cJSON_AddNumberToObject(de, "active", detector_active());

    struct outputs_status os;
    outputs_status(&os);
    cJSON *ou = cJSON_AddObjectToObject(j, "outputs");
    cJSON_AddStringToObject(ou, "mqtt", os.mqtt);
    cJSON_AddStringToObject(ou, "cot", os.cot);
    cJSON_AddNumberToObject(ou, "mqtt_sent", os.mqtt_sent);
    cJSON_AddNumberToObject(ou, "cot_sent", os.cot_sent);

    cJSON *d = cJSON_AddObjectToObject(j, "device");
    cJSON_AddStringToObject(d, "ip", ip);
    cJSON_AddNumberToObject(d, "rssi", hs_link_rssi());
    cJSON_AddNumberToObject(d, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(d, "uptime", esp_timer_get_time() / 1e6);
    static const char *const resets[] = {
        [ESP_RST_UNKNOWN] = "unknown", [ESP_RST_POWERON] = "power on", [ESP_RST_EXT] = "reset pin",
        [ESP_RST_SW] = "restart", [ESP_RST_PANIC] = "crash", [ESP_RST_INT_WDT] = "interrupt watchdog",
        [ESP_RST_TASK_WDT] = "task watchdog", [ESP_RST_WDT] = "watchdog", [ESP_RST_DEEPSLEEP] = "deep sleep",
        [ESP_RST_BROWNOUT] = "brownout", [ESP_RST_SDIO] = "SDIO",
    };
    esp_reset_reason_t why = esp_reset_reason();
    cJSON_AddStringToObject(d, "last_reset", why < sizeof(resets) / sizeof(resets[0]) && resets[why] ? resets[why] : "other");
    send_json(fd, j);
}

static void status_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int fd = client_fd;
        if (fd >= 0)
            send_status(fd);
    }
}

/* ---- receiving ------------------------------------------------------------------ */

static double number(const cJSON *j, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static void on_fft(const cJSON *j)
{
    struct spectrum_status s;
    spectrum_status(&s);
    struct fft_config c = s.fft;
    unsigned size = (unsigned)number(j, "size", c.size);
    if (size >= 512 && size <= 8192 && !(size & (size - 1)))
        c.size = size;
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(j, "window");
    if (cJSON_IsString(w))
        for (unsigned k = 0; k < WINDOW_COUNT; k++)
            if (strcmp(w->valuestring, spectrum_window_names[k]) == 0)
                c.window = k;
    double rate = number(j, "rate", c.rate);
    if (rate >= 1 && rate <= 100)
        c.rate = (unsigned)rate;
    double averaging = number(j, "averaging", c.averaging);
    if (averaging >= 0 && averaging <= 4096)
        c.averaging = (unsigned)averaging;
    const cJSON *peak = cJSON_GetObjectItemCaseSensitive(j, "peak");
    if (cJSON_IsBool(peak))
        c.peak = cJSON_IsTrue(peak);
    const cJSON *dc = cJSON_GetObjectItemCaseSensitive(j, "dc");
    if (cJSON_IsBool(dc))
        c.dc = cJSON_IsTrue(dc);
    spectrum_set_fft(&c);
}

static void on_sweep(const cJSON *j)
{
    struct spectrum_status s;
    spectrum_status(&s);
    struct sweep_config c = s.sweep;
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on");
    if (cJSON_IsBool(on))
        c.on = cJSON_IsTrue(on);
    double start = number(j, "start", c.start_hz), stop = number(j, "stop", c.stop_hz);
    /* The LO range, plus half a capture either side. */
    start = fmax(start, 2180e6);
    stop = fmin(stop, 2820e6);
    if (stop - start < 10e6) {
        web_notice("A sweep needs at least 10 MHz.");
        return;
    }
    c.start_hz = (uint32_t)start;
    c.stop_hz = (uint32_t)stop;
    spectrum_set_sweep(&c);
}

/* ---- history ---------------------------------------------------------------------- */

/* The latest request of each kind; a newer one replaces one not yet served. */
struct history_request {
    bool want, overview;
    int fd;
    uint32_t id;
    double top_ms, row_ms, start_hz, stop_hz;
    unsigned rows, bins;
};
static struct history_request wanted[2];   /* 0: rows, 1: overview */
static SemaphoreHandle_t history_lock, history_sent;
static TaskHandle_t history_task_handle;

#define HISTORY_MAX_ROWS 2048u
#define HISTORY_MAX_BINS 1024u
/* About 4 KB a message, so live frames interleave on a slow link: 4 rows of
 * 1024 bins, or the whole overview of a narrow strip in one or two. */
#define HISTORY_CHUNK_BYTES 4096u
#define HISTORY_HEADER 56u

static void on_history(int fd, const cJSON *j, bool overview)
{
    struct history_request r = { .want = true, .overview = overview, .fd = fd };
    r.id = (uint32_t)number(j, "id", 0);
    r.start_hz = number(j, "start", 0);
    r.stop_hz = number(j, "stop", 0);
    double rows = number(j, "rows", 0), bins = number(j, overview ? "groups" : "bins", 0);
    if (rows < 1 || bins < 1 || r.stop_hz <= r.start_hz)
        return;
    r.rows = rows > HISTORY_MAX_ROWS ? HISTORY_MAX_ROWS : (unsigned)rows;
    r.bins = bins > HISTORY_MAX_BINS ? HISTORY_MAX_BINS : (unsigned)bins;
    if (overview) {
        struct history_span s;
        history_span(&s);
        r.top_ms = s.newest_ms;
        r.row_ms = (s.newest_ms - s.oldest_ms + s.line_ms) / r.rows;
    } else {
        r.top_ms = number(j, "top", 0);
        r.row_ms = number(j, "row_ms", 0);
    }
    if (r.row_ms <= 0)
        return;
    xSemaphoreTake(history_lock, portMAX_DELAY);
    wanted[overview] = r;
    xSemaphoreGive(history_lock);
    xTaskNotifyGive(history_task_handle);
}

/* Builds the rows a chunk at a time and sends each chunk, one in flight. */
static void serve_history(const struct history_request *r)
{
    unsigned chunk = HISTORY_CHUNK_BYTES / r->bins;
    if (chunk < 1)
        chunk = 1;
    float *rows = heap_caps_malloc(chunk * r->bins * sizeof(float), MALLOC_CAP_SPIRAM);
    uint8_t *msg = heap_caps_malloc(HISTORY_HEADER + chunk * r->bins, MALLOC_CAP_SPIRAM);
    if (!rows || !msg)
        goto done;
    for (unsigned row0 = 0; row0 < r->rows; row0 += chunk) {
        unsigned n = r->rows - row0 < chunk ? r->rows - row0 : chunk;
        double top = r->top_ms - row0 * r->row_ms;
        history_rows(top, r->row_ms, n, r->start_hz, r->stop_hz, r->bins, r->overview, rows);
        float low = INFINITY, high = -INFINITY;
        for (unsigned i = 0; i < n * r->bins; i++) {
            if (isnan(rows[i]))
                continue;
            low = fminf(low, rows[i]);
            high = fmaxf(high, rows[i]);
        }
        float base = isinf(low) ? 0 : fmaxf(low, high - 254 * DB_STEP), step = DB_STEP;
        uint16_t first = row0, count = n, total = r->rows, zero = 0;
        uint16_t bins = r->bins;
        msg[0] = r->overview ? 3 : 2;
        msg[1] = row0 + n >= r->rows ? 1 : 0;
        memcpy(msg + 2, &bins, 2);
        memcpy(msg + 4, &r->id, 4);
        memcpy(msg + 8, &r->start_hz, 8);
        memcpy(msg + 16, &r->stop_hz, 8);
        memcpy(msg + 24, &base, 4);
        memcpy(msg + 28, &step, 4);
        memcpy(msg + 32, &first, 2);
        memcpy(msg + 34, &count, 2);
        memcpy(msg + 36, &total, 2);
        memcpy(msg + 38, &zero, 2);
        memcpy(msg + 40, &r->top_ms, 8);
        memcpy(msg + 48, &r->row_ms, 8);
        for (unsigned i = 0; i < n * r->bins; i++) {
            if (isnan(rows[i])) {
                msg[HISTORY_HEADER + i] = 0;
                continue;
            }
            float c = roundf((rows[i] - base) / DB_STEP) + 1;
            msg[HISTORY_HEADER + i] = c < 1 ? 1 : c > 255 ? 255 : (uint8_t)c;
        }
        if (r->fd != client_fd)
            break;
        xSemaphoreTake(history_sent, 0);
        if (!queue_frame_signal(r->fd, HTTPD_WS_TYPE_BINARY, msg, HISTORY_HEADER + n * r->bins, history_sent))
            break;
        xSemaphoreTake(history_sent, pdMS_TO_TICKS(5000));
        /* A newer request for the same kind wins over the rest of this one. */
        xSemaphoreTake(history_lock, portMAX_DELAY);
        bool superseded = wanted[r->overview].want;
        xSemaphoreGive(history_lock);
        if (superseded)
            break;
    }
done:
    heap_caps_free(rows);
    heap_caps_free(msg);
}

static void history_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            struct history_request r = { 0 };
            xSemaphoreTake(history_lock, portMAX_DELAY);
            /* The overview first: it is small and the rail needs it. */
            for (int k = 1; k >= 0 && !r.want; k--)
                if (wanted[k].want) {
                    r = wanted[k];
                    wanted[k].want = false;
                }
            xSemaphoreGive(history_lock);
            if (!r.want)
                break;
            serve_history(&r);
        }
    }
}

/* ---- detector ---------------------------------------------------------------------- */

static cJSON *event_json(const struct detector_event *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", e->id);
    cJSON_AddStringToObject(o, "rule", detector_rule_name(e->rule));
    cJSON_AddBoolToObject(o, "active", e->active);
    cJSON_AddNumberToObject(o, "start", e->start_ms);
    cJSON_AddNumberToObject(o, "last", e->last_ms);
    cJSON_AddNumberToObject(o, "lo", e->lo_mhz);
    cJSON_AddNumberToObject(o, "hi", e->hi_mhz);
    cJSON_AddNumberToObject(o, "peak", e->peak_dbfs);
    cJSON_AddNumberToObject(o, "excess", e->excess_db);
    cJSON_AddStringToObject(o, "type", detector_type_name(e->type));
    cJSON *f = cJSON_AddObjectToObject(o, "features");
    cJSON_AddNumberToObject(f, "center", e->f.center_mhz);
    cJSON_AddNumberToObject(f, "center_sd", e->f.center_sd_mhz);
    cJSON_AddNumberToObject(f, "width", e->f.width_mhz);
    cJSON_AddNumberToObject(f, "width_sd", e->f.width_sd_mhz);
    cJSON_AddNumberToObject(f, "duty", e->f.duty);
    cJSON_AddNumberToObject(f, "wifi_share", e->f.wifi_share);
    cJSON_AddNumberToObject(f, "samples", e->f.samples);
    return o;
}

static void send_detector(int fd)
{
    struct detector_config c;
    detector_get_config(&c);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "detector");
    cJSON *k = cJSON_AddObjectToObject(j, "config");
    cJSON_AddBoolToObject(k, "on", c.on);
    cJSON_AddNumberToObject(k, "duty", c.duty);
    cJSON_AddNumberToObject(k, "learn_min", c.learn_min);
    cJSON_AddNumberToObject(k, "end_s", c.end_s);
    cJSON_AddNumberToObject(k, "cooldown_s", c.cooldown_s);
    cJSON *rules = cJSON_AddArrayToObject(k, "rules");
    for (unsigned r = 0; r < DETECTOR_RULES; r++) {
        const struct detector_rule *u = &c.rule[r];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "on", u->on);
        cJSON_AddStringToObject(o, "name", u->name);
        cJSON_AddNumberToObject(o, "start", u->start_mhz);
        cJSON_AddNumberToObject(o, "stop", u->stop_mhz);
        cJSON_AddNumberToObject(o, "threshold", u->threshold_db);
        cJSON_AddNumberToObject(o, "min_bw", u->min_bw_mhz);
        cJSON_AddNumberToObject(o, "min_s", u->min_s);
        cJSON_AddItemToArray(rules, o);
    }
    static struct detector_event list[100];
    unsigned n = detector_events(list, 100);
    cJSON *ev = cJSON_AddArrayToObject(j, "events");
    for (unsigned i = 0; i < n; i++)
        cJSON_AddItemToArray(ev, event_json(&list[i]));
    send_json(fd, j);
}

static void on_detector(int fd, const cJSON *j)
{
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "config");
    if (cJSON_IsObject(k)) {
        struct detector_config c;
        detector_get_config(&c);
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(k, "on");
        if (cJSON_IsBool(on))
            c.on = cJSON_IsTrue(on);
        c.duty = number(k, "duty", c.duty);
        c.learn_min = number(k, "learn_min", c.learn_min);
        c.end_s = number(k, "end_s", c.end_s);
        c.cooldown_s = number(k, "cooldown_s", c.cooldown_s);
        const cJSON *rules = cJSON_GetObjectItemCaseSensitive(k, "rules");
        for (unsigned r = 0; r < DETECTOR_RULES && cJSON_IsArray(rules); r++) {
            const cJSON *o = cJSON_GetArrayItem(rules, r);
            if (!cJSON_IsObject(o))
                break;
            struct detector_rule *u = &c.rule[r];
            const cJSON *ron = cJSON_GetObjectItemCaseSensitive(o, "on");
            if (cJSON_IsBool(ron))
                u->on = cJSON_IsTrue(ron);
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
            if (cJSON_IsString(name))
                strlcpy(u->name, name->valuestring, sizeof(u->name));
            u->start_mhz = number(o, "start", u->start_mhz);
            u->stop_mhz = number(o, "stop", u->stop_mhz);
            u->threshold_db = number(o, "threshold", u->threshold_db);
            u->min_bw_mhz = number(o, "min_bw", u->min_bw_mhz);
            u->min_s = number(o, "min_s", u->min_s);
        }
        if (!detector_set_config(&c))
            web_notice("Detector settings out of range: not saved.");
    }
    send_detector(fd);
}

static void send_outputs(int fd)
{
    struct outputs_config c;
    outputs_get_config(&c);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "outputs");
    cJSON *k = cJSON_AddObjectToObject(j, "config");
    cJSON_AddBoolToObject(k, "has_position", c.has_position);
    cJSON_AddNumberToObject(k, "lat", c.lat);
    cJSON_AddNumberToObject(k, "lon", c.lon);
    cJSON_AddNumberToObject(k, "alt", c.alt_m);
    cJSON_AddStringToObject(k, "callsign", c.callsign);
    cJSON_AddBoolToObject(k, "mqtt_on", c.mqtt_on);
    cJSON_AddStringToObject(k, "mqtt_host", c.mqtt_host);
    cJSON_AddNumberToObject(k, "mqtt_port", c.mqtt_port);
    cJSON_AddStringToObject(k, "mqtt_topic", c.mqtt_topic);
    cJSON_AddStringToObject(k, "mqtt_user", c.mqtt_user);
    cJSON_AddBoolToObject(k, "mqtt_has_pass", c.mqtt_pass[0] != 0);
    cJSON_AddBoolToObject(k, "cot_on", c.cot_on);
    cJSON_AddStringToObject(k, "cot_group", c.cot_group);
    cJSON_AddNumberToObject(k, "cot_port", c.cot_port);
    send_json(fd, j);
}

static void copy_string(const cJSON *k, const char *key, char *out, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(k, key);
    if (cJSON_IsString(v))
        strlcpy(out, v->valuestring, len);
}

static void copy_bool(const cJSON *k, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(k, key);
    if (cJSON_IsBool(v))
        *out = cJSON_IsTrue(v);
}

static void on_outputs(int fd, const cJSON *j)
{
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "config");
    if (cJSON_IsObject(k)) {
        struct outputs_config c;
        outputs_get_config(&c);
        copy_bool(k, "has_position", &c.has_position);
        c.lat = number(k, "lat", c.lat);
        c.lon = number(k, "lon", c.lon);
        c.alt_m = number(k, "alt", c.alt_m);
        copy_string(k, "callsign", c.callsign, sizeof(c.callsign));
        copy_bool(k, "mqtt_on", &c.mqtt_on);
        copy_string(k, "mqtt_host", c.mqtt_host, sizeof(c.mqtt_host));
        c.mqtt_port = (uint16_t)number(k, "mqtt_port", c.mqtt_port);
        copy_string(k, "mqtt_topic", c.mqtt_topic, sizeof(c.mqtt_topic));
        copy_string(k, "mqtt_user", c.mqtt_user, sizeof(c.mqtt_user));
        c.mqtt_pass[0] = 0;   /* empty keeps the saved one */
        copy_string(k, "mqtt_pass", c.mqtt_pass, sizeof(c.mqtt_pass));
        copy_bool(k, "cot_on", &c.cot_on);
        copy_string(k, "cot_group", c.cot_group, sizeof(c.cot_group));
        c.cot_port = (uint16_t)number(k, "cot_port", c.cot_port);
        if (!outputs_set_config(&c))
            web_notice("Output settings incomplete or out of range: not saved.");
    }
    send_outputs(fd);
}

/* Event changes, from the detector's reporter task. */
static void event_to_page(enum detector_report kind, const struct detector_event *e)
{
    static const char *const kinds[] = { "start", "update", "end" };
    int fd = client_fd;
    if (fd < 0)
        return;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "event");
    cJSON_AddStringToObject(j, "kind", kinds[kind]);
    cJSON_AddItemToObject(j, "event", event_json(e));
    send_json(fd, j);
}

static void on_message(int fd, const char *text)
{
    cJSON *j = cJSON_Parse(text);
    if (!j)
        return;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "t");
    const char *type = cJSON_IsString(t) ? t->valuestring : "";

    if (strcmp(type, "ping") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "t", "pong");
        cJSON_AddNumberToObject(r, "c", number(j, "c", 0));
        cJSON_AddNumberToObject(r, "s", now_ms());
        send_json(fd, r);
    } else if (strcmp(type, "ack") == 0) {
        xSemaphoreTake(lock, portMAX_DELAY);
        if (in_flight)
            in_flight--;
        try_send();
        xSemaphoreGive(lock);
    } else if (strcmp(type, "view") == 0) {
        xSemaphoreTake(lock, portMAX_DELAY);
        view.start = number(j, "start", 0);
        view.stop = number(j, "stop", 0);
        double bins = number(j, "bins", 1024);
        view.bins = bins < MIN_BINS ? MIN_BINS : bins > MAX_BINS ? MAX_BINS : (unsigned)bins;
        double fps = number(j, "fps", 0);
        view.fps = fps > 0 && fps <= 100 ? (unsigned)fps : 0;
        xSemaphoreGive(lock);
    } else if (strcmp(type, "set") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v");
        if (cJSON_IsString(v)) {
            char setting[48];
            strlcpy(setting, v->valuestring, sizeof(setting));
            char *eq = strchr(setting, '=');
            if (eq) {
                *eq = 0;
                spectrum_set(setting, eq + 1);
            }
        }
    } else if (strcmp(type, "fft") == 0) {
        on_fft(j);
    } else if (strcmp(type, "sweep") == 0) {
        on_sweep(j);
    } else if (strcmp(type, "detector") == 0) {
        on_detector(fd, j);
    } else if (strcmp(type, "outputs") == 0) {
        on_outputs(fd, j);
    } else if (strcmp(type, "clock") == 0) {
        wallclock_offer(number(j, "ms", 0));
    } else if (strcmp(type, "history") == 0 || strcmp(type, "overview") == 0) {
        on_history(fd, j, strcmp(type, "overview") == 0);
    } else if (strcmp(type, "run") == 0) {
        spectrum_run(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "v")));
    }
    cJSON_Delete(j);
}

static void send_hello(int fd)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "hello");
    cJSON *w = cJSON_AddArrayToObject(j, "windows");
    for (unsigned k = 0; k < WINDOW_COUNT; k++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", spectrum_window_names[k]);
        cJSON_AddNumberToObject(o, "bandwidth", spectrum_window_bandwidth[k]);
        cJSON_AddItemToArray(w, o);
    }
    cJSON_AddNumberToObject(j, "max_bins", MAX_BINS);
    send_json(fd, j);
}

/* Handshake done: this browser takes over. */
static esp_err_t ws_connected(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(lock, portMAX_DELAY);
    int old = client_fd;
    client_fd = fd;
    in_flight = 0;
    pending.ready = false;
    view.start = view.stop = 0;
    view.bins = 0;
    xSemaphoreGive(lock);
    if (old >= 0 && old != fd) {
        static const uint8_t replaced[2] = {4001 >> 8, 4001 & 255};
        queue_frame(old, HTTPD_WS_TYPE_CLOSE, replaced, sizeof(replaced));
    }
    ESP_LOGI(TAG, "browser connected (socket %d)", fd);
    send_hello(fd);
    send_status(fd);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    /* The first call reads the header (len 0 asks for it), the second the
     * whole payload; frames are read completely even when ignored. */
    httpd_ws_frame_t f = { 0 };
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK || f.len > 1024)
        return ESP_FAIL;
    char *text = malloc(f.len + 1);
    if (!text)
        return ESP_ERR_NO_MEM;
    f.payload = (uint8_t *)text;
    esp_err_t err = f.len ? httpd_ws_recv_frame(req, &f, f.len) : ESP_OK;
    if (err == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT && fd == client_fd) {
        text[f.len] = 0;
        on_message(fd, text);
    }
    free(text);
    return err;
}

static void on_close(httpd_handle_t hd, int fd)
{
    if (fd == client_fd) {
        client_fd = -1;
        ESP_LOGI(TAG, "browser disconnected");
    }
    close(fd);
}

/* ---- static files ------------------------------------------------------------------ */

struct asset {
    const char *uri, *type;
    const uint8_t *start, *end;
};

#define ASSET(sym) \
    extern const uint8_t _binary_##sym##_gz_start[]; \
    extern const uint8_t _binary_##sym##_gz_end[];
ASSET(index_html)
ASSET(app_js)
ASSET(render_js)
ASSET(style_css)
ASSET(icon_svg)

static const struct asset assets[] = {
    {"/", "text/html; charset=utf-8", _binary_index_html_gz_start, _binary_index_html_gz_end},
    {"/app.js", "text/javascript", _binary_app_js_gz_start, _binary_app_js_gz_end},
    {"/render.js", "text/javascript", _binary_render_js_gz_start, _binary_render_js_gz_end},
    {"/style.css", "text/css", _binary_style_css_gz_start, _binary_style_css_gz_end},
    {"/icon.svg", "image/svg+xml", _binary_icon_svg_gz_start, _binary_icon_svg_gz_end},
};

static esp_err_t asset_handler(httpd_req_t *req)
{
    const struct asset *a = req->user_ctx;
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
}

void hs_web_start(void)
{
    lock = xSemaphoreCreateMutex();
    pending.db = heap_caps_malloc(MAX_BINS * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_close;
    /* HaLow can stall for seconds (a lost packet waits for TCP's retransmit
     * timer); wait that out rather than abandon a frame half sent. */
    cfg.send_wait_timeout = 20;
    /* Stacks of tasks that never write to the flash chip can live in PSRAM,
     * keeping internal RAM for HaLow and the network stack. */
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (!pending.db || httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        return;
    }
    for (unsigned i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {
        httpd_uri_t u = { .uri = assets[i].uri, .method = HTTP_GET, .handler = asset_handler,
                          .user_ctx = (void *)&assets[i] };
        httpd_register_uri_handler(server, &u);
    }
    httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
                       .ws_post_handshake_cb = ws_connected };
    httpd_register_uri_handler(server, &ws);
    xTaskCreateWithCaps(status_task, "wsstatus", 4096, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    history_lock = xSemaphoreCreateMutex();
    history_sent = xSemaphoreCreateBinary();
    xTaskCreateWithCaps(history_task, "history", 5120, NULL, 2, &history_task_handle,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    detector_add_sink(event_to_page);
}
