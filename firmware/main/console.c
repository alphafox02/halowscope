/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Serial commands on UART0 (115200), compatible with the stock firmware:
 *   AT+CWJAP="ssid","passphrase"   store the HaLow network and join it
 *   AT+CWJAP?                      show the stored SSID and the link state
 *   AT+RST                         restart
 *   AT+SDFORMAT=YES                erase and format the whole TF card
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "halowscope.h"
#include "storage.h"

#define UART UART_NUM_0
#define CMD_MAX 200

/* Copy one double-quoted field starting at *p into out; advance *p past it. */
static bool quoted(const char **p, char *out, size_t max)
{
    const char *s = *p;
    size_t n = 0;

    if (*s++ != '"')
        return false;
    while (*s && *s != '"') {
        if (n == max)
            return false;
        out[n++] = *s++;
    }
    if (*s != '"')
        return false;
    out[n] = 0;
    *p = s + 1;
    return true;
}

static void cwjap(const char *args)
{
    struct hs_creds c = { 0 };
    const char *p = args;

    if (!quoted(&p, c.ssid, HS_SSID_MAX) || *p++ != ',' ||
        !quoted(&p, c.pass, HS_PASS_MAX) || !c.ssid[0]) {
        printf("ERROR usage: AT+CWJAP=\"ssid\",\"passphrase\"\n");
        return;
    }
    if (hs_creds_save(&c) != ESP_OK) {
        printf("ERROR saving\n");
        return;
    }
    printf("OK, joining \"%s\"\n", c.ssid);
    hs_link_reconnect();
}

static void command(char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == ';' || line[n - 1] == ' '))
        line[--n] = 0;

    if (strncasecmp(line, "AT+CWJAP=", 9) == 0) {
        cwjap(line + 9);
    } else if (strcasecmp(line, "AT+CWJAP?") == 0) {
        struct hs_creds c;
        char ip[16];
        hs_link_ip(ip, sizeof(ip));
        if (hs_creds_load(&c) == ESP_OK)
            printf("+CWJAP:\"%s\" %s %s\n", c.ssid,
                   hs_link_up() ? "connected" : "not connected", ip);
        else
            printf("+CWJAP: none stored\n");
    } else if (strcasecmp(line, "AT+RST") == 0) {
        printf("OK\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (strcmp(line, "AT+SDFORMAT=YES") == 0) {
        printf(storage_format() == ESP_OK ? "OK\n" : "ERROR formatting the card\n");
    } else if (strcasecmp(line, "AT") == 0) {
        printf("OK\n");
    } else if (n) {
        printf("ERROR unknown command\n");
    }
}

static void console_task(void *arg)
{
    char line[CMD_MAX + 1];
    size_t n = 0;
    uint8_t ch;

    for (;;) {
        if (uart_read_bytes(UART, &ch, 1, portMAX_DELAY) != 1)
            continue;
        if (ch == '\r' || ch == '\n') {
            line[n] = 0;
            if (n)
                command(line);
            n = 0;
        } else if (n < CMD_MAX) {
            line[n++] = ch;
        }
    }
}

void hs_console_start(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART, 512, 0, 0, NULL, 0));
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
}
