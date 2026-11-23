/* Wi-Fi station + HTTPS downloads for TamaNest's UPDATE screen (2026-11-22).
 * Memory: the board's internal RAM is tight (the menu screen needs one ~48 KB block per flush), so Wi-Fi/lwIP and
 * TLS buffers go to PSRAM (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP, CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC), and Wi-Fi is
 * only on while it is needed. */
#include "wifi_update.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "TamaNest-wifi";
#define BIT_UP   1
#define BIT_FAIL 2

static esp_netif_t *s_netif;
static EventGroupHandle_t s_ev;
static bool s_on, s_base;
static volatile int s_reason;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupSetBits(s_ev, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, BIT_UP);
    }
}

esp_err_t wifi_on(void)
{
    if (s_on) return ESP_OK;
    if (!s_base) {                                   /* once per boot */
        esp_err_t e = esp_netif_init();
        if (e != ESP_OK) return e;
        e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;   /* already there is fine */
        s_ev = xEventGroupCreate();
        s_base = true;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;                          /* we keep the network ourselves; no extra NVS writes */
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; return e; }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, &s_h_ip);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    e = esp_wifi_start();
    if (e != ESP_OK) { s_on = true; wifi_off(); return e; }
    esp_wifi_set_ps(WIFI_PS_NONE);                   /* full speed while downloading (it is only on briefly) */
    s_on = true;
    ESP_LOGI(TAG, "Wi-Fi on");
    return ESP_OK;
}

void wifi_off(void)
{
    if (!s_on) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (s_h_wifi) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi); s_h_wifi = NULL; }
    if (s_h_ip) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip); s_h_ip = NULL; }
    esp_wifi_deinit();
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    s_on = false;
    ESP_LOGI(TAG, "Wi-Fi off");
}

int wifi_scan(wifi_ap_t *out, int max)
{
    if (!s_on && wifi_on() != ESP_OK) return -1;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;   /* blocking, ~2-3 s */
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    wifi_ap_record_t *rec = calloc(n, sizeof *rec);
    if (!rec) { esp_wifi_clear_ap_list(); return -1; }
    esp_wifi_scan_get_ap_records(&n, rec);            /* sorted by signal strength */
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!rec[i].ssid[0]) continue;
        bool dup = false;                             /* one row per network name (mesh systems repeat it) */
        for (int j = 0; j < k; j++) if (!strcmp(out[j].ssid, (const char *)rec[i].ssid)) { dup = true; break; }
        if (dup) continue;
        snprintf(out[k].ssid, sizeof out[k].ssid, "%s", (const char *)rec[i].ssid);
        out[k].rssi = rec[i].rssi;
        out[k].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    free(rec);
    return k;
}

esp_err_t wifi_connect(const char *ssid, const char *pass, int timeout_ms, const char **why)
{
    static const char *const unknown = "COULD NOT CONNECT";
    if (why) *why = unknown;
    if (!s_on) { esp_err_t e = wifi_on(); if (e != ESP_OK) { if (why) *why = "WI-FI WOULD NOT START"; return e; } }
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass ? pass : "");
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    for (int attempt = 0; attempt < 3; attempt++) {   /* the first try sometimes fails with a busy router */
        xEventGroupClearBits(s_ev, BIT_UP | BIT_FAIL);
        s_reason = 0;
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(s_ev, BIT_UP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        if (b & BIT_UP) { ESP_LOGI(TAG, "Connected"); return ESP_OK; }   /* no network names in the log */
        if (!(b & BIT_FAIL)) { esp_wifi_disconnect(); if (why) *why = "NO ANSWER (TIMEOUT)"; return ESP_ERR_TIMEOUT; }
        int r = s_reason;
        ESP_LOGW(TAG, "Connect failed (reason %d, try %d)", r, attempt + 1);
        if (why) {
            if (r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
                r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD || r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD)
                *why = "NETWORK NOT FOUND";
            else if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                     r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE ||
                     r == WIFI_REASON_CONNECTION_FAIL)
                *why = "WRONG PASSWORD?";
            else *why = unknown;
        }
        if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) break;   /* retrying won't help */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return ESP_FAIL;
}

/* Open url, following redirects (GitHub release links redirect). Returns the client with headers read. */
static esp_http_client_handle_t open_url(const char *url, long *len, esp_err_t *err)
{
    esp_http_client_config_t c = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,     /* checks the server's certificate */
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .max_redirection_count = 6,
        .user_agent = "TamaNest",
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) { *err = ESP_ERR_NO_MEM; return NULL; }
    for (int hop = 0; hop < 6; hop++) {
        *err = esp_http_client_open(h, 0);
        if (*err != ESP_OK) break;
        long n = esp_http_client_fetch_headers(h);
        int st = esp_http_client_get_status_code(h);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            char tmp[256];
            while (esp_http_client_read(h, tmp, sizeof tmp) > 0) { }   /* drain the body */
            *err = esp_http_client_set_redirection(h);
            if (*err != ESP_OK) break;
            continue;
        }
        if (st != 200) { ESP_LOGW(TAG, "%s: HTTP %d", url, st); *err = ESP_ERR_NOT_FOUND; break; }
        *len = n > 0 ? n : esp_http_client_get_content_length(h);
        if (*len < 0) *len = 0;
        return h;
    }
    if (*err == ESP_OK) *err = ESP_FAIL;
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return NULL;
}

esp_err_t net_get_text(const char *url, char *buf, size_t max)
{
    long len = 0; esp_err_t err;
    esp_http_client_handle_t h = open_url(url, &len, &err);
    if (!h) { ESP_LOGW(TAG, "GET %s: %s", url, esp_err_to_name(err)); return err; }
    size_t got = 0;
    while (got + 1 < max) {
        int r = esp_http_client_read(h, buf + got, (int)(max - 1 - got));
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) break;
        got += (size_t)r;
    }
    buf[got] = 0;
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return err;
}

esp_err_t net_download(const char *url, const char *path, void (*progress)(long, long))
{
    long len = 0; esp_err_t err;
    esp_http_client_handle_t h = open_url(url, &len, &err);
    if (!h) { ESP_LOGW(TAG, "Download %s: %s", url, esp_err_to_name(err)); return err; }
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    char *buf = malloc(4096);
    long done = 0;
    if (!f || !buf) err = ESP_FAIL;
    while (err == ESP_OK) {
        int r = esp_http_client_read(h, buf, 4096);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) {
            if (!esp_http_client_is_complete_data_received(h)) err = ESP_FAIL;   /* connection dropped */
            break;
        }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { err = ESP_FAIL; break; }
        done += r;
        if (progress) progress(done, len);
    }
    free(buf);
    if (f && fclose(f) != 0) err = ESP_FAIL;
    if (err == ESP_OK && len && done != len) err = ESP_FAIL;
    if (err == ESP_OK) { remove(path); if (rename(tmp, path) != 0) err = ESP_FAIL; }
    else remove(tmp);
    ESP_LOGI(TAG, "Download %s -> %s: %ld bytes, %s", url, path, done, esp_err_to_name(err));
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return err;
}

esp_err_t net_get_utc(time_t *utc, int timeout_ms)
{
    /* three servers (CONFIG_LWIP_SNTP_MAX_SERVERS 3): if one is blocked or slow, another answers */
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
                              ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org", "time.cloudflare.com"));
    esp_err_t e = esp_netif_sntp_init(&c);
    if (e != ESP_OK) { ESP_LOGW(TAG, "SNTP start: %s", esp_err_to_name(e)); return e; }
    e = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (e == ESP_OK) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        *utc = tv.tv_sec;
        ESP_LOGI(TAG, "Internet time received (UTC %lld)", (long long)tv.tv_sec);
    } else ESP_LOGW(TAG, "Internet time: %s", esp_err_to_name(e));
    esp_netif_sntp_deinit();
    return e;
}
