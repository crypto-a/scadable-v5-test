// SCADABLE on-device diagnostic tests.
//
// Four starter "local" tests live here. Each is a small C function
// returning pass/fail with a message. The dashboard kicks them off via
// scadable/devices/{id}/cmd/test; results come back on
// scadable/devices/{id}/test_result.
//
// To add a test:
//   1. Write a static scadable_test_result_t fn(void) here.
//   2. Add a row to s_tests[] below.
//   3. Add the same id + description to .scadable/tests.yml so the
//      backend (and dashboard) know about it at build time.
//
// Keep tests CHEAP — they run on the device, no spawning extra tasks,
// no blocking on network. If you need a slow check, queue it.

#include "scadable_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "scadable_client.h"

static const char *TAG = "scadable_test";

// ─── Test functions ────────────────────────────────────────────────────

static scadable_test_result_t test_heap_available(void) {
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < 30000) {
        static char buf[64];
        snprintf(buf, sizeof(buf), "free heap %u B below 30 KB threshold", (unsigned)free_heap);
        return scadable_test_fail(buf);
    }
    static char buf[64];
    snprintf(buf, sizeof(buf), "free heap %u B", (unsigned)free_heap);
    return (scadable_test_result_t){true, buf};
}

static scadable_test_result_t test_wifi_connected(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return scadable_test_fail("not associated with any AP");
    }
    static char buf[96];
    snprintf(buf, sizeof(buf), "associated to '%.*s' rssi=%d",
             (int)sizeof(ap.ssid), (const char *)ap.ssid, ap.rssi);
    return (scadable_test_result_t){true, buf};
}

static scadable_test_result_t test_self_diagnostic(void) {
    // Quick scratch-memory R/W sanity check.
    uint8_t buf[128];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i ^ 0x5A);
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != (uint8_t)(i ^ 0x5A)) {
            return scadable_test_fail("scratch memory R/W mismatch");
        }
    }
    return scadable_test_pass();
}

// ─── Registry ──────────────────────────────────────────────────────────

static const scadable_test_t s_tests[] = {
    {"heap_available",  "Free heap is above 30 KB",          test_heap_available},
    {"wifi_connected",  "Device is associated to its AP",    test_wifi_connected},
    {"self_diagnostic", "Scratch-memory read/write sanity",  test_self_diagnostic},
};
static const size_t s_tests_count = sizeof(s_tests) / sizeof(s_tests[0]);

const scadable_test_t *scadable_tests_list(size_t *count_out) {
    if (count_out) *count_out = s_tests_count;
    return s_tests;
}

scadable_test_result_t scadable_test_pass(void) {
    return (scadable_test_result_t){true, "ok"};
}

scadable_test_result_t scadable_test_fail(const char *msg) {
    return (scadable_test_result_t){false, msg ? msg : "fail"};
}

// ─── Result publishing ─────────────────────────────────────────────────

static void publish_result(const char *test_id, const char *run_id,
                            scadable_test_result_t r, int64_t started_us) {
    int64_t duration_us = esp_timer_get_time() - started_us;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "test_id", test_id);
    if (run_id && *run_id) cJSON_AddStringToObject(root, "run_id", run_id);
    cJSON_AddStringToObject(root, "status",  r.ok ? "pass" : "fail");
    cJSON_AddStringToObject(root, "message", r.message ? r.message : "");
    cJSON_AddNumberToObject(root, "duration_ms", duration_us / 1000);

    char *body = cJSON_PrintUnformatted(root);
    if (body) {
        scadable_client_post("/test_result", body, strlen(body));
        free(body);
    }
    cJSON_Delete(root);
}

// ─── Dispatch ──────────────────────────────────────────────────────────

typedef struct {
    char test_id[64];
    char run_id[64];
    bool run_all;
} dispatch_args_t;

static void dispatch_task(void *arg) {
    dispatch_args_t *a = (dispatch_args_t *)arg;
    if (a->run_all) {
        ESP_LOGI(TAG, "running all %u tests (run_id=%s)",
                 (unsigned)s_tests_count, a->run_id);
        for (size_t i = 0; i < s_tests_count; i++) {
            int64_t start = esp_timer_get_time();
            scadable_test_result_t r = s_tests[i].fn();
            publish_result(s_tests[i].id, a->run_id, r, start);
        }
    } else {
        ESP_LOGI(TAG, "running test '%s' (run_id=%s)", a->test_id, a->run_id);
        scadable_test_result_t r = scadable_test_fail("test not found");
        int64_t start = esp_timer_get_time();
        for (size_t i = 0; i < s_tests_count; i++) {
            if (strcmp(s_tests[i].id, a->test_id) == 0) {
                r = s_tests[i].fn();
                break;
            }
        }
        publish_result(a->test_id, a->run_id, r, start);
    }
    free(a);
    vTaskDelete(NULL);
}

void scadable_tests_dispatch(const char *json_payload, size_t len) {
    if (!json_payload || len == 0) return;
    cJSON *root = cJSON_ParseWithLength(json_payload, len);
    if (!root) {
        ESP_LOGW(TAG, "invalid test dispatch json");
        return;
    }
    dispatch_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        cJSON_Delete(root);
        return;
    }
    cJSON *id_j  = cJSON_GetObjectItem(root, "test_id");
    cJSON *run_j = cJSON_GetObjectItem(root, "run_id");
    cJSON *all_j = cJSON_GetObjectItem(root, "run_all");
    if (cJSON_IsString(id_j)  && id_j->valuestring)  strncpy(args->test_id, id_j->valuestring,  sizeof(args->test_id)  - 1);
    if (cJSON_IsString(run_j) && run_j->valuestring) strncpy(args->run_id,  run_j->valuestring, sizeof(args->run_id)   - 1);
    args->run_all = cJSON_IsTrue(all_j);
    cJSON_Delete(root);

    if (xTaskCreate(dispatch_task, "scadable_test_run", 6144, args, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn test task");
        free(args);
    }
}
