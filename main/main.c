#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config.h"
#include "heartbeat.h"
#include "log_sink.h"
#include "scadable_client.h"
#include "wifi.h"

static const char *TAG = "app";

#ifndef SCADABLE_FW_VERSION
#define SCADABLE_FW_VERSION "unknown"
#endif

static void format_device_id(char *out, size_t n) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, n, "esp32_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    log_sink_install();
    config_init();

    char device_id[40];
    format_device_id(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "boot · device=%s · firmware=%s · rollout-test", device_id, SCADABLE_FW_VERSION);

    // wifi_connect_or_provision reads creds from NVS. On first boot
    // (no creds) or after repeated STA failures it brings up a
    // SoftAP + HTTP portal so the user can enter Wi-Fi creds from
    // their phone, then reboots into STA mode. Doesn't return until
    // we have an IP.
    ESP_ERROR_CHECK(wifi_connect_or_provision());

    // Sync wall-clock via SNTP so heartbeats can carry a real UTC
    // timestamp for the backend to compute end-to-end latency. We
    // start it and move on — the heartbeat task tolerates an
    // unsynced clock by emitting client_ts_ms=0.
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    scadable_client_init(device_id, SCADABLE_FW_VERSION);
    log_sink_start_flush_task(device_id);
    heartbeat_start();

    // Liveness ticker so the dashboard sees a steady log stream.
    int counter = 0;
    while (1) {
        ESP_LOGI("tick", "tick %d", counter++);
        if ((counter % 10) == 0) {
            ESP_LOGW("checkpoint", "checkpoint reached at tick %d", counter);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
