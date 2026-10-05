/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Outputs for detector events, from one task with its stack in PSRAM:
 *
 *   MQTT  a minimal MQTT 3.1.1 client, publish only, QoS 0, no TLS: one JSON
 *         message per event start, update (every 10 s) and end, to
 *         <topic>/event. Reconnects with a growing pause, up to a minute.
 *   CoT   Cursor on Target over UDP, to TAK's usual multicast group
 *         (239.2.3.1:6969) or to one receiver's address:
 *         the node itself once a minute, and each event as a marker at the
 *         node's position, refreshed while it lasts and made stale when it
 *         ends. Needs the node's position and the wall-clock time.
 *
 * Settings are kept on the board; the MQTT password is never sent back out.
 */

#include "outputs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "detector.h"
#include "settings.h"
#include "wallclock.h"

static const char *TAG = "outputs";

#define NVS_NAMESPACE "halowscope"
#define NVS_KEY "outputs"
#define CONFIG_VERSION 1
#define PRESENCE_US (60LL * 1000000)
#define MQTT_KEEPALIVE_S 60
#define MQTT_PING_US (30LL * 1000000)
#define EVENT_STALE_S 30          /* a marker not refreshed this long fades */

struct message {
    enum detector_report kind;
    struct detector_event e;
};

static struct outputs_config config;
static SemaphoreHandle_t lock;
static QueueHandle_t queue;
static struct outputs_status state = { "off", "off", 0, 0 };
static char node_uid[32];

static const struct outputs_config defaults = {
    .has_position = false, .callsign = "HaLowScope",
    .mqtt_on = false, .mqtt_port = 1883, .mqtt_topic = "halowscope",
    .cot_on = false, .cot_group = "239.2.3.1", .cot_port = 6969,
};

/* ---- configuration --------------------------------------------------------------- */

static bool printable(const char *s, size_t size, bool allow_empty)
{
    size_t n = strnlen(s, size);
    if (n == size || (!allow_empty && !n))
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isprint((unsigned char)s[i]))
            return false;
    return true;
}

static bool valid(const struct outputs_config *c)
{
    if (c->has_position && !(c->lat >= -90 && c->lat <= 90 && c->lon >= -180 && c->lon <= 180 &&
                             c->alt_m >= -500 && c->alt_m <= 9000))
        return false;
    if (!printable(c->callsign, sizeof(c->callsign), false) || !printable(c->mqtt_host, sizeof(c->mqtt_host), true) ||
        !printable(c->mqtt_topic, sizeof(c->mqtt_topic), false) || !printable(c->mqtt_user, sizeof(c->mqtt_user), true) ||
        !printable(c->mqtt_pass, sizeof(c->mqtt_pass), true) || strpbrk(c->mqtt_topic, "+#"))
        return false;
    if (c->mqtt_on && (!c->mqtt_host[0] || !c->mqtt_port))
        return false;
    /* A multicast group (TAK's usual), or one receiver such as a TAK server. */
    struct in_addr to;
    if (!inet_aton(c->cot_group, &to) || !to.s_addr || !c->cot_port)
        return false;
    return true;
}

static void save(void)
{
    struct { uint32_t version; struct outputs_config c; } saved = { CONFIG_VERSION, config };
    settings_save(NVS_KEY, &saved, sizeof(saved));
}

static void load(void)
{
    struct { uint32_t version; struct outputs_config c; } saved;
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

void outputs_get_config(struct outputs_config *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = config;
    xSemaphoreGive(lock);
}

bool outputs_set_config(const struct outputs_config *c)
{
    struct outputs_config next = *c;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!next.mqtt_pass[0])
        memcpy(next.mqtt_pass, config.mqtt_pass, sizeof(next.mqtt_pass));
    bool ok = valid(&next);
    if (ok) {
        config = next;
        save();
    }
    xSemaphoreGive(lock);
    return ok;
}

void outputs_status(struct outputs_status *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = state;
    xSemaphoreGive(lock);
}

static void set_state(const char *mqtt, const char *cot)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (mqtt)
        state.mqtt = mqtt;
    if (cot)
        state.cot = cot;
    xSemaphoreGive(lock);
}

/* ---- MQTT ------------------------------------------------------------------------------ */

static size_t put_length(uint8_t *p, size_t n)
{
    size_t i = 0;
    do {
        uint8_t b = n % 128;
        n /= 128;
        p[i++] = b | (n ? 0x80 : 0);
    } while (n);
    return i;
}

static size_t put_string(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    p[0] = n >> 8;
    p[1] = n & 255;
    memcpy(p + 2, s, n);
    return n + 2;
}

static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int w = send(fd, p, n, 0);
        if (w <= 0)
            return false;
        p += w;
        n -= w;
    }
    return true;
}

static int mqtt_connect(const struct outputs_config *c)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", c->mqtt_port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(c->mqtt_host, port, &hints, &res) != 0 || !res) {
        set_state("cannot resolve the broker", NULL);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        if (fd >= 0)
            close(fd);
        set_state("cannot reach the broker", NULL);
        return -1;
    }
    freeaddrinfo(res);

    uint8_t body[200], packet[210];
    size_t n = 0;
    n += put_string(body + n, "MQTT");
    body[n++] = 4;                                  /* protocol level 3.1.1 */
    uint8_t flags = 0x02;                           /* clean session */
    if (c->mqtt_user[0])
        flags |= 0x80;
    if (c->mqtt_user[0] && c->mqtt_pass[0])
        flags |= 0x40;
    body[n++] = flags;
    body[n++] = 0;
    body[n++] = MQTT_KEEPALIVE_S;
    n += put_string(body + n, node_uid);
    if (flags & 0x80)
        n += put_string(body + n, c->mqtt_user);
    if (flags & 0x40)
        n += put_string(body + n, c->mqtt_pass);
    size_t m = 0;
    packet[m++] = 0x10;
    m += put_length(packet + m, n);
    memcpy(packet + m, body, n);
    uint8_t ack[4];
    if (!send_all(fd, packet, m + n) || recv(fd, ack, 4, MSG_WAITALL) != 4 || ack[0] != 0x20 || ack[3] != 0) {
        close(fd);
        set_state("broker refused the connection", NULL);
        return -1;
    }
    set_state("connected", NULL);
    ESP_LOGI(TAG, "MQTT connected to %s:%u", c->mqtt_host, c->mqtt_port);
    return fd;
}

static bool mqtt_publish(int fd, const char *topic, const char *payload)
{
    size_t tl = strlen(topic), pl = strlen(payload), n = 2 + tl + pl, m = 0;
    uint8_t head[8];
    head[m++] = 0x30;
    m += put_length(head + m, n);
    uint8_t tlen[2] = { tl >> 8, tl & 255 };
    return send_all(fd, head, m) && send_all(fd, tlen, 2) && send_all(fd, (const uint8_t *)topic, tl) &&
           send_all(fd, (const uint8_t *)payload, pl);
}

/* ---- CoT -------------------------------------------------------------------------------- */

static void iso(double epoch_ms, char *buf, size_t len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)((long long)epoch_ms % 1000));
}

static bool cot_send(int fd, const struct outputs_config *c, const char *uid, const char *type,
                     const char *callsign, const char *remarks, double stale_s)
{
    double now = wallclock_boot_epoch_ms() + esp_timer_get_time() / 1000.0;
    char t[64], stale[64], xml[1000];
    iso(now, t, sizeof(t));
    iso(now + stale_s * 1000, stale, sizeof(stale));
    int n = snprintf(xml, sizeof(xml),
                     "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                     "<event version=\"2.0\" uid=\"%s\" type=\"%s\" how=\"h-e\" time=\"%s\" start=\"%s\" stale=\"%s\">"
                     "<point lat=\"%.7f\" lon=\"%.7f\" hae=\"%.1f\" ce=\"50.0\" le=\"50.0\"/>"
                     "<detail><contact callsign=\"%s\"/><remarks>%s</remarks></detail></event>",
                     uid, type, t, t, stale, c->lat, c->lon, c->alt_m, callsign, remarks);
    if (n <= 0 || n >= (int)sizeof(xml))
        return false;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(c->cot_port) };
    inet_aton(c->cot_group, &to.sin_addr);
    return sendto(fd, xml, n, 0, (struct sockaddr *)&to, sizeof(to)) == n;
}

/* Callsigns and remarks go into XML: keep them to safe characters. */
static void xml_safe(char *s)
{
    for (; *s; s++)
        if (*s == '<' || *s == '>' || *s == '&' || *s == '"' || *s == '\'')
            *s = ' ';
}

/* ---- the task ------------------------------------------------------------------------------ */

static void on_event(enum detector_report kind, const struct detector_event *e)
{
    struct message m = { kind, *e };
    if (queue)
        xQueueSend(queue, &m, 0);
}

static void task(void *arg)
{
    static const char *const kinds[] = { "start", "update", "end" };
    int mqtt = -1, cot = socket(AF_INET, SOCK_DGRAM, 0);
    int64_t next_try = 0, backoff = 2000000, last_ping = 0, last_presence = -PRESENCE_US;
    char mqtt_target[128] = "";

    for (;;) {
        struct message msg;
        bool got = xQueueReceive(queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE;
        struct outputs_config c;
        outputs_get_config(&c);
        xml_safe(c.callsign);
        int64_t now = esp_timer_get_time();
        bool clock = wallclock_source() != CLOCK_NONE;

        /* MQTT: (re)connect, keep alive, drop when switched off or moved. */
        char target[128];
        snprintf(target, sizeof(target), "%s:%u/%s", c.mqtt_host, c.mqtt_port, c.mqtt_user);
        if (mqtt >= 0 && (!c.mqtt_on || strcmp(target, mqtt_target) != 0)) {
            close(mqtt);
            mqtt = -1;
            next_try = 0;
        }
        if (!c.mqtt_on) {
            set_state("off", NULL);
        } else if (mqtt < 0 && now >= next_try) {
            set_state("connecting", NULL);
            mqtt = mqtt_connect(&c);
            strlcpy(mqtt_target, target, sizeof(mqtt_target));
            if (mqtt < 0) {
                next_try = now + backoff;
                backoff = backoff * 2 > 60000000 ? 60000000 : backoff * 2;
            } else {
                backoff = 2000000;
                last_ping = now;
            }
        }
        if (mqtt >= 0) {
            uint8_t sink[16];
            while (recv(mqtt, sink, sizeof(sink), MSG_DONTWAIT) > 0)
                ;                                   /* PINGRESP and the like */
            if (now - last_ping > MQTT_PING_US) {
                static const uint8_t ping[2] = { 0xC0, 0x00 };
                last_ping = now;
                if (!send_all(mqtt, ping, 2)) {
                    close(mqtt);
                    mqtt = -1;
                    set_state("connection lost", NULL);
                }
            }
        }

        /* CoT needs a position and the time. */
        if (!c.cot_on)
            set_state(NULL, "off");
        else if (!c.has_position)
            set_state(NULL, "needs the node's position");
        else if (!clock)
            set_state(NULL, "waiting for the clock");
        else
            set_state(NULL, "on");
        bool cot_ok = c.cot_on && c.has_position && clock && cot >= 0;
        if (cot_ok && now - last_presence > PRESENCE_US) {
            last_presence = now;
            if (cot_send(cot, &c, node_uid, "a-f-G-E-S", c.callsign, "HaLowScope 2.4 GHz spectrum sensor",
                         PRESENCE_US / 1000000 * 3)) {
                xSemaphoreTake(lock, portMAX_DELAY);
                state.cot_sent++;
                xSemaphoreGive(lock);
            }
        }

        if (!got)
            continue;
        const struct detector_event *e = &msg.e;
        double boot = wallclock_boot_epoch_ms();
        char start[64] = "", last[64] = "";
        if (clock) {
            iso(boot + e->start_ms, start, sizeof(start));
            iso(boot + e->last_ms, last, sizeof(last));
        }
        char rule[24];
        strlcpy(rule, detector_rule_name(e->rule), sizeof(rule));
        xml_safe(rule);
        if (mqtt >= 0) {
            char json[480], topic[80];
            snprintf(json, sizeof(json),
                     "{\"node\":\"%s\",\"id\":%lu,\"kind\":\"%s\",\"rule\":\"%s\",\"type\":\"%s\",\"start\":\"%s\","
                     "\"last\":\"%s\",\"duration_s\":%.1f,\"lo_mhz\":%.2f,\"hi_mhz\":%.2f,\"peak_dbfs\":%.1f,"
                     "\"excess_db\":%.1f}",
                     c.callsign, (unsigned long)e->id, kinds[msg.kind], rule, detector_type_name(e->type), start, last,
                     (e->last_ms - e->start_ms) / 1000, e->lo_mhz, e->hi_mhz, e->peak_dbfs, e->excess_db);
            snprintf(topic, sizeof(topic), "%s/event", c.mqtt_topic);
            if (mqtt_publish(mqtt, topic, json)) {
                xSemaphoreTake(lock, portMAX_DELAY);
                state.mqtt_sent++;
                xSemaphoreGive(lock);
            } else {
                close(mqtt);
                mqtt = -1;
                set_state("connection lost", NULL);
            }
        }
        if (cot_ok) {
            char uid[96], callsign[96], remarks[260];
            snprintf(uid, sizeof(uid), "%s-event-%lu", node_uid, (unsigned long)e->id);
            snprintf(callsign, sizeof(callsign), "%s RF %.0f-%.0f MHz", c.callsign, e->lo_mhz, e->hi_mhz);
            snprintf(remarks, sizeof(remarks), "%s, %s: %.1f-%.1f MHz, %.0f dB above usual, peak %.0f dBFS, %.0f s (%s)",
                     detector_type_name(e->type), rule, e->lo_mhz, e->hi_mhz, e->excess_db, e->peak_dbfs,
                     (e->last_ms - e->start_ms) / 1000, msg.kind == REPORT_END ? "ended" : "ongoing");
            /* An ended event goes stale at once, so TAK clears it. */
            if (cot_send(cot, &c, uid, "a-u-G", callsign, remarks, msg.kind == REPORT_END ? 0 : EVENT_STALE_S)) {
                xSemaphoreTake(lock, portMAX_DELAY);
                state.cot_sent++;
                xSemaphoreGive(lock);
            }
        }
    }
}

void outputs_start(void)
{
    lock = xSemaphoreCreateMutex();
    load();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(node_uid, sizeof(node_uid), "halowscope-%02x%02x%02x", mac[3], mac[4], mac[5]);
    queue = xQueueCreate(16, sizeof(struct message));
    detector_add_sink(on_event);
    /* Stack in PSRAM: network and NVS reads only; saves go through settings.c. */
    xTaskCreateWithCaps(task, "outputs", 6144, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
