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
void wifi_off(void);
bool wifi_ip(char *buf, size_t n);                 /* "192.168.1.23" while connected */
int wifi_rssi(void);                                     /* signal of the last connection in dBm (0 = unknown) */                                       /* disconnect, stop, free the driver */

/* HTTPS GET into a buffer (text, zero-terminated). Follows redirects. */
esp_err_t net_get_text(const char *url, char *buf, size_t max);
/* HTTPS GET into a file: downloaded into PSRAM first, then Wi-Fi is SWITCHED OFF and the file written (as
 * path.tmp, renamed when complete). progress(done, total) may be NULL; total is 0 if the server did not say.
 * Returns ESP_OK only for a complete file. Up to 3 tries. */
esp_err_t net_download(const char *url, const char *path, void (*progress)(long done, long total));

/* Exact time from internet time servers (SNTP), while connected. *utc = seconds since 1970 (UTC). Leaves the
 * ESP32's own clock set to UTC - the caller sets it back (TamaNest keeps it at local time for SD card file dates). */
#include <time.h>
esp_err_t net_get_utc(time_t *utc, int timeout_ms);

/* Phone setup (soft access point + setup page). portal_names: "TAMANEST-XXXX" + a new 8-digit password.
 * portal_start shows `nets` (from wifi_scan) on the page. portal_take: true once the phone sent a network
 * (copies it). portal_stop switches it all off. */
void portal_names(char *ssid, size_t ns, char *pass, size_t np);
void portal_prepare(void);                   /* call before freeing memory for the access point */
esp_err_t portal_start(const char *ap_ssid, const char *ap_pass, const wifi_ap_t *nets, int n);
int portal_phones(void);
bool portal_take(char *ssid, size_t ns, char *pass, size_t np);
void portal_stop(void);
