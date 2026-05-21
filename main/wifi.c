#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "provisioning.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// We give the STA path this many consecutive disconnect attempts
// before declaring the credentials bad and falling back into
// provisioning mode. ~30s total at the doubling backoff.
#define MAX_STA_RETRIES 5

static EventGroupHandle_t s_event_group;
static int  s_backoff_ms     = 1000;
static int  s_retries        = 0;
static const int s_backoff_max_ms = 30 * 1000;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retries++;
        if (s_retries >= MAX_STA_RETRIES) {
            ESP_LOGW(TAG, "STA gave up after %d tries — falling back to provisioning", s_retries);
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            return;
        }
        ESP_LOGW(TAG, "disconnected (retry %d/%d); next attempt in %d ms",
                 s_retries, MAX_STA_RETRIES, s_backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));
        s_backoff_ms = (s_backoff_ms * 2 < s_backoff_max_ms) ? s_backoff_ms * 2 : s_backoff_max_ms;
        esp_wifi_connect();
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_backoff_ms = 1000;
        s_retries    = 0;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

// Try to join the network described by (ssid, password). Returns true
// on success (got an IP), false if we exhausted retries.
static bool try_sta(const char *ssid, const char *password) {
    s_event_group = xEventGroupCreate();
    s_backoff_ms  = 1000;
    s_retries     = 0;

    // esp_netif_init / event_loop_create may have been called already
    // by provisioning_run_until_credentials on a previous boot path.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init_cfg);

    esp_event_handler_instance_t any_wifi, got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &any_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &got_ip);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid,     sizeof(cfg.sta.ssid)     - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // accept any auth mode the AP advertises
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "associating with '%s'", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_connect_or_provision(void) {
    char ssid[64], pass[96];

    // First-boot path: no creds in NVS → straight into provisioning.
    if (!provisioning_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "no stored credentials — entering provisioning mode");
        ESP_ERROR_CHECK(provisioning_run_until_credentials());
        ESP_LOGI(TAG, "credentials submitted — restarting");
        esp_restart();
    }

    // Normal path: try to join. If we run out of retries, the creds
    // are probably stale (user changed Wi-Fi password, moved, etc.).
    // Clear them and reboot into provisioning so the user can correct.
    ESP_LOGI(TAG, "stored SSID: '%s'", ssid);
    if (try_sta(ssid, pass)) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA failed — clearing creds and restarting into provisioning");
    provisioning_save_credentials("", "");
    esp_restart();
    return ESP_FAIL; // unreachable
}

// Legacy entry point — kept for source compatibility. Routes to the
// new credential-aware flow; arguments are ignored.
esp_err_t wifi_start_and_wait(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    return wifi_connect_or_provision();
}
