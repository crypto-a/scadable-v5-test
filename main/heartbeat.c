// SCADABLE heartbeat task.
//
// Publishes a single JSON document every 5 seconds with the device's
// vital signs. The dashboard graphs these over time. Each sample is
// engineered to be small (~200 bytes) so 5s cadence is essentially
// free on a typical wifi link.
//
// Fields:
//   device_id           — stable per-device identifier (esp32_<mac>)
//   firmware_sha        — short git SHA baked into the build
//   uptime_seconds      — seconds since boot
//   client_ts_ms        — device's view of UTC time in ms (0 if NTP
//                         hasn't synced yet); used by the backend to
//                         compute end-to-end MQTT latency
//   free_heap_bytes     — current free heap
//   min_free_heap_bytes — lowest free heap seen since boot
//   cpu_pct             — % CPU used since last heartbeat (computed
//                         from FreeRTOS idle task runtime delta)
//   flash_total_bytes   — total flash size on the chip
//   tx_bytes / rx_bytes — cumulative MQTT bytes since boot

#include "heartbeat.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "scadable_client.h"

static const char *TAG = "heartbeat";

// Snapshot the FreeRTOS scheduler counters between heartbeats so we
// can report a meaningful "CPU % since last sample". With idle on both
// cores tracked, cpu_pct goes 0 (fully idle) → 100 (saturated).
static uint64_t prev_total = 0;
static uint64_t prev_idle  = 0;

// Returns 0..100 representing CPU usage averaged across cores since
// the last call. Allocates a small TaskStatus_t array on the heap
// and frees it before returning, so it does not retain memory.
static int sample_cpu_pct(void) {
    const UBaseType_t max_tasks = 24;
    TaskStatus_t *tasks = pvPortMalloc(sizeof(TaskStatus_t) * max_tasks);
    if (!tasks) return -1;

    uint32_t total_runtime32 = 0;
    UBaseType_t n = uxTaskGetSystemState(tasks, max_tasks, &total_runtime32);

    uint64_t idle_now = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        const char *name = tasks[i].pcTaskName;
        // ESP32 dual-core: IDLE0 + IDLE1. Other ports just have IDLE.
        if (strncmp(name, "IDLE", 4) == 0) {
            idle_now += tasks[i].ulRunTimeCounter;
        }
    }
    uint64_t total_now = (uint64_t)total_runtime32;

    vPortFree(tasks);

    if (prev_total == 0) {
        // First call — no baseline yet.
        prev_total = total_now;
        prev_idle  = idle_now;
        return 0;
    }
    uint64_t dt = total_now - prev_total;
    uint64_t di = idle_now  - prev_idle;
    prev_total = total_now;
    prev_idle  = idle_now;

    if (dt == 0) return 0;
    if (di > dt) di = dt;          // clamp against counter quirks
    // Multiply by 2 because total_runtime is summed across both cores
    // on dual-core ESP32, so idle covers 2× the wall-clock budget.
    // The result is "fraction of total core-seconds used" expressed
    // as a percentage 0..100.
    uint64_t used = dt - di;
    return (int)((used * 100ULL) / dt);
}

// Wall-clock ms since unix epoch, or 0 if the device hasn't yet
// completed an SNTP sync. The backend treats 0 as "unknown" and
// suppresses latency computation for that sample.
static int64_t client_ts_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    // Before SNTP sync the kernel clock is at boot epoch (~1970).
    if (tv.tv_sec < 946684800LL) return 0;  // 2000-01-01
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

static uint32_t cached_flash_size(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    if (esp_flash_get_size(NULL, &cached) != ESP_OK) cached = 0;
    return cached;
}

static void heartbeat_task(void *arg) {
    while (1) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device_id",         scadable_client_device_id());
        cJSON_AddStringToObject(root, "firmware_sha",      scadable_client_firmware_version());
        cJSON_AddNumberToObject(root, "uptime_seconds",    esp_timer_get_time() / 1000000);
        cJSON_AddNumberToObject(root, "free_heap_bytes",   esp_get_free_heap_size());
        cJSON_AddNumberToObject(root, "min_free_heap_bytes", esp_get_minimum_free_heap_size());

        // CPU usage % since previous heartbeat — capped 0..100.
        int cpu = sample_cpu_pct();
        if (cpu < 0) cpu = 0;
        if (cpu > 100) cpu = 100;
        cJSON_AddNumberToObject(root, "cpu_pct", cpu);

        // Flash + network counters.
        cJSON_AddNumberToObject(root, "flash_total_bytes", cached_flash_size());
        cJSON_AddNumberToObject(root, "tx_bytes",          (double)scadable_client_tx_bytes());
        cJSON_AddNumberToObject(root, "rx_bytes",          (double)scadable_client_rx_bytes());

        // Wall-clock for round-trip latency on the server side.
        cJSON_AddNumberToObject(root, "client_ts_ms",      (double)client_ts_ms());

        // Boot-relative timestamp kept for backwards compatibility
        // with the existing dashboard. Cheap to keep.
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

        // Re-read the configured interval every iteration so a cmd/config
        // update takes effect on the very next tick.
        vTaskDelay(pdMS_TO_TICKS(config_heartbeat_interval_ms()));
    }
}

void heartbeat_start(void) {
    xTaskCreate(heartbeat_task, "heartbeat", 6144, NULL, 4, NULL);
}
