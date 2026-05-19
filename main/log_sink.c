#include "log_sink.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mqtt.h"

#define RING_SIZE     (16 * 1024)
#define FLUSH_HIGH_WATERMARK ((RING_SIZE * 3) / 4)
#define FLUSH_INTERVAL_MS    60000

static const char *TAG_INTERNAL = "log_sink";

static char s_ring[RING_SIZE];
static size_t s_ring_head = 0;
static SemaphoreHandle_t s_ring_mux = NULL;
static SemaphoreHandle_t s_flush_signal = NULL;
static vprintf_like_t s_prev_vprintf = NULL;
static volatile bool s_in_sink = false;  // re-entrancy guard

static int sink_vprintf(const char *fmt, va_list ap) {
    // Forward to the previous (UART) sink first so the serial console still works.
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int forwarded = s_prev_vprintf ? s_prev_vprintf(fmt, ap_copy) : 0;
    va_end(ap_copy);

    if (s_in_sink) {
        // Re-entered from inside our own machinery; do not capture.
        return forwarded;
    }
    s_in_sink = true;

    char line[512];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0) {
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        if (xSemaphoreTake(s_ring_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
            size_t free_space = RING_SIZE - s_ring_head;
            if ((size_t)n > free_space) {
                n = (int)free_space;  // drop overflow; will flush soon
            }
            if (n > 0) {
                memcpy(s_ring + s_ring_head, line, n);
                s_ring_head += n;
            }
            bool need_flush = s_ring_head >= FLUSH_HIGH_WATERMARK;
            xSemaphoreGive(s_ring_mux);
            if (need_flush && s_flush_signal) {
                xSemaphoreGive(s_flush_signal);
            }
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

// Find first occurrence of any char in `chars` from `start`, up to len bytes.
static const char *find_any(const char *start, size_t len, const char *chars) {
    for (size_t i = 0; i < len; i++) {
        for (const char *c = chars; *c; c++) {
            if (start[i] == *c) return start + i;
        }
    }
    return NULL;
}

// Add a JSON string field copied from a non-NUL-terminated slice of length `n`.
static void add_slice_string(cJSON *parent, const char *key, const char *src, size_t n) {
    if (n > 480) n = 480;  // cap individual fields
    char tmp[512];
    memcpy(tmp, src, n);
    tmp[n] = '\0';
    cJSON_AddStringToObject(parent, key, tmp);
}

// Parse one ESP_LOG line of the form "X (12345) tag: message" into pieces.
// Adds an entry into `arr` when parse succeeds.
static void parse_line_into(cJSON *arr, const char *line, size_t len) {
    if (len == 0) return;
    char level = line[0];
    if (level != 'I' && level != 'W' && level != 'E' && level != 'D' && level != 'V') {
        // Not a recognized log line — store it raw at info level so it isn't lost.
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "level", "I");
        cJSON_AddStringToObject(entry, "tag", "raw");
        add_slice_string(entry, "message", line, len);
        cJSON_AddNumberToObject(entry, "uptime_ms", (double)(esp_timer_get_time() / 1000));
        cJSON_AddItemToArray(arr, entry);
        return;
    }

    const char *p = line + 1;
    size_t remaining = len - 1;

    // Expect " (" then digits ")"
    if (remaining < 4 || p[0] != ' ' || p[1] != '(') return;
    p += 2; remaining -= 2;
    const char *paren_close = memchr(p, ')', remaining);
    if (!paren_close) return;
    long ts_ms = strtol(p, NULL, 10);
    remaining -= (paren_close + 1 - p);
    p = paren_close + 1;
    if (remaining < 2 || p[0] != ' ') return;
    p++; remaining--;

    // tag is up to ':'
    const char *colon = memchr(p, ':', remaining);
    if (!colon) return;
    size_t tag_len = colon - p;

    const char *msg_p = colon + 1;
    size_t msg_remaining = remaining - (msg_p - p);
    while (msg_remaining > 0 && (*msg_p == ' ' || *msg_p == '\t')) {
        msg_p++; msg_remaining--;
    }

    cJSON *entry = cJSON_CreateObject();
    char level_str[2] = {level, 0};
    cJSON_AddStringToObject(entry, "level", level_str);
    add_slice_string(entry, "tag", p, tag_len);
    add_slice_string(entry, "message", msg_p, msg_remaining);
    cJSON_AddNumberToObject(entry, "uptime_ms", (double)ts_ms);
    cJSON_AddItemToArray(arr, entry);
}

typedef struct {
    char device_id[32];
} flush_ctx_t;

static void flush_task(void *arg) {
    flush_ctx_t *ctx = (flush_ctx_t *)arg;
    char topic[96];
    snprintf(topic, sizeof(topic), "scadable/%s/logs", ctx->device_id);

    static char snapshot[RING_SIZE];
    while (1) {
        // Wait for either the periodic timer or a high-watermark signal.
        xSemaphoreTake(s_flush_signal, pdMS_TO_TICKS(FLUSH_INTERVAL_MS));

        if (!mqtt_is_connected()) {
            // Don't drain if we can't ship; let the buffer fill up to its cap.
            continue;
        }

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

        // Split snapshot into lines and parse each one.
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device_id", ctx->device_id);
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "logs", arr);

        const char *cursor = snapshot;
        size_t left = snap_len;
        while (left > 0) {
            const char *nl = memchr(cursor, '\n', left);
            size_t line_len = nl ? (size_t)(nl - cursor) : left;
            // strip trailing \r
            size_t effective = line_len;
            while (effective > 0 && (cursor[effective - 1] == '\r')) effective--;
            if (effective > 0) {
                parse_line_into(arr, cursor, effective);
            }
            if (!nl) break;
            size_t advance = (size_t)(nl - cursor) + 1;
            cursor += advance;
            left -= advance;
        }

        char *payload = cJSON_PrintUnformatted(root);
        if (payload) {
            int r = mqtt_publish(topic, payload, strlen(payload));
            if (r < 0) {
                // best-effort logging without recursion — go to UART only
                if (s_prev_vprintf) {
                    char msg[64];
                    int m = snprintf(msg, sizeof(msg), "[%s] log batch publish failed\n", TAG_INTERNAL);
                    (void)m;
                    // can't pass varargs safely; skip
                }
            }
            free(payload);
        }
        cJSON_Delete(root);
    }
}

void log_sink_start_flush_task(const char *device_id) {
    flush_ctx_t *ctx = calloc(1, sizeof(flush_ctx_t));
    strncpy(ctx->device_id, device_id, sizeof(ctx->device_id) - 1);
    xTaskCreate(flush_task, "log_flush", 6144, ctx, 3, NULL);
}
