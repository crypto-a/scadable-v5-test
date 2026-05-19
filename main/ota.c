#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "mqtt.h"

static const char *TAG = "ota";

typedef struct {
    char version[64];
    char url[256];
} ota_request_t;

static char s_device_id[40];
static char s_command_topic[96];
static char s_status_topic[96];
static QueueHandle_t s_queue = NULL;

static void publish_status(const char *version, const char *state, const char *details) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", version);
    cJSON_AddStringToObject(root, "state", state);
    if (details && details[0]) cJSON_AddStringToObject(root, "details", details);

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        mqtt_publish(s_status_topic, payload, strlen(payload));
        free(payload);
    }
    cJSON_Delete(root);
}

static void do_ota(const ota_request_t *req) {
    ESP_LOGI(TAG, "starting OTA version=%s url=%s", req->version, req->url);
    publish_status(req->version, "downloading", NULL);

    esp_http_client_config_t http_cfg = {
        .url = req->url,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        publish_status(req->version, "failed", esp_err_to_name(err));
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "ota image size %d bytes", total);

    int last_reported_pct = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(handle);
        int pct = total > 0 ? (int)((int64_t)read * 100 / total) : 0;
        if ((pct / 10) != (last_reported_pct / 10)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d%% (%d/%d)", pct, read, total);
            publish_status(req->version, "progress", buf);
            last_reported_pct = pct;
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "ota perform failed: %s", esp_err_to_name(err));
        publish_status(req->version, "failed", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_finish failed: %s", esp_err_to_name(err));
        publish_status(req->version, "failed", esp_err_to_name(err));
        return;
    }

    publish_status(req->version, "success", "restarting");
    ESP_LOGI(TAG, "OTA success; restarting in 1s");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void ota_task(void *arg) {
    ota_request_t req;
    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            do_ota(&req);
        }
    }
}

static void on_mqtt_message(const char *topic, const char *data, int data_len) {
    if (strcmp(topic, s_command_topic) != 0) return;

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) {
        ESP_LOGW(TAG, "ota command: invalid JSON");
        return;
    }
    const cJSON *version = cJSON_GetObjectItem(root, "version");
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(version) || !cJSON_IsString(url)) {
        ESP_LOGW(TAG, "ota command: missing version/url");
        cJSON_Delete(root);
        return;
    }
    ota_request_t req = {0};
    strncpy(req.version, version->valuestring, sizeof(req.version) - 1);
    strncpy(req.url, url->valuestring, sizeof(req.url) - 1);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "ota command received: %s", req.version);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ota queue full; dropping command");
    }
}

void ota_start(const char *device_id) {
    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    snprintf(s_command_topic, sizeof(s_command_topic), "scadable/%s/ota/command", s_device_id);
    snprintf(s_status_topic, sizeof(s_status_topic), "scadable/%s/ota/status", s_device_id);

    s_queue = xQueueCreate(2, sizeof(ota_request_t));
    mqtt_set_message_handler(on_mqtt_message);
    mqtt_subscribe(s_command_topic);

    // On boot, validate the running image. esp_ota_mark_app_valid_cancel_rollback
    // tells the bootloader "this image is good"; otherwise a fresh OTA would
    // roll back to the previous slot on the next reset.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "marked running image as valid");
        }
    }

    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
}
