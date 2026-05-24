// Wi-Fi provisioning via SoftAP + HTTP form.
//
// When the device has no stored Wi-Fi credentials (first boot after a
// flash, or after the user explicitly cleared them) it brings up a
// SoftAP named "SCADABLE-XXXXXX" where XXXXXX = the last 6 hex chars
// of the AP MAC. An HTTP server at 192.168.4.1 serves a form for
// SSID + password. Submitting the form writes both into NVS and
// returns control to the caller; main.c follows up with an
// esp_restart() to come back up in STA mode.
//
// Design notes:
//   - SoftAP is open (no password) for V1. The credential-entry page
//     is HTTP, not HTTPS — fine on a one-hop link the user is in
//     control of, and avoids the cert-trust UX problem.
//   - Captive portal: we run a tiny DNS hijacker on UDP/53 that
//     resolves every name to 192.168.4.1, and a wildcard HTTP
//     fall-through that serves the provisioning page for any URI.
//     Together these trigger iOS Captive Network Assistant + Android
//     Wi-Fi assistant to auto-pop the portal on join.

#include "provisioning.h"

#include <string.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG  = "provisioning";
static const char *NS   = "scadable_cfg";

#define DONE_BIT BIT0

static EventGroupHandle_t s_done_group;
static httpd_handle_t     s_server;
static TaskHandle_t       s_dns_task;

// ─── NVS helpers ───────────────────────────────────────────────────────

bool provisioning_load_credentials(char *ssid, size_t ssid_n,
                                   char *pass, size_t pass_n) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t s_len = ssid_n;
    size_t p_len = pass_n;
    esp_err_t es = nvs_get_str(h, "wifi_ssid", ssid, &s_len);
    esp_err_t ep = nvs_get_str(h, "wifi_pass", pass, &p_len);
    nvs_close(h);

    if (es != ESP_OK || ep != ESP_OK || s_len <= 1) {
        ssid[0] = '\0';
        pass[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t provisioning_save_credentials(const char *ssid, const char *password) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, "wifi_ssid", ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_pass", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ─── URL form parsing ──────────────────────────────────────────────────

// Decodes one %XX hex escape. Returns the byte value or -1 on bad input.
static int hex_pair(const char *p) {
    int hi, lo;
    if (p[0] >= '0' && p[0] <= '9') hi = p[0] - '0';
    else if (p[0] >= 'a' && p[0] <= 'f') hi = p[0] - 'a' + 10;
    else if (p[0] >= 'A' && p[0] <= 'F') hi = p[0] - 'A' + 10;
    else return -1;
    if (p[1] >= '0' && p[1] <= '9') lo = p[1] - '0';
    else if (p[1] >= 'a' && p[1] <= 'f') lo = p[1] - 'a' + 10;
    else if (p[1] >= 'A' && p[1] <= 'F') lo = p[1] - 'A' + 10;
    else return -1;
    return (hi << 4) | lo;
}

// Extracts one form-urlencoded field from a body and decodes it into
// `out`. Returns true if the key was found.
static bool form_get(const char *body, const char *key, char *out, size_t out_n) {
    size_t key_n = strlen(key);
    const char *p = body;
    while (*p) {
        // Match key at current position followed by '='
        if (strncmp(p, key, key_n) == 0 && p[key_n] == '=') {
            p += key_n + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_n) {
                if (*p == '+') {
                    out[i++] = ' ';
                    p++;
                } else if (*p == '%' && p[1] && p[2]) {
                    int b = hex_pair(p + 1);
                    if (b < 0) return false;
                    out[i++] = (char)b;
                    p += 3;
                } else {
                    out[i++] = *p++;
                }
            }
            out[i] = '\0';
            return true;
        }
        // Advance to next field
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

// ─── HTTP handlers ─────────────────────────────────────────────────────

static const char FORM_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SCADABLE Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#0a0a0a;color:#e8e8e8;"
    "min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px}"
    ".card{width:100%;max-width:360px;background:#121212;border:1px solid #222;border-radius:12px;padding:28px}"
    ".brand{display:flex;align-items:center;gap:8px;margin-bottom:24px}"
    ".dot{width:24px;height:24px;border-radius:6px;background:linear-gradient(135deg,rgba(34,197,94,.3),transparent);"
    "border:1px solid rgba(34,197,94,.4);display:flex;align-items:center;justify-content:center}"
    ".dot i{display:block;width:6px;height:6px;border-radius:50%;background:#22c55e;box-shadow:0 0 8px rgba(34,197,94,.8)}"
    ".brand b{font-size:13px;letter-spacing:.18em}"
    "h1{font-size:16px;margin-bottom:6px}"
    "p{font-size:12px;color:#888;margin-bottom:20px;line-height:1.5}"
    "label{display:block;font-size:11px;text-transform:uppercase;letter-spacing:.1em;color:#666;margin-bottom:6px}"
    "input,select{width:100%;background:#0c0c0c;border:1px solid #222;color:#e8e8e8;padding:10px 12px;border-radius:6px;"
    "font:13px monospace;margin-bottom:12px;appearance:none;-webkit-appearance:none}"
    "input:focus,select:focus{outline:none;border-color:rgba(34,197,94,.4)}"
    ".row{display:flex;align-items:center;gap:8px;margin-bottom:16px}"
    ".row input{margin-bottom:0}"
    ".rescan{flex:0 0 auto;width:auto;padding:8px 10px;font-size:11px;background:#1a1a1a;border-color:#333;cursor:pointer}"
    ".rescan:hover{border-color:rgba(34,197,94,.4);color:#22c55e}"
    ".hint{font-size:10px;color:#555;margin:-8px 0 16px}"
    "button.go{width:100%;background:rgba(34,197,94,.1);border:1px solid rgba(34,197,94,.4);color:#22c55e;"
    "padding:12px;border-radius:6px;font:bold 12px sans-serif;letter-spacing:.1em;text-transform:uppercase;cursor:pointer}"
    "button.go:hover{background:rgba(34,197,94,.2)}"
    "</style></head><body><div class=card>"
    "<div class=brand><span class=dot><i></i></span><b>SCADABLE</b></div>"
    "<h1>Connect to Wi-Fi</h1><p>Pick your network from the list, or type the name manually if it's hidden. The device will join your Wi-Fi and come online in your dashboard.</p>"
    "<form method=post action=/save>"
    "<label>Nearby networks</label>"
    "<div class=row>"
    "<select id=netlist onchange=\"document.getElementsByName('ssid')[0].value=this.value\">"
    "<option value=''>scanning…</option></select>"
    "<button type=button class=rescan onclick=loadScan()>↻</button>"
    "</div>"
    "<label>Network name (SSID)</label>"
    "<input name=ssid required maxlength=32 autofocus placeholder='or type a hidden network'>"
    "<label>Password</label>"
    "<input name=pass type=password maxlength=64 placeholder='leave blank for open networks'>"
    "<button class=go>Connect</button></form>"
    "<script>"
    "function loadScan(){"
    "var s=document.getElementById('netlist');"
    "s.innerHTML='<option value=\"\">scanning…</option>';"
    "fetch('/scan').then(function(r){return r.json()}).then(function(nets){"
    "var seen={};var list=[];"
    "for(var i=0;i<nets.length;i++){var n=nets[i];if(!n.ssid)continue;"
    "if(!seen[n.ssid]||seen[n.ssid].rssi<n.rssi){seen[n.ssid]=n}}"
    "for(var k in seen)list.push(seen[k]);"
    "list.sort(function(a,b){return b.rssi-a.rssi});"
    "s.innerHTML='<option value=\"\">— pick one —</option>';"
    "for(var j=0;j<list.length;j++){"
    "var o=document.createElement('option');o.value=list[j].ssid;"
    "var lock=list[j].open?'':' 🔒';"
    "o.textContent=list[j].ssid+lock+' ('+list[j].rssi+' dBm)';"
    "s.appendChild(o)}"
    "if(!list.length){s.innerHTML='<option value=\"\">(no networks found)</option>'}"
    "}).catch(function(){s.innerHTML='<option value=\"\">(scan failed — type below)</option>'})}"
    "loadScan();"
    "</script>"
    "</div></body></html>";

static const char SAVED_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SCADABLE</title>"
    "<style>body{font-family:-apple-system,system-ui,sans-serif;background:#0a0a0a;color:#e8e8e8;"
    "min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;text-align:center}"
    ".card{max-width:340px;background:#121212;border:1px solid #222;border-radius:12px;padding:28px}"
    "h1{font-size:16px;margin-bottom:10px;color:#22c55e}p{font-size:12px;color:#888;line-height:1.5}"
    "</style></head><body><div class=card><h1>✓ Saved</h1>"
    "<p>Your device is rebooting and joining your Wi-Fi. It should show up in your SCADABLE dashboard in about 30 seconds. You can close this tab.</p>"
    "</div></body></html>";

static esp_err_t form_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, FORM_PAGE);
}

// /scan — synchronous Wi-Fi scan, returns a JSON array of nearby APs.
// The form's JS calls this to populate the network dropdown. We dedupe
// by SSID and sort by RSSI on the client side. APSTA mode is required
// — see provisioning_run_until_credentials below.
static esp_err_t scan_handler(httpd_req_t *req) {
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 250 } },
    };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;

    wifi_ap_record_t *recs = NULL;
    if (n > 0) {
        recs = calloc(n, sizeof(wifi_ap_record_t));
        if (!recs) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        esp_wifi_scan_get_ap_records(&n, recs);
    }

    // Each AP entry is ~80 bytes of JSON max → 30 × 100 = 3000. 4 KB safe.
    char *buf = malloc(4096);
    if (!buf) {
        free(recs);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int pos = 0;
    pos += snprintf(buf + pos, 4096 - pos, "[");
    for (uint16_t i = 0; i < n; i++) {
        // Escape backslashes + double-quotes in the SSID — the rest of
        // ASCII control chars are unusual in SSIDs, but the JSON RFC
        // requires escaping if present.
        char ssid_esc[68];
        const char *src = (const char *)recs[i].ssid;
        int e = 0;
        for (int s = 0; src[s] && e + 2 < (int)sizeof(ssid_esc); s++) {
            if (src[s] == '"' || src[s] == '\\') {
                ssid_esc[e++] = '\\';
            }
            ssid_esc[e++] = src[s];
        }
        ssid_esc[e] = '\0';

        bool open = (recs[i].authmode == WIFI_AUTH_OPEN);
        int n_written = snprintf(buf + pos, 4096 - pos,
                                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                                 i ? "," : "", ssid_esc, recs[i].rssi,
                                 open ? "true" : "false");
        if (n_written <= 0 || n_written >= 4096 - pos) {
            break; // ran out of buffer; ship what we have
        }
        pos += n_written;
    }
    if (pos < 4096 - 1) {
        pos += snprintf(buf + pos, 4096 - pos, "]");
    }
    free(recs);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, pos);
    free(buf);
    return err;
}

// Common save path used by both POST /save and GET /save (iOS Captive
// Network Assistant has been observed to convert form POSTs to GETs).
// `payload` is form-urlencoded (e.g. "ssid=Foo&pass=bar"); `len` is
// its length.
static esp_err_t do_save(httpd_req_t *req, const char *payload, int len) {
    ESP_LOGI(TAG, "save request (len=%d): '%.*s'", len, len > 80 ? 80 : len, payload);

    char ssid[64] = {0};
    char pass[96] = {0};
    if (!form_get(payload, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        ESP_LOGW(TAG, "save: ssid missing or empty");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid is required");
    }
    form_get(payload, "pass", pass, sizeof(pass)); // optional for open networks

    esp_err_t err = provisioning_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
    }
    ESP_LOGI(TAG, "saved credentials for SSID '%s' (pass=%d chars)", ssid, (int)strlen(pass));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, SAVED_PAGE);

    // Let the browser finish receiving the response before we tear
    // down the AP and reboot into STA mode.
    vTaskDelay(pdMS_TO_TICKS(500));
    xEventGroupSetBits(s_done_group, DONE_BIT);
    return ESP_OK;
}

static esp_err_t form_post_handler(httpd_req_t *req) {
    if (req->content_len > 512) {
        ESP_LOGW(TAG, "POST /save payload too large: %d bytes", (int)req->content_len);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
    }
    char body[513] = {0};
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n <= 0) {
            ESP_LOGW(TAG, "POST /save recv failed at %d/%d", total, (int)req->content_len);
            return ESP_FAIL;
        }
        total += n;
    }
    body[total] = '\0';
    return do_save(req, body, total);
}

// GET /save — same as POST but with credentials in the query string.
// Registered because iOS's Captive Network Assistant rewrites form
// POSTs to GETs when it's the browser. Also fires if someone uses
// curl with -G.
static esp_err_t form_get_save_handler(httpd_req_t *req) {
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) {
        ESP_LOGW(TAG, "GET /save: no query string");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    }
    if (qlen > 512) {
        ESP_LOGW(TAG, "GET /save: query too long (%d bytes)", (int)qlen);
        return httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "query too long");
    }
    char query[513] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        ESP_LOGW(TAG, "GET /save: get_url_query_str failed");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
    }
    return do_save(req, query, (int)qlen);
}

// captive_404_handler is invoked by the HTTP server when an incoming
// URI doesn't match any registered handler. Apple's Captive Network
// Assistant probes http://captive.apple.com/hotspot-detect.html on
// Wi-Fi join and expects a plain "Success" body; anything else
// triggers the portal popup. Android probes /generate_204 and
// expects a 204. Serving the provisioning HTML with status 200 here
// makes both platforms pop the portal automatically the moment the
// user joins the SCADABLE-XXXXXX network.
static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, FORM_PAGE);
}

// ─── DNS hijacker ──────────────────────────────────────────────────────
//
// The captive-portal popup only works if the OS's probe request
// actually reaches us — which only happens if the DNS lookup for
// (e.g.) captive.apple.com resolves to our AP. We bind UDP/53 and
// reply to every A query with 192.168.4.1.
//
// The reply is built by patching the original query: copy the
// transaction ID + question section, set the response bit + RA,
// bump ANCOUNT to 1, and append a 16-byte answer using a DNS
// compression pointer to refer back to the question name. That keeps
// us under ~80 LOC instead of pulling in a full resolver.

static void dns_hijack_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "dns hijacker up on udp/53 (all queries → 192.168.4.1)");

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
        if (n < 12) continue;            // shorter than a DNS header
        if (n + 16 > (int)sizeof(buf)) continue;  // refuse oversized queries

        // Flip header → response. Leave transaction ID (buf[0..1]),
        // RD bit, and QDCOUNT (buf[4..5]) intact.
        buf[2] = 0x81;  // QR=1, RD copied
        buf[3] = 0x80;  // RA=1, RCODE=0
        buf[6] = 0; buf[7] = 1;   // ANCOUNT=1
        buf[8] = 0; buf[9] = 0;   // NSCOUNT=0
        buf[10] = 0; buf[11] = 0; // ARCOUNT=0

        // Append answer: name=pointer-to-offset-12, type=A, class=IN,
        // TTL=60, rdlength=4, rdata=192.168.4.1.
        uint8_t *a = buf + n;
        a[0]  = 0xc0; a[1]  = 0x0c;
        a[2]  = 0x00; a[3]  = 0x01;
        a[4]  = 0x00; a[5]  = 0x01;
        a[6]  = 0x00; a[7]  = 0x00; a[8] = 0x00; a[9] = 60;
        a[10] = 0x00; a[11] = 0x04;
        a[12] = 192; a[13] = 168; a[14] = 4; a[15] = 1;

        sendto(sock, buf, n + 16, 0, (struct sockaddr *)&src, srclen);
    }
}

static void start_dns_hijacker(void) {
    if (s_dns_task) return;
    xTaskCreate(dns_hijack_task, "dns_hijack", 3072, NULL, 3, &s_dns_task);
}

static void stop_dns_hijacker(void) {
    if (s_dns_task) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
}

// ─── Server bring-up ───────────────────────────────────────────────────

static void start_http_server(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // Slightly larger backlog so a flurry of captive-portal probes
    // (iOS often fires three or four in parallel) doesn't get refused.
    cfg.max_open_sockets = 7;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start http server");
        return;
    }
    httpd_uri_t get_root  = { .uri = "/",     .method = HTTP_GET,  .handler = form_get_handler };
    httpd_uri_t post_save = { .uri = "/save", .method = HTTP_POST, .handler = form_post_handler };
    httpd_uri_t get_save  = { .uri = "/save", .method = HTTP_GET,  .handler = form_get_save_handler };
    httpd_uri_t get_scan  = { .uri = "/scan", .method = HTTP_GET,  .handler = scan_handler };
    httpd_register_uri_handler(s_server, &get_root);
    httpd_register_uri_handler(s_server, &post_save);
    httpd_register_uri_handler(s_server, &get_save);
    httpd_register_uri_handler(s_server, &get_scan);

    // Wildcard fall-through so captive-portal probes (any host, any
    // path) get the provisioning page instead of a 404.
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404_handler);

    ESP_LOGI(TAG, "http portal up on http://192.168.4.1/  (GET /, POST/GET /save, GET /scan, * → form)");
}

static void stop_http_server(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

// ─── Public entry point ────────────────────────────────────────────────

esp_err_t provisioning_run_until_credentials(void) {
    s_done_group = xEventGroupCreate();

    // We may be called BEFORE wifi.c has touched the radio (first
    // boot, no creds yet) OR AFTER it gave up trying to join. Either
    // way the init sequence is idempotent.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    // APSTA mode: AP for the portal, STA for scanning nearby networks.
    // The STA interface is created but never configured to join a
    // specific SSID — we only use it via esp_wifi_scan_start().
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    // Build the SSID from the AP MAC suffix.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "SCADABLE-%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len       = strlen(ssid);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
    ap_cfg.ap.channel        = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "softAP up: SSID=%s", ssid);

    start_http_server();
    start_dns_hijacker();

    // Block until form_post_handler signals completion.
    xEventGroupWaitBits(s_done_group, DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    stop_dns_hijacker();
    stop_http_server();
    esp_wifi_stop();
    return ESP_OK;
}
