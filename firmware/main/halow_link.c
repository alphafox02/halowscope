/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * HaLow station: credentials in NVS and the link to the access point.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "mmhalow.h"
#include "halowscope.h"
#include "wallclock.h"

static const char *TAG = "link";

/*
 * The stock firmware keeps a 180-byte blob in NVS "wifi_data"/"sta_cfg":
 * ssid[32] at 0, u32 ssid_len at 32, passphrase[64] at 36, u32 pass_len at
 * 100. Reading and writing the same layout keeps the credentials usable by
 * both firmwares.
 */
#define NVS_NS        "wifi_data"
#define NVS_KEY       "sta_cfg"
#define BLOB_LEN      180
#define OFF_SSID      0
#define OFF_SSID_LEN  32
#define OFF_PASS      36
#define OFF_PASS_LEN  100

static volatile bool link_up;
static volatile enum mmwlan_sta_state sta;
static esp_ip4_addr_t ip4;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

esp_err_t hs_creds_load(struct hs_creds *c)
{
    uint8_t blob[BLOB_LEN];
    size_t len = sizeof(blob);
    nvs_handle_t h;

    memset(c, 0, sizeof(*c));
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_get_blob(h, NVS_KEY, blob, &len);
    nvs_close(h);
    if (err != ESP_OK)
        return err;
    if (len < OFF_PASS_LEN + 4)
        return ESP_ERR_INVALID_SIZE;

    uint32_t sl = rd32(blob + OFF_SSID_LEN), pl = rd32(blob + OFF_PASS_LEN);
    if (sl == 0 || sl > HS_SSID_MAX || pl > HS_PASS_MAX)
        return ESP_ERR_INVALID_STATE;
    memcpy(c->ssid, blob + OFF_SSID, sl);
    memcpy(c->pass, blob + OFF_PASS, pl);
    return ESP_OK;
}

esp_err_t hs_creds_save(const struct hs_creds *c)
{
    uint8_t blob[BLOB_LEN];
    size_t len = sizeof(blob);
    nvs_handle_t h;
    size_t sl = strlen(c->ssid), pl = strlen(c->pass);

    if (sl == 0 || sl > HS_SSID_MAX || pl > HS_PASS_MAX)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    /* Keep whatever else the stock firmware stored in the blob. */
    if (nvs_get_blob(h, NVS_KEY, blob, &len) != ESP_OK || len != BLOB_LEN)
        memset(blob, 0, sizeof(blob));
    memset(blob + OFF_SSID, 0, HS_SSID_MAX);
    memset(blob + OFF_PASS, 0, HS_PASS_MAX);
    memcpy(blob + OFF_SSID, c->ssid, sl);
    memcpy(blob + OFF_PASS, c->pass, pl);
    wr32(blob + OFF_SSID_LEN, sl);
    wr32(blob + OFF_PASS_LEN, pl);
    err = nvs_set_blob(h, NVS_KEY, blob, BLOB_LEN);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void sta_state(enum mmwlan_sta_state s)
{
    sta = s;
    switch (s) {
    case MMWLAN_STA_DISABLED:
        ESP_LOGI(TAG, "HaLow STA disabled");
        break;
    case MMWLAN_STA_CONNECTING:
        ESP_LOGI(TAG, "HaLow STA connecting");
        break;
    case MMWLAN_STA_CONNECTED:
        ESP_LOGI(TAG, "HaLow STA connected");
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ip4 = e->ip_info.ip;
        link_up = true;
        wallclock_start();
        ESP_LOGI(TAG, "IP " IPSTR ", open http://" IPSTR "/",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip));
    } else if (id == IP_EVENT_STA_LOST_IP) {
        link_up = false;
        ip4.addr = 0;
        ESP_LOGW(TAG, "IP lost");
    }
}

static void join(void)
{
    struct hs_creds c;
    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };

    if (hs_creds_load(&c) != ESP_OK) {
        ESP_LOGW(TAG, "No HaLow network stored. Send AT+CWJAP=\"ssid\",\"passphrase\"");
        return;
    }
    memcpy(conf.sta.ssid, c.ssid, strlen(c.ssid));
    conf.sta.ssid_len = strlen(c.ssid);
    if (c.pass[0]) {
        memcpy(conf.sta.passphrase, c.pass, strlen(c.pass));
        conf.sta.passphrase_len = strlen(c.pass);
        conf.sta.security_type = MMWLAN_SAE;
    } else {
        conf.sta.security_type = MMWLAN_OPEN;
    }
    mmhalow_set_config(WIFI_IF_STA, &conf);
    ESP_LOGI(TAG, "Joining \"%s\" (%s)", c.ssid, c.pass[0] ? "SAE" : "open");
    esp_err_t err = mmhalow_connect(sta_state);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "mmhalow_connect: %s", esp_err_to_name(err));
}

/* One status line every 10 s, to follow association and DHCP on serial. */
static void status_task(void *arg)
{
    static const char *const names[] = { "disabled", "connecting", "connected" };
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_INIT;
    esp_netif_ip_info_t info = { 0 };

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (nif) {
            esp_netif_dhcpc_get_status(nif, &dhcp);
            esp_netif_get_ip_info(nif, &info);
        }
        ESP_LOGI(TAG, "sta %s, rssi %ld, netif %s, dhcpc %d, ip " IPSTR,
                 sta < 3 ? names[sta] : "?", (long)mmwlan_get_rssi(),
                 nif && esp_netif_is_netif_up(nif) ? "up" : "down", (int)dhcp,
                 IP2STR(&info.ip));
    }
}

void hs_link_start(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL));
    ESP_ERROR_CHECK(mmhalow_init(NULL));
    mmhalow_print_version_info();
    join();
    xTaskCreate(status_task, "linkstat", 3072, NULL, 2, NULL);
}

void hs_link_reconnect(void)
{
    link_up = false;
    mmhalow_disconnect();
    join();
}

bool hs_link_up(void)
{
    return link_up;
}

void hs_link_ip(char *buf, size_t len)
{
    if (link_up)
        snprintf(buf, len, IPSTR, IP2STR(&ip4));
    else if (len)
        buf[0] = 0;
}

int hs_link_rssi(void)
{
    return mmwlan_get_rssi();
}
