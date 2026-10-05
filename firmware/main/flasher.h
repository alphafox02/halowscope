/*
 * Copyright 2026 CEMAXECUTER LLC
 *
 * Firmware slots: other ESP-IDF apps for this board, kept on the TF card in
 * /firmware, can be written into the spare app slots in flash and started
 * from the page. HaLowScope stays in the factory slot as the way back.
 *
 * A slot can be started until the next restart (any reset, crash or power
 * cycle comes back to the HaLowScope that started it) or kept (it starts at
 * every boot until changed from its own page, if it has a way, or over USB:
 * erasing otadata, `esptool erase-region 0x410000 0x2000`, starts factory).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "storage.h"

#define FLASHER_DIR STORAGE_ROOT "/firmware"
#define FLASHER_SLOTS 4      /* factory and three spare */
#define FLASHER_FILES 16
#define FLASHER_NAME_MAX 64

struct flasher_app {         /* an image's app description */
    char project[32], version[32], date[16], idf[32];
};

struct flasher_slot {
    char label[17];
    uint32_t size;
    bool has_app, running, next;   /* next: what the next boot starts */
    struct flasher_app app;
};

struct flasher_file {
    char name[FLASHER_NAME_MAX];
    uint32_t size;
    bool ok;                 /* an app image for the ESP32-S3 */
    struct flasher_app app;
};

struct flasher_status {
    bool busy;
    int percent;             /* of the current install, or -1 */
    char text[96];           /* what it is doing, or how the last job went */
};

/* Call at startup. Confirms the running app if it was started as a trial. */
void flasher_start(void);

unsigned flasher_slots(struct flasher_slot *out);
/* The card's /firmware folder; false if there is no card. */
bool flasher_files(struct flasher_file *out, unsigned max, unsigned *count);

/* A plain file name ending in .bin; writes its full path. */
bool flasher_path(const char *name, char *out, unsigned len);
bool flasher_remove(const char *name);

/* These start in the background and report through flasher_status(); false
 * if another job is running or the arguments are wrong (see the status). */
bool flasher_install(const char *name, const char *slot);
bool flasher_boot(const char *slot, bool keep);
void flasher_status(struct flasher_status *out);
