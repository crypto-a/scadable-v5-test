#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "heartbeat.h"
#include "log_sink.h"
#include "mqtt.h"
#include "ota.h"
#include "wifi.h"

static const char *TAG = "app";

// Hardcoded for V1 prototype. Move to provisioning later.
#define WIFI_SSID     "Velocity"
#define WIFI_PASSWORD "furtherfaster"

#define MDNS_TARGET_HOST "scadable"  // resolves scadable.local
#define MDNS_RESOLVE_TIMEOUT_MS 5000
#define MDNS_RETRY_LIMIT 6

#ifndef SCADABLE_FW_VERSION
#define SCADABLE_FW_VERSION "unknown"
#endif

// SCADABLE_BROKER_HOST is set at compile time (via -DSCADABLE_BROKER_HOST=...
// or env var picked up in CMakeLists). When set, the firmware skips mDNS and
// connects directly to that host. Use this to point at a remote VM.
#ifndef SCADABLE_BROKER_HOST
#define SCADABLE_BROKER_HOST ""
#endif

#ifndef SCADABLE_BROKER_PORT
#define SCADABLE_BROKER_PORT 1883
#endif

static void format_device_id(char *out, size_t n) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, n, "esp32_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t resolve_broker(char *uri_out, size_t n) {
    // Hardcoded broker (e.g. a remote VM) wins over mDNS when set.
    if (SCADABLE_BROKER_HOST[0] != '\0') {
        snprintf(uri_out, n, "mqtt://%s:%d", SCADABLE_BROKER_HOST, SCADABLE_BROKER_PORT);
        ESP_LOGI(TAG, "broker pinned by build flag: %s", uri_out);
        return ESP_OK;
    }

    ESP_ERROR_CHECK(mdns_init());

    esp_ip4_addr_t addr = {0};
    for (int i = 0; i < MDNS_RETRY_LIMIT; i++) {
        addr.addr = 0;
        esp_err_t err = mdns_query_a(MDNS_TARGET_HOST, MDNS_RESOLVE_TIMEOUT_MS, &addr);
        if (err == ESP_OK && addr.addr != 0) {
            snprintf(uri_out, n, "mqtt://" IPSTR ":%d", IP2STR(&addr), SCADABLE_BROKER_PORT);
            ESP_LOGI(TAG, "broker resolved via mDNS: %s", uri_out);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "mDNS resolve attempt %d failed (err=0x%x); retrying", i + 1, err);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return ESP_FAIL;
}

void app_main(void) {
    // NVS is required by Wi-Fi to persist calibration data.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Install the log sink BEFORE we generate interesting log traffic.
    log_sink_install();

    char device_id[32];
    format_device_id(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "device_id=%s firmware_version=%s", device_id, SCADABLE_FW_VERSION);

    ESP_ERROR_CHECK(wifi_start_and_wait(WIFI_SSID, WIFI_PASSWORD));

    char broker_uri[64];
    if (resolve_broker(broker_uri, sizeof(broker_uri)) != ESP_OK) {
        ESP_LOGE(TAG, "could not resolve scadable.local via mDNS; halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    ESP_ERROR_CHECK(mqtt_start(broker_uri));

    log_sink_start_flush_task(device_id);
    heartbeat_start(device_id, SCADABLE_FW_VERSION);
    ota_start(device_id);

    // Dummy task: tick every 3s, checkpoint warning every 30s.
    static int counter = 0;
    while (1) {
        ESP_LOGI("tick", "tick %d", counter);
        counter++;
        if ((counter % 10) == 0) {
            ESP_LOGW("checkpoint", "checkpoint reached");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
