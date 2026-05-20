#include "heartbeat.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "scadable_client.h"

static const char *TAG = "heartbeat";

static void heartbeat_task(void *arg) {
    while (1) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device_id", scadable_client_device_id());
        cJSON_AddStringToObject(root, "firmware_sha", scadable_client_firmware_version());
        cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
        cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());

        char ts[64];
        int64_t us = esp_timer_get_time();
        snprintf(ts, sizeof(ts), "boot+%lld.%03lds",
                 (long long)(us / 1000000), (long)((us / 1000) % 1000));
        cJSON_AddStringToObject(root, "timestamp", ts);

        char *body = cJSON_PrintUnformatted(root);
        if (body) {
            esp_err_t err = scadable_client_post("/heartbeat", body, strlen(body));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "heartbeat post failed");
            }
            free(body);
        }
        cJSON_Delete(root);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void heartbeat_start(void) {
    xTaskCreate(heartbeat_task, "heartbeat", 6144, NULL, 4, NULL);
}
