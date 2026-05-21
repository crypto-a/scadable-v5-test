// SCADABLE runtime config.
//
// Two knobs that the dashboard can flip live without re-flashing:
//   heartbeat_interval_ms    how often the heartbeat task publishes
//   log_flush_interval_ms    how often log_sink ships buffered logs
//
// Values arrive as a JSON payload on
//   scadable/devices/{id}/cmd/config
// with QoS 1 + retain=true, so a reconnecting device picks up the
// latest config automatically.
//
// Values are persisted to NVS namespace "scadable_cfg" so reboots keep
// the user's last setting.

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";
static const char *NS  = "scadable_cfg";

#define DEFAULT_HEARTBEAT_MS  5000
#define DEFAULT_LOG_FLUSH_MS  30000

#define MIN_HEARTBEAT_MS      1000
#define MAX_HEARTBEAT_MS      300000
#define MIN_LOG_FLUSH_MS      5000
#define MAX_LOG_FLUSH_MS      600000

static int s_heartbeat_ms = DEFAULT_HEARTBEAT_MS;
static int s_log_flush_ms = DEFAULT_LOG_FLUSH_MS;

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void config_init(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "hb_ms",  &v) == ESP_OK) s_heartbeat_ms = clamp_int(v, MIN_HEARTBEAT_MS, MAX_HEARTBEAT_MS);
        if (nvs_get_i32(h, "log_ms", &v) == ESP_OK) s_log_flush_ms = clamp_int(v, MIN_LOG_FLUSH_MS, MAX_LOG_FLUSH_MS);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "loaded hb=%dms log_flush=%dms", s_heartbeat_ms, s_log_flush_ms);
}

void config_apply(const char *json_payload, size_t len) {
    if (!json_payload || len == 0) return;
    cJSON *root = cJSON_ParseWithLength(json_payload, len);
    if (!root) {
        ESP_LOGW(TAG, "invalid config json");
        return;
    }
    int new_hb  = s_heartbeat_ms;
    int new_log = s_log_flush_ms;

    cJSON *hb_j  = cJSON_GetObjectItem(root, "heartbeat_interval_ms");
    cJSON *log_j = cJSON_GetObjectItem(root, "log_flush_interval_ms");
    if (cJSON_IsNumber(hb_j))  new_hb  = clamp_int(hb_j->valueint,  MIN_HEARTBEAT_MS, MAX_HEARTBEAT_MS);
    if (cJSON_IsNumber(log_j)) new_log = clamp_int(log_j->valueint, MIN_LOG_FLUSH_MS, MAX_LOG_FLUSH_MS);

    cJSON_Delete(root);

    if (new_hb == s_heartbeat_ms && new_log == s_log_flush_ms) {
        ESP_LOGI(TAG, "config unchanged");
        return;
    }

    s_heartbeat_ms = new_hb;
    s_log_flush_ms = new_log;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "hb_ms",  s_heartbeat_ms);
        nvs_set_i32(h, "log_ms", s_log_flush_ms);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "applied hb=%dms log_flush=%dms", s_heartbeat_ms, s_log_flush_ms);
}

int config_heartbeat_interval_ms(void) { return s_heartbeat_ms; }
int config_log_flush_interval_ms(void) { return s_log_flush_ms; }
