#include "log_sink.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"
#include "scadable_client.h"

#define RING_SIZE              (16 * 1024)
#define FLUSH_HIGH_WATERMARK   ((RING_SIZE * 3) / 4)

static char s_ring[RING_SIZE];
static size_t s_ring_head = 0;
static SemaphoreHandle_t s_ring_mux = NULL;
static SemaphoreHandle_t s_flush_signal = NULL;
static vprintf_like_t s_prev_vprintf = NULL;
static volatile bool s_in_sink = false;

static int sink_vprintf(const char *fmt, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int forwarded = s_prev_vprintf ? s_prev_vprintf(fmt, ap_copy) : 0;
    va_end(ap_copy);

    if (s_in_sink) return forwarded;
    s_in_sink = true;

    char line[512];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0) {
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        if (xSemaphoreTake(s_ring_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
            size_t avail = RING_SIZE - s_ring_head;
            if ((size_t)n > avail) n = (int)avail;
            if (n > 0) {
                memcpy(s_ring + s_ring_head, line, n);
                s_ring_head += n;
            }
            bool need_flush = s_ring_head >= FLUSH_HIGH_WATERMARK;
            xSemaphoreGive(s_ring_mux);
            if (need_flush && s_flush_signal) xSemaphoreGive(s_flush_signal);
        }
    }

    s_in_sink = false;
    return forwarded;
}

void log_sink_install(void) {
    s_ring_mux = xSemaphoreCreateMutex();
    s_flush_signal = xSemaphoreCreateBinary();
    s_prev_vprintf = esp_log_set_vprintf(sink_vprintf);
}

static void add_slice_string(cJSON *parent, const char *key, const char *src, size_t n) {
    if (n > 480) n = 480;
    char tmp[512];
    memcpy(tmp, src, n);
    tmp[n] = '\0';
    cJSON_AddStringToObject(parent, key, tmp);
}

static void parse_line_into(cJSON *arr, const char *line, size_t len) {
    if (len == 0) return;
    char level = line[0];
    if (level != 'I' && level != 'W' && level != 'E' && level != 'D' && level != 'V') {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "level", "I");
        cJSON_AddStringToObject(e, "tag", "raw");
        add_slice_string(e, "message", line, len);
        cJSON_AddNumberToObject(e, "uptime_ms", (double)(esp_timer_get_time() / 1000));
        cJSON_AddItemToArray(arr, e);
        return;
    }
    const char *p = line + 1;
    size_t remaining = len - 1;
    if (remaining < 4 || p[0] != ' ' || p[1] != '(') return;
    p += 2; remaining -= 2;
    const char *paren_close = memchr(p, ')', remaining);
    if (!paren_close) return;
    long ts_ms = strtol(p, NULL, 10);
    remaining -= (paren_close + 1 - p);
    p = paren_close + 1;
    if (remaining < 2 || p[0] != ' ') return;
    p++; remaining--;
    const char *colon = memchr(p, ':', remaining);
    if (!colon) return;
    size_t tag_len = colon - p;
    const char *msg_p = colon + 1;
    size_t msg_remaining = remaining - (msg_p - p);
    while (msg_remaining > 0 && (*msg_p == ' ' || *msg_p == '\t')) { msg_p++; msg_remaining--; }

    cJSON *e = cJSON_CreateObject();
    char level_str[2] = {level, 0};
    cJSON_AddStringToObject(e, "level", level_str);
    add_slice_string(e, "tag", p, tag_len);
    add_slice_string(e, "message", msg_p, msg_remaining);
    cJSON_AddNumberToObject(e, "uptime_ms", (double)ts_ms);
    cJSON_AddItemToArray(arr, e);
}

typedef struct { char device_id[40]; } flush_ctx_t;

static void flush_task(void *arg) {
    flush_ctx_t *ctx = (flush_ctx_t *)arg;
    static char snapshot[RING_SIZE];
    while (1) {
        // Wait up to the user-configured log flush interval; high-watermark
        // can signal earlier if the buffer fills up.
        xSemaphoreTake(s_flush_signal, pdMS_TO_TICKS(config_log_flush_interval_ms()));

        size_t snap_len = 0;
        if (xSemaphoreTake(s_ring_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
            snap_len = s_ring_head;
            if (snap_len > 0) {
                memcpy(snapshot, s_ring, snap_len);
                s_ring_head = 0;
            }
            xSemaphoreGive(s_ring_mux);
        }
        if (snap_len == 0) continue;

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device_id", ctx->device_id);
        cJSON_AddStringToObject(root, "firmware_sha", scadable_client_firmware_version());
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "logs", arr);

        const char *cursor = snapshot;
        size_t left = snap_len;
        while (left > 0) {
            const char *nl = memchr(cursor, '\n', left);
            size_t line_len = nl ? (size_t)(nl - cursor) : left;
            size_t effective = line_len;
            while (effective > 0 && (cursor[effective - 1] == '\r')) effective--;
            if (effective > 0) parse_line_into(arr, cursor, effective);
            if (!nl) break;
            size_t advance = (size_t)(nl - cursor) + 1;
            cursor += advance;
            left -= advance;
        }

        char *body = cJSON_PrintUnformatted(root);
        if (body) {
            scadable_client_post("/logs", body, strlen(body));
            free(body);
        }
        cJSON_Delete(root);
    }
}

void log_sink_start_flush_task(const char *device_id) {
    flush_ctx_t *ctx = calloc(1, sizeof(flush_ctx_t));
    strncpy(ctx->device_id, device_id, sizeof(ctx->device_id) - 1);
    xTaskCreate(flush_task, "log_flush", 6144, ctx, 3, NULL);
}
