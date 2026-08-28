#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "router_config.h"
#include "wifi_config.h"
#include "http_server.h"

extern esp_netif_t *wifiAP;
extern esp_netif_t *wifiSTA;
extern void router_reconnect_uplink(void);
extern void router_apply_ap_config(void);

static volatile bool dns_started = false;
static const char *TAG = "http_server";

/* Deliberately plain HTML: no external files, images, CSS framework, or heavy UI. */
static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-S3 NAT Router</title></head><body>"
"<h2>ESP32-S3 NAT Router</h2>"
"<h3>Status</h3><pre id='status'>Loading...</pre>"
"<h3>Wi-Fi Uplink</h3>"
"<button type='button' onclick='scan()'>Scan Wi-Fi</button><div id='scan'></div>"
"<form onsubmit='connectWifi(event)'>"
"<p><input id='ssid' maxlength='32' placeholder='SSID (hidden Wi-Fi can be entered manually)'></p>"
"<p><input id='pass' type='password' maxlength='64' placeholder='Password'></p>"
"<button>Connect / Save Uplink</button></form><p id='wmsg'></p>"
"<h3>Access Point</h3>"
"<form onsubmit='saveAP(event)'>"
"<p><input id='apssid' maxlength='32' placeholder='AP SSID'></p>"
"<p><input id='appass' type='password' maxlength='64' placeholder='AP password (8+ chars, or blank)'></p>"
"<button>Save AP</button></form><p id='amsg'></p>"
"<script>"
"const $=id=>document.getElementById(id);"
"async function api(u,o){const r=await fetch(u,o);return r.json()}"
"async function load(){try{const x=await api('/api/status');$('status').textContent="Uplink: "+x.uplink+"\nIP: "+x.ip+"\nRSSI: "+x.rssi+" dBm\nUptime: "+x.uptime+"\nDownloaded: "+x.rx+"\nUploaded: "+x.tx+"\nAP clients: "+x.clients+"\nAP SSID: "+x.ap_ssid;$('apssid').value=x.ap_ssid}catch(e){$('status').textContent='Status unavailable'}}"
"async function scan(){ $('scan').textContent='Scanning...';try{const x=await api('/api/scan');$('scan').innerHTML=x.networks.map(n=>'<p><button type=button data-s='+encodeURIComponent(n.ssid)+'>Use</button> '+esc(n.ssid)+' ('+n.rssi+' dBm)</p>').join('')||'No networks found';document.querySelectorAll('[data-s]').forEach(b=>b.onclick=()=>{$('ssid').value=decodeURIComponent(b.dataset.s);$('pass').focus()})}catch(e){$('scan').textContent='Scan failed'}}"
"async function connectWifi(e){e.preventDefault();const b=new URLSearchParams();b.set('ssid',$('ssid').value);b.set('pass',$('pass').value);try{const x=await api('/api/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});$('wmsg').textContent=x.message;setTimeout(load,1000)}catch(e){$('wmsg').textContent='Connection request failed'}}"
"async function saveAP(e){e.preventDefault();const b=new URLSearchParams();b.set('ssid',$('apssid').value);b.set('pass',$('appass').value);try{const x=await api('/api/ap',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});$('amsg').textContent=x.message}catch(e){$('amsg').textContent='AP update failed'}}"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;')}load();setInterval(load,3000);</script></body></html>";

static void json_error(httpd_req_t *req, int code, const char *msg)
{
    char out[192];
    snprintf(out, sizeof(out), "{\"ok\":false,\"message\":\"%s\"}", msg);
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    char uptime[32];
    format_uptime(get_uptime_seconds(), uptime, sizeof(uptime));

    char ipbuf[16] = "-";
    if (ap_connect) {
        ip4addr_ntoa_r((const ip4_addr_t *)&my_ip, ipbuf, sizeof(ipbuf));
    }

    char apbuf[65];
    size_t w = 0;
    for (size_t i = 0; ap_ssid[i] && w + 2 < sizeof(apbuf); ++i) {
        if (ap_ssid[i] == '\\' || ap_ssid[i] == '"') apbuf[w++] = '\\';
        apbuf[w++] = ap_ssid[i];
    }
    apbuf[w] = '\0';

    char out[700];
    snprintf(out, sizeof(out),
             "{\"uplink\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"uptime\":\"%s\",\"rx\":\"%.2f MB\",\"tx\":\"%.2f MB\",\"clients\":%u,\"ap_ssid\":\"%s\"}",
             ap_connect ? "Connected" : "Disconnected", ipbuf, rssi, uptime,
             (double)get_sta_bytes_received() / 1048576.0,
             (double)get_sta_bytes_sent() / 1048576.0,
             connect_count, apbuf);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static bool read_form_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    int len = req->content_len;
    if (len <= 0 || len > 256 || out_len < 2) return false;

    char body[257];
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) return false;
        got += n;
    }
    body[len] = '\0';

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    char *p = strstr(body, pattern);
    if (!p) return false;
    p += strlen(pattern);

    char *e = strchr(p, '&');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= out_len) n = out_len - 1;

    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < out_len; ++i) {
        if (p[i] == '+') {
            out[w++] = ' ';
        } else if (p[i] == '%' && i + 2 < n) {
            unsigned v = 0;
            if (sscanf(p + i + 1, "%2x", &v) == 1) {
                out[w++] = (char)v;
                i += 2;
            } else {
                out[w++] = p[i];
            }
        } else {
            out[w++] = p[i];
        }
    }
    out[w] = '\0';
    return true;
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    char s[33], p[65];
    if (!read_form_value(req, "ssid", s, sizeof(s)) ||
        !read_form_value(req, "pass", p, sizeof(p))) {
        json_error(req, 400, "SSID and password are required");
        return ESP_OK;
    }

    esp_err_t err = wifi_config_save_sta(s, p);
    if (err != ESP_OK) {
        json_error(req, 400, "Invalid Wi-Fi settings");
        return ESP_OK;
    }

    router_reconnect_uplink();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Uplink saved; connecting...\"}");
}

static esp_err_t ap_handler(httpd_req_t *req)
{
    char s[33], p[65];
    if (!read_form_value(req, "ssid", s, sizeof(s)) ||
        !read_form_value(req, "pass", p, sizeof(p))) {
        json_error(req, 400, "AP SSID and password are required");
        return ESP_OK;
    }

    esp_err_t err = wifi_config_save_ap(s, p);
    if (err != ESP_OK) {
        json_error(req, 400, "AP password must be empty or at least 8 characters");
        return ESP_OK;
    }

    router_apply_ap_config();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"AP settings saved\"}");
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    if (wifi_scan_active) {
        json_error(req, 400, "Scan already running");
        return ESP_OK;
    }

    wifi_scan_active = true;
    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 250;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        wifi_scan_active = false;
        json_error(req, 500, "Wi-Fi scan failed");
        return ESP_OK;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 32) count = 32;

    wifi_ap_record_t *list = count ? calloc(count, sizeof(*list)) : NULL;
    if (count && !list) {
        wifi_scan_active = false;
        json_error(req, 500, "Out of memory");
        return ESP_OK;
    }

    if (count) {
        esp_wifi_scan_get_ap_records(&count, list);
    }

    char *out = malloc(4096);
    if (!out) {
        free(list);
        wifi_scan_active = false;
        json_error(req, 500, "Out of memory");
        return ESP_OK;
    }

    size_t pos = 0;
    size_t written = snprintf(out, 4096, "{\"networks\":[");
    if (written >= 4096) written = 4095;
    pos = written;

    bool first = true;
    for (uint16_t i = 0; i < count && pos < 3900; ++i) {
        if (list[i].ssid[0] == 0) continue;

        char esc[65];
        size_t ew = 0;
        for (size_t j = 0; j < sizeof(list[i].ssid) && list[i].ssid[j] && ew + 2 < sizeof(esc); ++j) {
            char c = (char)list[i].ssid[j];
            if (c == '\\' || c == '"') esc[ew++] = '\\';
            esc[ew++] = c;
        }
        esc[ew] = '\0';

        int n = snprintf(out + pos, 4096 - pos, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                         first ? "" : ",", esc, list[i].rssi);
        if (n < 0 || (size_t)n >= 4096 - pos) break;
        pos += (size_t)n;
        first = false;
    }

    snprintf(out + pos, 4096 - pos, "]}");
    free(list);
    wifi_scan_active = false;

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        dns_started = false;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        dns_started = false;
        vTaskDelete(NULL);
        return;
    }

    while (!ap_connect) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &client_len);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;
        if (buf[4] != 0 || buf[5] != 1) continue;
        if (n > (int)sizeof(buf) - 16) continue;

        /* Captive DNS: resolve every requested name to the router itself. */
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0;
        buf[7] = 1;
        int pos = n;
        buf[pos++] = 0xC0;
        buf[pos++] = 0x0C;
        buf[pos++] = 0;
        buf[pos++] = 1;
        buf[pos++] = 0;
        buf[pos++] = 1;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 30;
        buf[pos++] = 0;
        buf[pos++] = 4;
        memcpy(buf + pos, &my_ap_ip, 4);
        pos += 4;
        sendto(sock, buf, pos, 0, (struct sockaddr *)&client, client_len);
    }

    close(sock);
    dns_started = false;
    vTaskDelete(NULL);
}

void captive_portal_start(void)
{
    if (ap_connect || dns_started) return;
    dns_started = true;
    if (xTaskCreate(dns_task, "captive_dns", 2048, NULL, 3, NULL) != pdPASS) {
        dns_started = false;
    }
}

httpd_handle_t start_webserver(uint16_t port)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.max_uri_handlers = 6;
    cfg.stack_size = 4096;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) return NULL;

    httpd_uri_t u = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler
    };
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/status";
    u.handler = status_handler;
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/scan";
    u.handler = scan_handler;
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/connect";
    u.method = HTTP_POST;
    u.handler = connect_handler;
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/ap";
    u.method = HTTP_POST;
    u.handler = ap_handler;
    httpd_register_uri_handler(server, &u);

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "HTTP server started on port %u", (unsigned)port);
    return server;
}
