/* WI-FI TRANSFER (owner 2026-11-24): the SD card's files in any web browser on the same Wi-Fi. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* What the server is doing, for TamaNest's screen. */
typedef struct {
    int64_t last_us;        /* last request (esp_timer time), 0 = none yet */
    int busy;               /* 1 while a file is being received or sent */
    long done, total;       /* bytes of the current/last file */
    int bad_codes;          /* wrong access codes tried */
    char what[16];          /* RECEIVING, RECEIVED, SENDING, SENT, DELETED, NEW FOLDER, FAILED, "" */
    char name[48];          /* file name of the last action */
} fs_status_t;

/* Serve /sdcard over http on port 80. `code` = the access code (also in the QR link as ?k=code). */
esp_err_t file_server_start(const char *code);
void file_server_stop(void);
void file_server_status(fs_status_t *out);
