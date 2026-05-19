#include "heartbeat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt.h"

static const char *TAG = "heartbeat";

typedef struct {
    char device_id[32];
    char firmware_version[32];
} ctx_t;

static void iso_timestamp(char *out, size_t n) {
    // Without NTP we don't have wall time; use boot-relative seconds via
    // an ISO-like format prefixed with "boot+". The streamer treats this as
    // a string; wall-clock alignment is out of scope for V1.
    int64_t us = esp_timer_get_time();
    int64_t secs = us / 1000000;
    snprintf(out, n, "boot+%lld.%03lds", (long long)secs, (long)((us / 1000) % 1000));
}

static void heartbeat_task(void *arg) {
    ctx_t *ctx = (ctx_t *)arg;
    char topic[96];
    snprintf(topic, sizeof(topic), "scadable/%s/heartbeat", ctx->device_id);

    while (1) {
        if (mqtt_is_connected()) {
            char ts[64];
            iso_timestamp(ts, sizeof(ts));

            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "device_id", ctx->device_id);
            cJSON_AddStringToObject(root, "timestamp", ts);
            cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
            cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());
            cJSON_AddStringToObject(root, "firmware_version", ctx->firmware_version);

            char *payload = cJSON_PrintUnformatted(root);
            if (payload) {
                int r = mqtt_publish(topic, payload, strlen(payload));
                if (r < 0) {
                    ESP_LOGW(TAG, "publish failed");
                }
                free(payload);
            }
            cJSON_Delete(root);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void heartbeat_start(const char *device_id, const char *firmware_version) {
    ctx_t *ctx = calloc(1, sizeof(ctx_t));
    strncpy(ctx->device_id, device_id, sizeof(ctx->device_id) - 1);
    strncpy(ctx->firmware_version, firmware_version, sizeof(ctx->firmware_version) - 1);
    xTaskCreate(heartbeat_task, "heartbeat", 4096, ctx, 4, NULL);
}
