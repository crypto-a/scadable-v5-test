// SCADABLE OTA receiver.
//
// Subscribes (via scadable_client.c) to scadable/devices/{id}/cmd/ota.
// On a command, downloads the firmware via esp_https_ota and reboots
// into the new image. Status events are published back on
// scadable/devices/{id}/ota_status so the dashboard can show progress
// in real time:
//
//   dispatched  ← set by backend when /api/devices/{id}/ota is called
//   started     ← device received the command, beginning download
//   applied     ← write to new partition succeeded, about to reboot
//   failed      ← something went wrong, device stayed on old firmware
//
// We use the embedded ESP-IDF certificate bundle so HTTPS works for
// Let's Encrypt-issued URLs. HTTP URLs (e.g. MinIO public endpoint)
// also work; the "https" in esp_https_ota is historical.

#include "ota.h"
#include "scadable_client.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

typedef struct {
    char url[256];
    char deployment_id[64];
} ota_args_t;

static void publish_status(const char *deployment_id, const char *status, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "deployment_id", deployment_id);
    cJSON_AddStringToObject(root, "status", status);
    if (msg) cJSON_AddStringToObject(root, "message", msg);
    char *body = cJSON_PrintUnformatted(root);
    if (body) {
        scadable_client_post("/ota_status", body, strlen(body));
        free(body);
    }
    cJSON_Delete(root);
}

static void ota_task(void *arg) {
    ota_args_t *args = (ota_args_t *)arg;
    ESP_LOGI(TAG, "starting OTA from %s (deployment=%s)", args->url, args->deployment_id);
    publish_status(args->deployment_id, "started", NULL);

    esp_http_client_config_t http_cfg = {
        .url                       = args->url,
        .crt_bundle_attach         = esp_crt_bundle_attach,
        .timeout_ms                = 30000,
        .keep_alive_enable         = true,
        .skip_cert_common_name_check = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting");
        publish_status(args->deployment_id, "applied", "rebooting");
        vTaskDelay(pdMS_TO_TICKS(800));  // let the publish actually flush
        esp_restart();
    }

    char errbuf[128];
    snprintf(errbuf, sizeof(errbuf), "%s", esp_err_to_name(err));
    ESP_LOGE(TAG, "OTA failed: %s", errbuf);
    publish_status(args->deployment_id, "failed", errbuf);

    free(args);
    vTaskDelete(NULL);
}

void ota_dispatch(const char *json_payload, size_t len) {
    cJSON *root = cJSON_ParseWithLength(json_payload, len);
    if (!root) {
        ESP_LOGE(TAG, "OTA command: invalid JSON");
        return;
    }
    cJSON *url_j = cJSON_GetObjectItem(root, "url");
    cJSON *dep_j = cJSON_GetObjectItem(root, "deployment_id");
    if (!cJSON_IsString(url_j) || url_j->valuestring == NULL) {
        ESP_LOGE(TAG, "OTA command: missing url");
        cJSON_Delete(root);
        return;
    }

    ota_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        cJSON_Delete(root);
        return;
    }
    strncpy(args->url, url_j->valuestring, sizeof(args->url) - 1);
    if (cJSON_IsString(dep_j) && dep_j->valuestring) {
        strncpy(args->deployment_id, dep_j->valuestring, sizeof(args->deployment_id) - 1);
    }
    cJSON_Delete(root);

    BaseType_t ok = xTaskCreate(ota_task, "scadable_ota", 8192, args, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn OTA task");
        publish_status(args->deployment_id, "failed", "task create failed");
        free(args);
    }
}
