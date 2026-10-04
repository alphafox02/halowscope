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
 *   board -> browser
 *     hello, status, notice, pong (JSON), and spectrum frames (binary):
 *       u8 1, u8 flags (1: peak detector), u16 bins, u32 sequence,
 *       f64 start Hz, f64 stop Hz, f32 base dB, f32 step dB, u32 frames
 *       merged, u32 0, f64 time (ms), then one byte per bin:
 *       dB = base + code * step.
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
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "halowscope.h"
#include "radio.h"
#include "spectrum.h"

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
    size_t len;
    uint8_t data[];
};

static void send_work(void *arg)
{
    struct outgoing *o = arg;
    httpd_ws_frame_t f = { .final = true, .type = o->type, .payload = o->data, .len = o->len };
    if (o->fd == client_fd || o->type == HTTPD_WS_TYPE_CLOSE)
        httpd_ws_send_frame_async(server, o->fd, &f);
    if (o->type == HTTPD_WS_TYPE_CLOSE)
        httpd_sess_trigger_close(server, o->fd);
    free(o);
}

static void queue_frame(int fd, httpd_ws_type_t type, const void *data, size_t len)
{
    struct outgoing *o = malloc(sizeof(*o) + len);
    if (!o)
        return;
    o->fd = fd;
    o->type = type;
    o->len = len;
    memcpy(o->data, data, len);
    if (httpd_queue_work(server, send_work, o) != ESP_OK)
        free(o);
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
    double in_bin = (stop_hz - start_hz) / bins, out_bin = (b - a) / out;

    bool same = pending.ready && pending.bins == out && pending.start == a && pending.stop == b &&
                pending.peak == peak;
    for (unsigned j = 0; j < out; j++) {
        double x0 = (a + j * out_bin - start_hz) / in_bin, x1 = x0 + out_bin / in_bin;
        float v;
        if (x1 - x0 >= 1.0) {
            int i0 = (int)floor(x0), i1 = (int)ceil(x1);
            if (i0 < 0)
                i0 = 0;
            if (i1 > (int)bins)
                i1 = bins;
            v = -INFINITY;
            for (int i = i0; i < i1; i++)
                v = fmaxf(v, db[i]);
        } else {
            double x = (x0 + x1) / 2 - 0.5;
            if (x < 0)
                x = 0;
            if (x > bins - 1)
                x = bins - 1;
            unsigned i = (unsigned)x;
            float f = (float)(x - i);
            v = i + 1 < bins ? db[i] * (1 - f) + db[i + 1] * f : db[i];
        }
        pending.db[j] = same ? fmaxf(pending.db[j], v) : v;
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

    cJSON *sw = cJSON_AddObjectToObject(j, "sweep");
    cJSON_AddBoolToObject(sw, "on", s.sweep.on);
    cJSON_AddNumberToObject(sw, "start", s.sweep.start_hz);
    cJSON_AddNumberToObject(sw, "stop", s.sweep.stop_hz);

    cJSON *d = cJSON_AddObjectToObject(j, "device");
    cJSON_AddStringToObject(d, "ip", ip);
    cJSON_AddNumberToObject(d, "rssi", hs_link_rssi());
    cJSON_AddNumberToObject(d, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(d, "uptime", esp_timer_get_time() / 1e6);
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
    xTaskCreate(status_task, "wsstatus", 4096, NULL, 2, NULL);
}
