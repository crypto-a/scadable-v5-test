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
//   - We don't run a captive-portal DNS hijacker. The user just types
//     192.168.4.1 in their browser. Adding one is ~80 LOC and worth
//     it later.

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
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG  = "provisioning";
static const char *NS   = "scadable_cfg";

#define DONE_BIT BIT0

static EventGroupHandle_t s_done_group;
static httpd_handle_t     s_server;

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
    "input{width:100%;background:#0c0c0c;border:1px solid #222;color:#e8e8e8;padding:10px 12px;border-radius:6px;"
    "font:13px monospace;margin-bottom:16px}"
    "input:focus{outline:none;border-color:rgba(34,197,94,.4)}"
    "button{width:100%;background:rgba(34,197,94,.1);border:1px solid rgba(34,197,94,.4);color:#22c55e;"
    "padding:12px;border-radius:6px;font:bold 12px sans-serif;letter-spacing:.1em;text-transform:uppercase;cursor:pointer}"
    "button:hover{background:rgba(34,197,94,.2)}"
    "</style></head><body><div class=card>"
    "<div class=brand><span class=dot><i></i></span><b>SCADABLE</b></div>"
    "<h1>Connect to Wi-Fi</h1><p>Enter your network credentials. The device will join your Wi-Fi and come online in your dashboard.</p>"
    "<form method=post action=/save>"
    "<label>Network name (SSID)</label><input name=ssid required maxlength=32 autofocus>"
    "<label>Password</label><input name=pass type=password maxlength=64>"
    "<button>Connect</button></form></div></body></html>";

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

static esp_err_t form_post_handler(httpd_req_t *req) {
    if (req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
    }
    char body[513] = {0};
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n <= 0) return ESP_FAIL;
        total += n;
    }
    body[total] = '\0';

    char ssid[64] = {0};
    char pass[96] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid is required");
    }
    form_get(body, "pass", pass, sizeof(pass)); // password is optional (open networks)

    esp_err_t err = provisioning_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
    }
    ESP_LOGI(TAG, "saved credentials for SSID '%s' (pass=%d chars)", ssid, (int)strlen(pass));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, SAVED_PAGE);

    // Signal the main task that we're done. Give the browser a beat
    // to receive the response before we tear down the AP.
    vTaskDelay(pdMS_TO_TICKS(500));
    xEventGroupSetBits(s_done_group, DONE_BIT);
    return ESP_OK;
}

// ─── Server bring-up ───────────────────────────────────────────────────

static void start_http_server(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start http server");
        return;
    }
    httpd_uri_t get  = { .uri = "/",     .method = HTTP_GET,  .handler = form_get_handler };
    httpd_uri_t post = { .uri = "/save", .method = HTTP_POST, .handler = form_post_handler };
    httpd_register_uri_handler(s_server, &get);
    httpd_register_uri_handler(s_server, &post);
    ESP_LOGI(TAG, "http portal up on http://192.168.4.1/");
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

    esp_netif_create_default_wifi_ap();

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "softAP up: SSID=%s", ssid);

    start_http_server();

    // Block until form_post_handler signals completion.
    xEventGroupWaitBits(s_done_group, DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    stop_http_server();
    esp_wifi_stop();
    return ESP_OK;
}
