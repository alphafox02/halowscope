/*
 * Copyright 2026 CEMAXECUTER LLC
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* HaLow station credentials, as the stock Elecrow firmware stores them. */
#define HS_SSID_MAX 32
#define HS_PASS_MAX 64

struct hs_creds {
    char ssid[HS_SSID_MAX + 1];
    char pass[HS_PASS_MAX + 1];
};

esp_err_t hs_creds_load(struct hs_creds *c);
esp_err_t hs_creds_save(const struct hs_creds *c);

/* Bring up the HaLow interface and join the stored network. */
void hs_link_start(void);
/* Drop the current association and join again with the stored credentials. */
void hs_link_reconnect(void);
bool hs_link_up(void);
/* Dotted IPv4 address, or "" when there is none. */
void hs_link_ip(char *buf, size_t len);
int hs_link_rssi(void);

void hs_console_start(void);
void hs_web_start(void);
