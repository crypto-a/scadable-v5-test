#include "scadable_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "scadable_client";

#ifndef SCADABLE_API_BASE
#define SCADABLE_API_BASE "http://146.190.253.86:8080"
#endif

#ifndef SCADABLE_API_TOKEN
#define SCADABLE_API_TOKEN "REPLACE_ME_WITH_NAMESPACE_TOKEN"
#endif

#ifndef SCADABLE_FW_VERSION
#define SCADABLE_FW_VERSION "unknown"
#endif

static char s_device_id[40];
static char s_firmware_version[40];
static SemaphoreHandle_t s_mux;

void scadable_client_init(const char *device_id, const char *firmware_version) {
    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    strncpy(s_firmware_version, firmware_version, sizeof(s_firmware_version) - 1);
    s_mux = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "init device=%s fw=%s base=%s", s_device_id, s_firmware_version, SCADABLE_API_BASE);
}

const char *scadable_client_device_id(void) { return s_device_id; }
const char *scadable_client_firmware_version(void) { return s_firmware_version; }

esp_err_t scadable_client_post(const char *path, const char *json_body, size_t body_len) {
    xSemaphoreTake(s_mux, portMAX_DELAY);

    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/ingest%s", SCADABLE_API_BASE, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        xSemaphoreGive(s_mux);
        return ESP_FAIL;
    }

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", SCADABLE_API_TOKEN);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_post_field(cli, json_body, body_len);

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    if (err == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGW(TAG, "POST %s -> %d", path, status);
        err = ESP_FAIL;
    }
    esp_http_client_cleanup(cli);

    xSemaphoreGive(s_mux);
    return err;
}
