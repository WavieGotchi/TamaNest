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
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "lwip/sockets.h"
#include "font_p2_woff2.h"
#include "dns_server.h"
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
static int s_rssi;                                  /* signal of the last connection (dBm), 0 = unknown */
int wifi_rssi(void) { return s_rssi; }
bool wifi_ip(char *buf, size_t n)                    /* TamaNest's address on the home network, while connected */
{
    esp_netif_ip_info_t ip;
    if (!s_on || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK || !ip.ip.addr) return false;
    snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    return true;
}
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

static esp_err_t base_init(void)                     /* network stack + event loop, once per boot */
{
    if (s_base) return ESP_OK;
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;   /* already there is fine */
    s_ev = xEventGroupCreate();
    s_base = true;
    return ESP_OK;
}

esp_err_t wifi_on(void)
{
    if (s_on) return ESP_OK;
    esp_err_t be = base_init();
    if (be != ESP_OK) return be;
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
        if (b & BIT_UP) {                             /* no network names in the log */
            wifi_ap_record_t ap;
            s_rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
            ESP_LOGI(TAG, "Connected (signal %d dBm)", s_rssi);
            return ESP_OK;
        }
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

static void net_log_memory(const char *what, int attempt)
{
    ESP_LOGI(TAG, "%s, try %d: internal RAM free %u, largest block %u", what, attempt,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
static esp_err_t net_get_text_once(const char *url, char *buf, size_t max);
static esp_err_t net_download_once(const char *url, uint8_t **mem, long *got, void (*progress)(long, long));
static esp_err_t net_save_file(const char *path, const uint8_t *mem, long n);
/* Weak Wi-Fi (2026-11-23, owner's test at -69..-81 dBm): a TLS handshake that stalls is reset by the server, or a
 * download drops. Try up to 3 times, a little apart, unless the site said the file does not exist. */
#define NET_TRIES 3
esp_err_t net_get_text(const char *url, char *buf, size_t max)
{
    esp_err_t e = ESP_FAIL;
    for (int t = 1; t <= NET_TRIES; t++) {
        net_log_memory("Get", t);
        e = net_get_text_once(url, buf, max);
        if (e == ESP_OK || e == ESP_ERR_NOT_FOUND) break;
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    return e;
}
esp_err_t net_download(const char *url, const char *path, void (*progress)(long done, long total))
{
    esp_err_t e = ESP_FAIL;
    uint8_t *mem = NULL; long n = 0;
    for (int t = 1; t <= NET_TRIES; t++) {
        net_log_memory("Download", t);
        e = net_download_once(url, &mem, &n, progress);
        if (e == ESP_OK || e == ESP_ERR_NOT_FOUND) break;
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    if (e != ESP_OK) return e;
    wifi_off();                                       /* frees the internal RAM the SD card driver needs */
    e = net_save_file(path, mem, n);
    free(mem);
    return e;
}
static esp_err_t net_get_text_once(const char *url, char *buf, size_t max)
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

/* Download into PSRAM (2026-11-23): while Wi-Fi and TLS run, only ~15 KB of internal RAM is left (largest block
 * ~7.5 KB) and the SD card driver could not get its DMA buffer ("allocate_dma_buf: not enough mem"), so writing the
 * file during the download failed. The whole file (~1.7 MB) is kept in PSRAM and written after Wi-Fi is off. */
#define NET_MAX_FILE (4 * 1024 * 1024)
static esp_err_t net_download_once(const char *url, uint8_t **mem, long *got, void (*progress)(long, long))
{
    long len = 0; esp_err_t err;
    *got = 0;
    esp_http_client_handle_t h = open_url(url, &len, &err);
    if (!h) { ESP_LOGW(TAG, "Download %s: %s", url, esp_err_to_name(err)); return err; }
    long cap = len > 0 ? len : 2 * 1024 * 1024;
    if (cap > NET_MAX_FILE) err = ESP_ERR_INVALID_SIZE;
    uint8_t *buf = err == ESP_OK ? heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM) : NULL;
    if (err == ESP_OK && !buf) err = ESP_ERR_NO_MEM;
    long done = 0;
    while (err == ESP_OK) {
        if (done == cap) {                           /* size not announced: grow */
            if (cap >= NET_MAX_FILE) { err = ESP_ERR_INVALID_SIZE; break; }
            long nc = cap * 2 > NET_MAX_FILE ? NET_MAX_FILE : cap * 2;
            uint8_t *nb = heap_caps_realloc(buf, (size_t)nc, MALLOC_CAP_SPIRAM);
            if (!nb) { err = ESP_ERR_NO_MEM; break; }
            buf = nb; cap = nc;
        }
        long want = cap - done < 4096 ? cap - done : 4096;
        int r = esp_http_client_read(h, (char *)buf + done, (int)want);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) {
            if (!esp_http_client_is_complete_data_received(h)) err = ESP_FAIL;   /* connection dropped */
            break;
        }
        done += r;
        if (progress) progress(done, len);
    }
    if (err == ESP_OK && len && done != len) err = ESP_FAIL;
    ESP_LOGI(TAG, "Download %s: %ld bytes into memory, %s", url, done, esp_err_to_name(err));
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) { free(buf); return err; }
    *mem = buf; *got = done;
    return ESP_OK;
}
static esp_err_t net_save_file(const char *path, const uint8_t *mem, long n)   /* Wi-Fi is off by now */
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    esp_err_t err = f ? ESP_OK : ESP_FAIL;
    for (long o = 0; err == ESP_OK && o < n; o += 4096) {
        size_t c = n - o < 4096 ? (size_t)(n - o) : 4096;
        if (fwrite(mem + o, 1, c, f) != c) err = ESP_FAIL;
    }
    if (f && fclose(f) != 0) err = ESP_FAIL;
    if (err == ESP_OK) { remove(path); if (rename(tmp, path) != 0) err = ESP_FAIL; }
    else remove(tmp);
    ESP_LOGI(TAG, "Saved %s: %ld bytes, %s", path, n, esp_err_to_name(err));
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


/* ---------------------------------------------------------------------------------------------------------------
 * PHONE SETUP (owner 2026-11-24): instead of typing the password on the device, TamaNest becomes its own little
 * Wi-Fi network for a few minutes. The phone joins it (scan the QR code on the screen, or type the name and the
 * 8-digit password shown there), a setup page opens by itself (every name the phone looks up is answered with
 * TamaNest's address: a "captive portal", like hotel Wi-Fi), and the phone sends the home network's name and
 * password. The setup network is password-protected (WPA2) so nobody nearby can read what the phone sends.
 * Nothing is logged except counts. */
static httpd_handle_t s_httpd;
static dns_server_handle_t s_dns;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_h_ap;
static bool s_portal;
static wifi_ap_t s_pnets[16];
static int s_pn;
static volatile int s_phones;
static volatile bool s_got;
static char s_p_ssid[33], s_p_pass[65];

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_phones++;
        ESP_LOGI(TAG, "Phone setup: a phone joined (internal RAM free %u, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_phones > 0) s_phones--;
}

static void send_esc(httpd_req_t *r, const char *t)  /* text into the page, HTML-escaped */
{
    char b[64]; size_t k = 0;
    for (; *t; t++) {
        const char *rep = *t == '<' ? "&lt;" : *t == '>' ? "&gt;" : *t == '&' ? "&amp;" : *t == '"' ? "&quot;" :
                          *t == '\'' ? "&#39;" : NULL;
        size_t l = rep ? strlen(rep) : 1;
        if (k + l >= sizeof b) { httpd_resp_send_chunk(r, b, k); k = 0; }
        if (rep) { memcpy(b + k, rep, l); k += l; } else b[k++] = *t;
    }
    if (k) httpd_resp_send_chunk(r, b, k);
}

static const char PAGE_TOP[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>TamaNest Wi-Fi setup</title>"
    "<link rel=icon href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' shape-rendering='crispEdges'%3E%3Crect width='16' height='16' rx='3' fill='%23a8d47c'/%3E%3Cpath fill='%231c2a14' d='M6 2h4v1h-4zM5 3h1v1h-1zM10 3h1v1h-1zM4 4h1v1h-1zM6 4h1v1h-1zM11 4h1v1h-1zM4 5h2v1h-2zM11 5h1v1h-1zM3 6h1v1h-1zM5 6h1v1h-1zM12 6h1v1h-1zM3 7h1v1h-1zM12 7h1v1h-1zM3 8h1v1h-1zM12 8h1v1h-1zM2 9h12v1h-12zM2 10h1v1h-1zM4 10h1v1h-1zM6 10h1v1h-1zM9 10h1v1h-1zM11 10h1v1h-1zM13 10h1v1h-1zM3 11h1v1h-1zM5 11h1v1h-1zM7 11h2v1h-2zM10 11h1v1h-1zM12 11h1v1h-1zM4 12h8v1h-8z'/%3E%3C/svg%3E\"><style>"
    "@font-face{font-family:P2;src:url(/p2.woff2) format('woff2')}"
    "body{margin:0;background:#a8d47c;color:#1c2a14;font:17px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;padding:20px 16px}"
    "main{max-width:440px;margin:0 auto}"
    "h1{font:1.6rem/1.3 P2,ui-monospace,monospace;margin:4px 0 8px}"
    ".band{background:#80a862;border-bottom:3px solid #1c2a14;padding:10px 12px 8px;font:.7rem/1.6 P2,ui-monospace,monospace;margin-top:18px}"
    ".box{background:#c4e4a0;border:2px solid #1c2a14;border-top:0;padding:4px 14px 16px}"
    "label{display:block;margin:12px 0 4px;font-weight:700}"
    "select,input[type=text],input[type=password]{width:100%;box-sizing:border-box;font:inherit;padding:10px;border:2px solid #1c2a14;background:#eef7e2;color:#1c2a14;border-radius:0}"
    ".row{display:flex;align-items:center;gap:10px;margin-top:12px}.row input{width:22px;height:22px;margin:0}.row label{margin:0;font-weight:400}"
    "button{margin-top:18px;width:100%;padding:16px 14px 14px;font:.8rem/1.4 P2,ui-monospace,monospace;background:#1c2a14;color:#a8d47c;border:0;cursor:pointer}"
    "p{margin:10px 0}.note{font-size:.9rem;color:#34482a}"
    "</style></head><body><main><h1>TAMANEST</h1><p style=margin-top:0>Wi-Fi setup</p>";
static const char PAGE_END[] = "</main></body></html>";

static esp_err_t page_form(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send_chunk(r, PAGE_TOP, HTTPD_RESP_USE_STRLEN);
    httpd_resp_sendstr_chunk(r,
        /* Sent with fetch(): a classic form post over http makes Chrome warn "not secure", although the details
         * only travel over TamaNest's own WPA2 network. The plain form still works without JavaScript. */
        "<script>function go(f){fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:new URLSearchParams(new FormData(f)).toString()}).then(function(r){return r.text()})"
        ".then(function(t){document.open();document.write(t);document.close()})"
        ".catch(function(){f.submit()});return false}</script>"
        "<form method=post action=/save autocomplete=off onsubmit=\"return go(this)\"><div class=band>YOUR WI-FI</div><div class=box>"
        "<label for=n>Network</label><select id=n name=n onchange=\"document.getElementById('od').style.display="
        "this.value=='x'?'block':'none'\">");
    for (int i = 0; i < s_pn; i++) {
        char v[40];
        snprintf(v, sizeof v, "<option value=%d>", i);
        httpd_resp_sendstr_chunk(r, v);
        send_esc(r, s_pnets[i].ssid);
        httpd_resp_sendstr_chunk(r, "</option>");
    }
    httpd_resp_sendstr_chunk(r, s_pn ? "<option value=x>Other (type its name)</option></select><div id=od style=display:none>"
                                     : "<option value=x selected>Other (type its name)</option></select><div id=od>");
    httpd_resp_sendstr_chunk(r,
        "<label for=o>Network name</label><input type=text id=o name=o maxlength=32 autocapitalize=off autocorrect=off spellcheck=false></div>"
        "<label for=p>Password</label><input type=password id=p name=p maxlength=64 autocapitalize=off autocorrect=off spellcheck=false>"
        "<div class=row><input type=checkbox id=s onchange=\"document.getElementById('p').type=this.checked?'text':'password'\">"
        "<label for=s>Show password</label></div>"
        "<button type=submit>CONNECT</button></div></form>"
        "<p class=note>TamaNest keeps your network name and password only on the device. It uses Wi-Fi to check for "
        "updates and to set the time, and switches it off the rest of the time.</p>");
    httpd_resp_sendstr_chunk(r, PAGE_END);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* value of field `key` from a form body (a=1&b=2, %XX and + decoded); false if missing or too long */
static bool form_field(const char *body, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        p += kl + 1;
        size_t k = 0;
        while (*p && *p != '&') {
            int c = *p++;
            if (c == '+') c = ' ';
            else if (c == '%' && hexv(p[0]) >= 0 && hexv(p[1]) >= 0) { c = hexv(p[0]) * 16 + hexv(p[1]); p += 2; }
            if (k + 1 >= n) { out[0] = 0; return false; }
            out[k++] = (char)c;
        }
        out[k] = 0;
        return true;
    }
    out[0] = 0;
    return false;
}

static esp_err_t page_save(httpd_req_t *r)
{
    char body[640];
    int len = r->content_len, got = 0;
    if (len <= 0 || len >= (int)sizeof body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Too long"); return ESP_FAIL; }
    while (got < len) {
        int k = httpd_req_recv(r, body + got, len - got);
        if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;
    char nsel[8], ssid[33], pass[65];
    form_field(body, "n", nsel, sizeof nsel);
    if (nsel[0] == 'x' || !nsel[0]) form_field(body, "o", ssid, sizeof ssid);
    else { int i = atoi(nsel); snprintf(ssid, sizeof ssid, "%s", (i >= 0 && i < s_pn) ? s_pnets[i].ssid : ""); }
    bool pass_ok = form_field(body, "p", pass, sizeof pass) || !strstr(body, "p=");
    memset(body, 0, sizeof body);                     /* the password does not linger in memory */
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send_chunk(r, PAGE_TOP, HTTPD_RESP_USE_STRLEN);
    const size_t pl = strlen(pass);
    if (!ssid[0] || !pass_ok || (pl > 0 && pl < 8)) {
        httpd_resp_sendstr_chunk(r, "<div class=band>PLEASE CHECK</div><div class=box><p>");
        httpd_resp_sendstr_chunk(r, !ssid[0] ? "Please choose a network, or type its name."
                                             : "Wi-Fi passwords are 8 to 63 characters long.");
        httpd_resp_sendstr_chunk(r, "</p><p><a href=/ style=color:#1c2a14>Back</a></p></div>");
    } else if (!s_got) {
        snprintf(s_p_ssid, sizeof s_p_ssid, "%s", ssid);
        snprintf(s_p_pass, sizeof s_p_pass, "%s", pass);
        __sync_synchronize();
        s_got = true;
        ESP_LOGI(TAG, "Phone setup: network details received");
        httpd_resp_sendstr_chunk(r, "<div class=band>THANK YOU</div><div class=box><p>TamaNest is now connecting to <b>");
        send_esc(r, ssid);
        httpd_resp_sendstr_chunk(r, "</b>.</p><p>Your phone will leave the TamaNest network in a moment. "
                                    "Look at TamaNest's screen to see if it worked.</p></div>");
    } else {
        httpd_resp_sendstr_chunk(r, "<div class=band>ALREADY SENT</div><div class=box><p>TamaNest is already connecting. "
                                    "Look at its screen.</p></div>");
    }
    memset(pass, 0, sizeof pass);
    httpd_resp_sendstr_chunk(r, PAGE_END);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t page_font(httpd_req_t *r)          /* the pixel font for the headings (font_p2_woff2.h) */
{
    httpd_resp_set_type(r, "font/woff2");
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=86400");
    return httpd_resp_send(r, (const char *)FONT_P2_WOFF2, sizeof FONT_P2_WOFF2);
}

/* Anything else (the phone's "am I online?" checks, other sites) is sent to the setup page. iOS needs some
 * content with the redirect to notice the portal. */
static esp_err_t page_redirect(httpd_req_t *r, httpd_err_code_t err)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send(r, "TamaNest Wi-Fi setup", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Things the system creates once, the first time they are needed, and then keeps for good (found with heap
 * tracing, 2026-11-24): log level entries, the crypto hardware lock, mbedtls/PSA mutexes, and the calling task's
 * lwIP socket semaphore. Made here, BEFORE TamaNest lends the emulator's memory to Wi-Fi, so they land elsewhere
 * and the emulator gets its 65 KB back in one piece afterwards (else TamaNest has to restart). */
void portal_prepare(void)
{
    base_init();
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);    /* the redirects make lots of harmless noise */
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("dns_server", ESP_LOG_WARN);     /* it logs every name the phone looks up */
    int sk = socket(AF_INET, SOCK_DGRAM, 0);          /* this task's lwIP semaphore (httpd_start opens a socket here) */
    if (sk >= 0) close(sk);
    if (psa_crypto_init() == PSA_SUCCESS) {           /* WPA2 uses SHA/HMAC through PSA: locks and mutexes */
        uint8_t h[32], r[4], kb[16] = { 0 };
        size_t hl;
        psa_hash_compute(PSA_ALG_SHA_256, kb, sizeof kb, h, sizeof h, &hl);
        psa_generate_random(r, sizeof r);
        psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&at, PSA_KEY_TYPE_HMAC);
        psa_set_key_bits(&at, 128);
        psa_set_key_usage_flags(&at, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&at, PSA_ALG_HMAC(PSA_ALG_SHA_1));
        psa_key_id_t k;
        if (psa_import_key(&at, kb, sizeof kb, &k) == PSA_SUCCESS) {
            uint8_t mac[20]; size_t ml;
            psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_1), kb, sizeof kb, mac, sizeof mac, &ml);
            psa_destroy_key(k);
        }
    }
}

esp_err_t portal_start(const char *ap_ssid, const char *ap_pass, const wifi_ap_t *nets, int n)
{
    if (s_portal) return ESP_OK;
    wifi_off();
    esp_err_t e = base_init();
    if (e != ESP_OK) return e;
    s_pn = 0;
    for (int i = 0; i < n && s_pn < (int)(sizeof s_pnets / sizeof s_pnets[0]); i++) s_pnets[s_pn++] = nets[i];
    s_phones = 0; s_got = false;
    ESP_LOGI(TAG, "Phone setup: internal RAM free %u before Wi-Fi", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) { esp_netif_destroy_default_wifi(s_ap_netif); s_ap_netif = NULL; return e; }
    s_portal = true;                                  /* from here on portal_stop() cleans up */
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_ap_event, NULL, &s_h_ap);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.ap.ssid, sizeof wc.ap.ssid, "%s", ap_ssid);
    wc.ap.ssid_len = strlen(ap_ssid);
    snprintf((char *)wc.ap.password, sizeof wc.ap.password, "%s", ap_pass);
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.channel = 6;
    wc.ap.max_connection = 2;
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    memset(&wc, 0, sizeof wc);
    if ((e = esp_wifi_start()) != ESP_OK) { portal_stop(); return e; }
    /* DHCP option 114 tells newer phones the setup page's address straight away */
    static const char uri[] = "http://192.168.4.1";
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)uri, strlen(uri));
    esp_netif_dhcps_start(s_ap_netif);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 5;
    hc.lru_purge_enable = true;
    hc.stack_size = 5120;
    hc.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   /* stack in PSRAM: internal RAM is short with Wi-Fi on */
    if ((e = httpd_start(&s_httpd, &hc)) != ESP_OK) { s_httpd = NULL; portal_stop(); return e; }
    static const httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = page_form };
    static const httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = page_save };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_save);
    static const httpd_uri_t u_font = { .uri = "/p2.woff2", .method = HTTP_GET, .handler = page_font };
    httpd_register_uri_handler(s_httpd, &u_font);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, page_redirect);
    httpd_register_err_handler(s_httpd, HTTPD_405_METHOD_NOT_ALLOWED, page_redirect);
    dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dc);
    ESP_LOGI(TAG, "Phone setup on (%d networks offered; internal RAM free %u, largest %u)", s_pn,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

int portal_phones(void) { return s_phones; }

bool portal_take(char *ssid, size_t ns, char *pass, size_t np)
{
    if (!s_got) return false;
    __sync_synchronize();
    snprintf(ssid, ns, "%s", s_p_ssid);
    snprintf(pass, np, "%s", s_p_pass);
    memset(s_p_pass, 0, sizeof s_p_pass);
    return true;
}

void portal_stop(void)
{
    if (!s_portal) return;
    if (s_dns) { stop_dns_server(s_dns); s_dns = NULL; }
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();
    if (s_h_ap) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_ap); s_h_ap = NULL; }
    esp_wifi_deinit();
    if (s_ap_netif) { esp_netif_destroy_default_wifi(s_ap_netif); s_ap_netif = NULL; }
    memset(s_p_pass, 0, sizeof s_p_pass);
    s_got = false; s_phones = 0; s_portal = false;
    ESP_LOGI(TAG, "Phone setup off");
}

/* Name: the last two bytes of the chip's own address tell devices apart. Password: random, made once per device and
 * kept (NVS wifi/appw), so a phone that joined before reconnects by itself (a new one each time made phones retry
 * the old one and fail). The network only exists while the PHONE SETUP screen is open. */
void portal_names(char *ssid, size_t ns, char *pass, size_t np)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, ns, "TAMANEST-%02X%02X", mac[4], mac[5]);
    char pw[12] = "";
    size_t l = sizeof pw;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_str(h, "appw", pw, &l) != ESP_OK || strlen(pw) != 8) {
            snprintf(pw, sizeof pw, "%08lu", (unsigned long)(esp_random() % 100000000UL));
            nvs_set_str(h, "appw", pw);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    if (strlen(pw) != 8) snprintf(pw, sizeof pw, "%08lu", (unsigned long)(esp_random() % 100000000UL));
    snprintf(pass, np, "%s", pw);
}
