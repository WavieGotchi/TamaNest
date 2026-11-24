/* WI-FI TRANSFER (owner 2026-11-24): a small file manager for the SD card, in any web browser on the same Wi-Fi
 * (phone, tablet or computer, no app). Browse, upload (several files, with progress), download, delete, new folder.
 *
 * Access: a new 6-digit code for every session, shown on TamaNest's screen and included in its QR link
 * (http://<address>/?k=<code>); the browser then keeps it as a cookie. After 20 wrong codes no more codes are
 * accepted until the session is restarted. The server only runs while the WI-FI TRANSFER screen is open.
 *
 * Memory: SD writes go from an internal, DMA-capable buffer in whole 4 KB sectors, so the SD driver needs no extra
 * internal memory while Wi-Fi runs (that failed once during the update work). TamaNest lends the emulator's memory
 * to Wi-Fi meanwhile (main.c), as for PHONE SETUP. */
#include "file_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "font_p2_woff2.h"

static const char *TAG = "TamaNest-files";
#define ROOT "/sdcard"
#define FBUF 8192                                   /* transfer buffer: 2 SD sectors */
#define MAX_BAD 20

static httpd_handle_t s_srv;
static char s_code[8];
static uint8_t *s_buf;                              /* used only by the transfer task */
/* Transfers run in their own task so the page stays usable meanwhile (owner 2026-11-24: the page froze during a
 * download - the server answers one request at a time). Queued one after the other. */
typedef struct { httpd_req_t *req; esp_err_t (*fn)(httpd_req_t *); } xfer_job_t;
static QueueHandle_t s_jobs;
static TaskHandle_t s_worker;
static volatile bool s_stopping;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fs_status_t s_st;

static void st_set(const char *what, const char *name, long done, long total, int busy)
{
    const char *base = name ? strrchr(name, '/') : NULL;
    base = base ? base + 1 : (name ? name : "");
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_st.what, sizeof s_st.what, "%s", what);
    snprintf(s_st.name, sizeof s_st.name, "%s", base);
    s_st.done = done; s_st.total = total; s_st.busy = busy;
    s_st.last_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mux);
}
static void st_progress(long done)
{
    taskENTER_CRITICAL(&s_mux);
    s_st.done = done; s_st.last_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mux);
}
static void st_touch(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_st.last_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mux);
}
void file_server_status(fs_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_st;
    taskEXIT_CRITICAL(&s_mux);
}

/* ---- helpers ---- */
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void urldec(char *o, const char *s, size_t n)
{
    size_t k = 0;
    while (*s && k + 1 < n) {
        int c = *s++;
        if (c == '+') c = ' ';
        else if (c == '%' && hexv(s[0]) >= 0 && hexv(s[1]) >= 0) { c = hexv(s[0]) * 16 + hexv(s[1]); s += 2; }
        o[k++] = (char)c;
    }
    o[k] = 0;
}
static bool qarg(httpd_req_t *r, const char *key, char *out, size_t n)
{
    char q[1100], raw[1000];                        /* a 200-byte path is up to 600 characters once %-encoded */
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, raw, sizeof raw) != ESP_OK) return false;
    urldec(out, raw, n);
    return true;
}
/* "/roms/x.bin" style: starts with '/', no "." or ".." parts, no backslash or control characters */
static bool path_ok(const char *p)
{
    size_t l = strlen(p);
    if (p[0] != '/' || l > 200) return false;
    for (const char *c = p; *c; c++) if ((unsigned char)*c < 0x20 || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') return false;
    for (const char *s = p; s; s = strchr(s + 1, '/')) {
        const char *e = strchr(s + 1, '/');
        size_t sl = e ? (size_t)(e - s - 1) : strlen(s + 1);
        if ((sl == 1 && s[1] == '.') || (sl == 2 && s[1] == '.' && s[2] == '.')) return false;
        if (sl == 0 && e) return false;             /* "//" */
    }
    return true;
}
static void full_path(char *out, size_t n, const char *p)
{
    snprintf(out, n, ROOT "%s", p);
    size_t l = strlen(out);
    if (l > strlen(ROOT) + 1 && out[l - 1] == '/') out[l - 1] = 0;
}
static esp_err_t send_json_err(httpd_req_t *r, const char *status, const char *msg)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    char b[160];
    snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(r, b);
}
static void send_json_str(httpd_req_t *r, const char *t)  /* a JSON string value, escaped */
{
    char b[96]; size_t k = 0;
    b[k++] = '"';
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (k > sizeof b - 8) { httpd_resp_send_chunk(r, b, k); k = 0; }
        if (c == '"' || c == '\\') { b[k++] = '\\'; b[k++] = (char)c; }
        else if (c < 0x20) k += snprintf(b + k, sizeof b - k, "\\u%04x", c);
        else b[k++] = (char)c;
    }
    b[k++] = '"';
    httpd_resp_send_chunk(r, b, k);
}

static bool authed(httpd_req_t *r)
{
    if (s_st.bad_codes >= MAX_BAD) return false;
    char v[16]; size_t l = sizeof v;
    esp_err_t e = httpd_req_get_cookie_val(r, "tn", v, &l);
    bool ok = e == ESP_OK && !strcmp(v, s_code);
    ESP_LOGI(TAG, "%s %.40s: cookie %s", r->method == HTTP_POST ? "POST" : "GET", r->uri,
             ok ? "ok" : e == ESP_OK ? "WRONG" : e == ESP_ERR_NOT_FOUND ? "none" : "unreadable");
    return ok;
}
static esp_err_t deny(httpd_req_t *r) { return send_json_err(r, "403 Forbidden", "Please open TamaNest's link again (scan its QR code)."); }

/* ---- pages ---- */
static const char PAGE_CSS[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>TamaNest files</title>"
    "<link rel=icon href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' shape-rendering='crispEdges'%3E%3Crect width='16' height='16' rx='3' fill='%23a8d47c'/%3E%3Cpath fill='%231c2a14' d='M6 2h4v1h-4zM5 3h1v1h-1zM10 3h1v1h-1zM4 4h1v1h-1zM6 4h1v1h-1zM11 4h1v1h-1zM4 5h2v1h-2zM11 5h1v1h-1zM3 6h1v1h-1zM5 6h1v1h-1zM12 6h1v1h-1zM3 7h1v1h-1zM12 7h1v1h-1zM3 8h1v1h-1zM12 8h1v1h-1zM2 9h12v1h-12zM2 10h1v1h-1zM4 10h1v1h-1zM6 10h1v1h-1zM9 10h1v1h-1zM11 10h1v1h-1zM13 10h1v1h-1zM3 11h1v1h-1zM5 11h1v1h-1zM7 11h2v1h-2zM10 11h1v1h-1zM12 11h1v1h-1zM4 12h8v1h-8z'/%3E%3C/svg%3E\"><style>"
    "@font-face{font-family:P2;src:url(/p2.woff2) format('woff2')}"
    "body{margin:0;background:#a8d47c;color:#1c2a14;font:16px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;padding:18px 14px 40px}"
    "main{max-width:720px;margin:0 auto}"
    "h1{font:1.6rem/1.3 P2,ui-monospace,monospace;margin:4px 0 8px}"
    ".sub{margin:2px 0 14px;color:#34482a}"
    ".band{background:#80a862;border-bottom:3px solid #1c2a14;padding:10px 12px 8px;font:.7rem/1.6 P2,ui-monospace,monospace;overflow-wrap:anywhere}"
    ".band a{color:#1c2a14;text-decoration:none}.band a:hover{text-decoration:underline}"
    ".box{background:#c4e4a0;border:2px solid #1c2a14;border-top:0;padding:10px 12px 12px}"
    ".quick{display:flex;flex-wrap:wrap;gap:6px;margin:0 0 14px}"
    "button,.btn{font:.65rem/1.4 P2,ui-monospace,monospace;background:#1c2a14;color:#a8d47c;border:0;padding:10px 12px 8px;cursor:pointer;text-decoration:none;display:inline-block}"
    ".quick button{background:#c4e4a0;color:#1c2a14;border:2px solid #1c2a14;padding:8px 9px 6px;font-size:.6rem}"
    ".tools{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}"
    ".hint{font-size:.9rem;color:#34482a;margin:0 0 8px}"
    "ul{list-style:none;margin:0;padding:0}"
    "li{display:flex;align-items:center;gap:8px;padding:8px 2px;border-top:1px solid #80a862}"
    "li .n{flex:1;min-width:0;overflow-wrap:anywhere}li .n a{color:#1c2a14;font-weight:700;text-decoration:none}"
    "li .s{color:#34482a;font-size:.85rem;white-space:nowrap}"
    "li .a{display:flex;gap:6px}li .a a,li .a button{padding:7px 8px 5px;font-size:.55rem}"
    "li .a .del{background:#a8321e;color:#fff}"
    ".bar{height:14px;border:2px solid #1c2a14;background:#eef7e2;margin:4px 0 2px}.bar div{height:100%;background:#1c2a14;width:0}"
    "#prog p{margin:6px 0 0;font-size:.9rem}"
    ".drop{outline:3px dashed #1c2a14;outline-offset:-6px}"
    ".note{font-size:.85rem;color:#34482a;margin-top:14px}"
    "input[type=text]{width:100%;box-sizing:border-box;font:inherit;padding:10px;border:2px solid #1c2a14;background:#eef7e2}"
    "</style></head><body><main><h1>TAMANEST</h1>";

static const char PAGE_LOGIN[] =
    "<p class=sub>SD card files</p><div class=band>ACCESS CODE</div><div class=box>"
    "<p class=hint>Type the code shown on TamaNest's WI-FI TRANSFER screen (or scan its QR code).</p>"
    "<form method=get action=/><input type=text name=k inputmode=numeric autocomplete=off maxlength=6>"
    "<p><button type=submit>OPEN</button></p></form></div></main></body></html>";

static const char PAGE_APP[] =
    "<p class=sub>SD card files <span id=free></span></p>"
    "<div class=quick><button data-d=/>ALL</button><button data-d=/roms>ROMS</button><button data-d=/saves>SAVES</button>"
    "<button data-d=/dlc>DLC</button><button data-d=/screenshots>SCREENSHOTS</button><button data-d=/exports>EXPORTS</button>"
    "<button data-d=/update>UPDATE</button></div>"
    "<div class=band id=path></div><div class=box id=box>"
    "<p class=hint id=hint></p>"
    "<div class=tools><label class=btn>UPLOAD FILES<input type=file id=up multiple hidden></label>"
    "<button id=nf>NEW FOLDER</button></div><div id=prog></div><ul id=list></ul></div>"
    "<p class=note>You can also drop files on this page to upload them into the open folder. "
    "Keep TamaNest on its WI-FI TRANSFER screen while you work.</p>"
    "<p class=note>Chrome may call a download insecure. That is because this page uses http: devices on a home "
    "network cannot get the certificate https needs. The file comes straight from your TamaNest, so choose Keep.</p>"
    "</main><script>"
    "var D='/',L=[];function $(i){return document.getElementById(i)}"
    "function E(s){return String(s).replace(/[&<>\"']/g,function(c){return'&#'+c.charCodeAt(0)+';'})}"
    "function F(n){return n>=1073741824?(n/1073741824).toFixed(1)+' GB':n>=1048576?(n/1048576).toFixed(1)+' MB':n>=1024?Math.round(n/1024)+' KB':n+' B'}"
    "function J(d,n){return d=='/'?'/'+n:d+'/'+n}"
    "var H={'/roms':'Your game ROMs (.bin files).','/saves':'TamaNest saves (one per game, plus safety copies). Download them to back them up.',"
    "'/dlc':'DLC files, one folder per game: ID, IDL, PS, 4U, 4UPLUS. Then in the game: quick menu > DLC > ADD FROM SD.',"
    "'/screenshots':'Screenshots (quick menu > SCREENSHOT IN 3S).','/exports':'Exported saves (quick menu > EXPORT SAVE).',"
    "'/update':'Put tamanest.bin here, then on TamaNest: SETTINGS > UPDATE > SD CARD.'};"
    "function load(d){fetch('/api/list?d='+encodeURIComponent(d)).then(function(r){return r.json()}).then(function(j){"
    "if(j.error){alert(j.error);if(d!='/')load('/');return}D=d;L=j.items;"
    "$('free').textContent='- '+F(j.free)+' free of '+F(j.total);"
    "var p=d.split('/').filter(Boolean),h='<a href=# data-d=/>SD CARD</a>',a='';"
    "p.forEach(function(x){a+='/'+x;h+=' / <a href=# data-d=\"'+E(a)+'\">'+E(x.toUpperCase())+'</a>'});$('path').innerHTML=h;"
    "$('hint').textContent=H[d.toLowerCase()]||'';"
    "var it=j.items.slice().sort(function(x,y){return(y.d-x.d)||x.n.toLowerCase().localeCompare(y.n.toLowerCase())}),s='';"
    "if(d!='/')s+='<li><span class=n><a href=# data-d=\"'+E(d.substring(0,d.lastIndexOf('/'))||'/')+'\">.. (up)</a></span></li>';"
    "it.forEach(function(x){var f=J(d,x.n),q=encodeURIComponent(f);"
    "if(x.d)s+='<li><span class=n><a href=# data-d=\"'+E(f)+'\">'+E(x.n)+' /</a></span><span class=a><button class=del data-x=\"'+E(f)+'\">DELETE</button></span></li>';"
    "else s+='<li><span class=n>'+E(x.n)+'</span><span class=s>'+F(x.s)+'</span><span class=a>'"
    "+(/\\.bmp$/i.test(x.n)?'<a class=btn target=_blank href=\"/api/get?inline=1&f='+q+'\">VIEW</a>':'')"
    "+'<a class=btn href=\"/api/get?f='+q+'\" download>DOWNLOAD</a><button class=del data-x=\"'+E(f)+'\">DELETE</button></span></li>'});"
    "if(!it.length)s+='<li><span class=n>(empty folder)</span></li>';$('list').innerHTML=s})"
    ".catch(function(){alert('TamaNest did not answer. Is its WI-FI TRANSFER screen still open?')})}"
    "document.addEventListener('click',function(e){var t=e.target.closest('[data-d],[data-x]');if(!t)return;e.preventDefault();"
    "if(t.dataset.d!==undefined)return load(t.dataset.d);var f=t.dataset.x;"
    "if(!confirm('Delete '+f.substring(f.lastIndexOf('/')+1)+'? This cannot be undone.'))return;"
    "fetch('/api/del?f='+encodeURIComponent(f),{method:'POST'}).then(function(r){return r.json()}).then(function(j){if(j.error)alert(j.error);load(D)})});"
    "$('nf').onclick=function(){var n=prompt('Name of the new folder:');if(!n)return;"
    "fetch('/api/mkdir?d='+encodeURIComponent(J(D,n.trim())),{method:'POST'}).then(function(r){return r.json()}).then(function(j){if(j.error)alert(j.error);load(D)})};"
    "function up(files){files=Array.prototype.slice.call(files);var i=0,P=$('prog');"
    "function next(){if(i>=files.length){setTimeout(function(){P.innerHTML=''},4000);return load(D)}var f=files[i++];"
    "if(L.some(function(x){return!x.d&&x.n.toLowerCase()==f.name.toLowerCase()})&&!confirm(f.name+' is already there. Replace it?'))return next();"
    "P.innerHTML='<p>Uploading '+E(f.name)+' ('+i+' of '+files.length+')</p><div class=bar><div id=b></div></div><p id=pt></p>';"
    "var x=new XMLHttpRequest();x.open('POST','/api/put?f='+encodeURIComponent(J(D,f.name)));"
    "x.upload.onprogress=function(e){if(e.lengthComputable){$('b').style.width=(100*e.loaded/e.total)+'%';$('pt').textContent=F(e.loaded)+' of '+F(e.total)}};"
    "x.onload=function(){var j={};try{j=JSON.parse(x.responseText)}catch(e){}"
    "if(x.status!=200){alert('Could not upload '+f.name+': '+(j.error||x.status));return load(D)}next()};"
    "x.onerror=function(){alert('Upload of '+f.name+' was cut off. Is TamaNest still on its WI-FI TRANSFER screen?');load(D)};"
    "x.send(f)}next()}"
    "$('up').onchange=function(){up(this.files);this.value=''};"
    "var B=document.body;B.addEventListener('dragover',function(e){e.preventDefault();B.classList.add('drop')});"
    "B.addEventListener('dragleave',function(){B.classList.remove('drop')});"
    "B.addEventListener('drop',function(e){e.preventDefault();B.classList.remove('drop');if(e.dataTransfer.files.length)up(e.dataTransfer.files)});"
    "load('/')</script></body></html>";

static esp_err_t h_root(httpd_req_t *r)
{
    st_touch();
    char k[16];
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (qarg(r, "k", k, sizeof k)) {                /* from the QR link or the code form */
        if (s_st.bad_codes < MAX_BAD && !strcmp(k, s_code)) {
            char c[64];
            snprintf(c, sizeof c, "tn=%s; Path=/; HttpOnly; SameSite=Lax", s_code);
            httpd_resp_set_hdr(r, "Set-Cookie", c);
            httpd_resp_set_status(r, "302 Found");
            httpd_resp_set_hdr(r, "Location", "/");   /* the code does not stay in the address bar */
            ESP_LOGI(TAG, "Access code ok: cookie set");
            return httpd_resp_send(r, NULL, 0);
        }
        taskENTER_CRITICAL(&s_mux); s_st.bad_codes++; taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "Wrong access code (%d)", s_st.bad_codes);
    }
    const bool ok = authed(r);                      /* before replying: httpd forgets the request headers then */
    httpd_resp_set_type(r, "text/html");
    httpd_resp_send_chunk(r, PAGE_CSS, HTTPD_RESP_USE_STRLEN);
    if (ok) httpd_resp_send_chunk(r, PAGE_APP, HTTPD_RESP_USE_STRLEN);
    else if (s_st.bad_codes >= MAX_BAD)
        httpd_resp_sendstr_chunk(r, "<div class=band>LOCKED</div><div class=box><p>Too many wrong codes. Close WI-FI TRANSFER "
                                    "on TamaNest and open it again.</p></div></main></body></html>");
    else httpd_resp_send_chunk(r, PAGE_LOGIN, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t h_list(httpd_req_t *r)
{
    if (!authed(r)) return deny(r);
    st_touch();
    char d[256], full[300];
    if (!qarg(r, "d", d, sizeof d)) strcpy(d, "/");
    if (!path_ok(d)) return send_json_err(r, "400 Bad Request", "Not a valid folder name.");
    full_path(full, sizeof full, d);
    DIR *dir = opendir(full);
    if (!dir) return send_json_err(r, "404 Not Found", "That folder is not there any more.");
    uint64_t tot = 0, fr = 0;
    esp_vfs_fat_info(ROOT, &tot, &fr);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    char b[96];
    snprintf(b, sizeof b, "{\"total\":%llu,\"free\":%llu,\"items\":[", (unsigned long long)tot, (unsigned long long)fr);
    httpd_resp_sendstr_chunk(r, b);
    struct dirent *e;
    bool first = true;
    char fp[300 + 260];
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;          /* hidden files (e.g. macOS "._" copies) */
        struct stat st;
        snprintf(fp, sizeof fp, "%s/%s", full, e->d_name);
        bool isdir = e->d_type == DT_DIR;
        long sz = 0;
        if (!isdir && stat(fp, &st) == 0) sz = (long)st.st_size;
        httpd_resp_sendstr_chunk(r, first ? "{\"n\":" : ",{\"n\":");
        first = false;
        send_json_str(r, e->d_name);
        snprintf(b, sizeof b, ",\"s\":%ld,\"d\":%d}", sz, isdir ? 1 : 0);
        httpd_resp_sendstr_chunk(r, b);
    }
    closedir(dir);
    ESP_LOGI(TAG, "Listed a folder");
    httpd_resp_sendstr_chunk(r, "]}");
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* Download. Sent with our own headers so the browser knows the size (progress) - httpd's own replies are chunked. */
static esp_err_t do_get(httpd_req_t *r)
{
    char f[256], full[300], tmp[8];
    if (!qarg(r, "f", f, sizeof f) || !path_ok(f)) return send_json_err(r, "400 Bad Request", "Not a valid file name.");
    full_path(full, sizeof full, f);
    int fd = open(full, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { if (fd >= 0) close(fd); return send_json_err(r, "404 Not Found", "That file is not there any more."); }
    const bool inl = qarg(r, "inline", tmp, sizeof tmp);
    const char *base = strrchr(f, '/') + 1;
    const size_t bl = strlen(base);
    const bool bmp = bl > 4 && !strcasecmp(base + bl - 4, ".bmp");
    char hdr[1000], fn[640];                        /* fn: the name %-encoded (up to 3 characters per byte) */
    size_t k = 0;                                    /* file name for the "save as": RFC 5987 encoded */
    for (const char *c = base; *c && k < sizeof fn - 4; c++) {
        unsigned char u = (unsigned char)*c;
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || strchr("-._~", u)) fn[k++] = (char)u;
        else k += snprintf(fn + k, sizeof fn - k, "%%%02X", u);
    }
    fn[k] = 0;
    int hl = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "Content-Disposition: %s; filename*=UTF-8''%s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      (inl && bmp) ? "image/bmp" : "application/octet-stream", (long)st.st_size, inl ? "inline" : "attachment", fn);
    st_set("SENDING", f, 0, (long)st.st_size, 1);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t res = ESP_OK;
    if (httpd_send(r, hdr, hl) != hl) res = ESP_FAIL;
    long done = 0;
    while (res == ESP_OK && !s_stopping) {
        int n = read(fd, s_buf, FBUF);
        if (n <= 0) break;
        for (int o = 0; o < n;) {
            int w = httpd_send(r, (const char *)s_buf + o, n - o);
            if (w == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (w <= 0) { res = ESP_FAIL; break; }
            o += w;
        }
        done += n;
        st_progress(done);
    }
    close(fd);
    st_set(res == ESP_OK && done == st.st_size ? "SENT" : "FAILED", f, done, (long)st.st_size, 0);
    ESP_LOGI(TAG, "Sent a file (%ld of %ld bytes, %ld KB/s) | internal RAM lowest %u", done, (long)st.st_size,
             (long)(done / ((esp_timer_get_time() - t0) / 1000 + 1)), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    return res;
}

static esp_err_t do_put(httpd_req_t *r)
{
    char f[256], full[300], part[310];
    if (!qarg(r, "f", f, sizeof f) || !path_ok(f) || !strcmp(f, "/")) return send_json_err(r, "400 Bad Request", "Not a valid file name.");
    full_path(full, sizeof full, f);
    snprintf(part, sizeof part, "%s.part", full);
    const long len = (long)r->content_len;
    uint64_t tot = 0, fr = 0;
    esp_vfs_fat_info(ROOT, &tot, &fr);
    if ((uint64_t)len + 256 * 1024 > fr) return send_json_err(r, "507 Insufficient Storage", "The SD card is too full for this file.");
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) return send_json_err(r, "409 Conflict", "A folder has that name.");
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return send_json_err(r, "500 Internal Server Error", "Could not create the file (is the name allowed on the SD card?).");
    st_set("RECEIVING", f, 0, len, 1);
    const int64_t t0 = esp_timer_get_time();
    long got = 0;
    int fill = 0, tries = 0;
    bool ok = true;
    while (got < len) {
        if (s_stopping) { ok = false; break; }
        int want = FBUF - fill;
        if (want > len - got) want = (int)(len - got);
        int n = httpd_req_recv(r, (char *)s_buf + fill, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) { if (++tries < 4) continue; ok = false; break; }
        if (n <= 0) { ok = false; break; }
        tries = 0;
        fill += n; got += n;
        if (fill == FBUF || got == len) {           /* whole sectors from a DMA-capable buffer */
            if (write(fd, s_buf, fill) != fill) { ok = false; break; }
            fill = 0;
            st_progress(got);
        }
    }
    if (close(fd) != 0) ok = false;
    if (ok) {
        unlink(full);                                /* replace an older copy */
        if (rename(part, full) != 0) ok = false;
    }
    if (!ok) unlink(part);
    st_set(ok ? "RECEIVED" : "FAILED", f, got, len, 0);
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "Received a file: %s, %ld bytes in %lld ms (%ld KB/s)", ok ? "ok" : "FAILED", got, ms,
             ms > 0 ? (long)(got / ms) : 0L);
    if (!ok) return send_json_err(r, "500 Internal Server Error", "The upload was cut off or could not be written.");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":1}");
}

static esp_err_t h_font(httpd_req_t *r)            /* the pixel font for the headings (no access code needed) */
{
    httpd_resp_set_type(r, "font/woff2");
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=86400");
    return httpd_resp_send(r, (const char *)FONT_P2_WOFF2, sizeof FONT_P2_WOFF2);
}

static void worker_task(void *arg)
{
    xfer_job_t j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        if (!j.req) break;                          /* file_server_stop() */
        j.fn(j.req);
        httpd_req_async_handler_complete(j.req);
    }
    for (;;) vTaskSuspend(NULL);                    /* deleted by file_server_stop() */
}
static esp_err_t queue_job(httpd_req_t *r, esp_err_t (*fn)(httpd_req_t *))
{
    if (!authed(r)) return deny(r);                 /* checked here: the request headers are still there */
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(r, &copy) != ESP_OK) return send_json_err(r, "500 Internal Server Error", "Busy, please try again.");
    xfer_job_t j = { copy, fn };
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        httpd_req_async_handler_complete(copy);
        return send_json_err(r, "503 Service Unavailable", "TamaNest is busy with other files. Please try again in a moment.");
    }
    return ESP_OK;
}
static esp_err_t h_get(httpd_req_t *r) { return queue_job(r, do_get); }
static esp_err_t h_put(httpd_req_t *r) { return queue_job(r, do_put); }

static esp_err_t h_del(httpd_req_t *r)
{
    if (!authed(r)) return deny(r);
    char f[256], full[300];
    if (!qarg(r, "f", f, sizeof f) || !path_ok(f) || !strcmp(f, "/")) return send_json_err(r, "400 Bad Request", "Not a valid name.");
    full_path(full, sizeof full, f);
    struct stat st;
    if (stat(full, &st) != 0) return send_json_err(r, "404 Not Found", "It is not there any more.");
    if (S_ISDIR(st.st_mode) ? rmdir(full) != 0 : unlink(full) != 0)
        return send_json_err(r, "409 Conflict", S_ISDIR(st.st_mode) ? "Only empty folders can be deleted. Delete what is inside first." : "Could not delete it.");
    st_set("DELETED", f, 0, 0, 0);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":1}");
}

static esp_err_t h_mkdir(httpd_req_t *r)
{
    if (!authed(r)) return deny(r);
    char d[256], full[300];
    if (!qarg(r, "d", d, sizeof d) || !path_ok(d) || !strcmp(d, "/")) return send_json_err(r, "400 Bad Request", "Not a valid folder name.");
    full_path(full, sizeof full, d);
    if (mkdir(full, 0777) != 0)
        return send_json_err(r, "409 Conflict", errno == EEXIST ? "That name is already used." : "Could not make the folder.");
    st_set("NEW FOLDER", d, 0, 0, 0);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":1}");
}

esp_err_t file_server_start(const char *code)
{
    if (s_srv) return ESP_OK;
    memset(&s_st, 0, sizeof s_st);
    snprintf(s_code, sizeof s_code, "%s", code);
    if (!s_buf) s_buf = heap_caps_malloc(FBUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf) return ESP_ERR_NO_MEM;
    s_stopping = false;
    s_jobs = xQueueCreate(4, sizeof(xfer_job_t));  /* made and freed every session (lent memory, see main.c) */
    if (!s_jobs) { heap_caps_free(s_buf); s_buf = NULL; return ESP_ERR_NO_MEM; }
    if (xTaskCreate(worker_task, "file_xfer", 8192, NULL, 5, &s_worker) != pdPASS) {   /* internal stack: SD driver */
        s_worker = NULL; vQueueDelete(s_jobs); s_jobs = NULL; heap_caps_free(s_buf); s_buf = NULL; return ESP_ERR_NO_MEM;
    }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 7;                         /* lwIP has 10; httpd keeps 3 for itself */
    hc.lru_purge_enable = true;
    hc.stack_size = 8192;                            /* internal: it calls the SD card driver */
    hc.max_uri_handlers = 10;
    hc.recv_wait_timeout = 15;
    hc.send_wait_timeout = 15;
    esp_err_t e = httpd_start(&s_srv, &hc);
    if (e != ESP_OK) { s_srv = NULL; file_server_stop(); return e; }
    static const httpd_uri_t U[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/api/list", .method = HTTP_GET, .handler = h_list },
        { .uri = "/api/get", .method = HTTP_GET, .handler = h_get },
        { .uri = "/api/put", .method = HTTP_POST, .handler = h_put },
        { .uri = "/api/del", .method = HTTP_POST, .handler = h_del },
        { .uri = "/api/mkdir", .method = HTTP_POST, .handler = h_mkdir },
        { .uri = "/p2.woff2", .method = HTTP_GET, .handler = h_font },
    };
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++) httpd_register_uri_handler(s_srv, &U[i]);
    ESP_LOGI(TAG, "File server on (internal RAM free %u, largest %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void file_server_stop(void)
{
    s_stopping = true;                               /* a running transfer ends at its next 8 KB */
    if (s_worker) {                                  /* let it finish its request, then park, then delete it */
        xfer_job_t q = { NULL, NULL }, j;
        while (xQueueReceive(s_jobs, &j, 0) == pdTRUE)   /* waiting jobs: give them back to httpd */
            if (j.req) httpd_req_async_handler_complete(j.req);
        xQueueSend(s_jobs, &q, pdMS_TO_TICKS(1000));
        for (int i = 0; i < 300 && eTaskGetState(s_worker) != eSuspended; i++) vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelete(s_worker);
        s_worker = NULL;
    }
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
    if (s_jobs) { vQueueDelete(s_jobs); s_jobs = NULL; }
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
    memset(s_code, 0, sizeof s_code);
    ESP_LOGI(TAG, "File server off");
}
