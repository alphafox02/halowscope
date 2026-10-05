/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Outputs for detector events: MQTT, and Cursor on Target (CoT) over UDP
 * multicast for TAK clients. Both send a few small messages per event.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct outputs_config {
    /* Where the node is: CoT needs it. */
    bool has_position;
    double lat, lon;        /* degrees */
    float alt_m;            /* height above the WGS84 ellipsoid */
    char callsign[24];

    bool mqtt_on;
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_topic[64];    /* events go to <topic>/event */
    char mqtt_user[32];
    char mqtt_pass[32];     /* never sent back to the page */

    bool cot_on;
    char cot_group[16];     /* multicast group, or one receiver's address */
    uint16_t cot_port;
};

struct outputs_status {
    const char *mqtt;       /* "off", "connecting", "connected", or why not */
    const char *cot;        /* "off", "on", or why not */
    uint32_t mqtt_sent, cot_sent;
};

void outputs_start(void);
void outputs_get_config(struct outputs_config *out);
/* Validates, applies and saves; an empty password keeps the saved one. */
bool outputs_set_config(const struct outputs_config *c);
void outputs_status(struct outputs_status *out);
