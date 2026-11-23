/* Wi-Fi for TamaNest updates (2026-11-22). Wi-Fi is only switched on while the UPDATE screen checks or downloads,
 * then switched off and its memory given back. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct { char ssid[33]; int8_t rssi; bool secure; } wifi_ap_t;

esp_err_t wifi_on(void);                                   /* start the Wi-Fi station (no connection yet) */
int wifi_scan(wifi_ap_t *out, int max);                    /* networks nearby, strongest first; -1 = error */
/* Connect and wait for an address. Returns ESP_OK, ESP_ERR_TIMEOUT, or ESP_FAIL (reason in *why: "WRONG PASSWORD",
 * "NETWORK NOT FOUND", ...) */
esp_err_t wifi_connect(const char *ssid, const char *pass, int timeout_ms, const char **why);
void wifi_off(void);                                       /* disconnect, stop, free the driver */

/* HTTPS GET into a buffer (text, zero-terminated). Follows redirects. */
esp_err_t net_get_text(const char *url, char *buf, size_t max);
/* HTTPS GET into a file (written as path.tmp, renamed when complete). progress(done, total) may be NULL;
 * total is 0 if the server did not say. Returns ESP_OK only for a complete file. */
esp_err_t net_download(const char *url, const char *path, void (*progress)(long done, long total));

/* Exact time from internet time servers (SNTP), while connected. *utc = seconds since 1970 (UTC). Leaves the
 * ESP32's own clock set to UTC - the caller sets it back (TamaNest keeps it at local time for SD card file dates). */
#include <time.h>
esp_err_t net_get_utc(time_t *utc, int timeout_ms);
