// SCADABLE MQTT client.
//
// Replaces the v5 HTTP ingest path with MQTT. Keeps the same external API
// (scadable_client_post(path, body, len)) so heartbeat.c and log_sink.c
// don't need to change — internally we publish to:
//
//   scadable/devices/{device_id}{path}
//
// e.g. scadable_client_post("/heartbeat", ...) → topic
//      scadable/devices/esp32_abc123/heartbeat
//
// TLS verification uses the ESP-IDF embedded certificate bundle which
// already trusts Let's Encrypt roots, so no extra cert provisioning on
// the device side.

#include "scadable_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "config.h"
#include "edge_route.h"
#include "ota.h"
#include "scadable_test.h"

static const char *TAG = "scadable_client";

#ifndef SCADABLE_MQTT_URI
#define SCADABLE_MQTT_URI "mqtts://mqtt-yyz.scadable.com:8883"
#endif

#ifndef SCADABLE_FW_VERSION
#define SCADABLE_FW_VERSION "unknown"
#endif

#define PREFIX_BUF_LEN    96
#define TOPIC_BUF_LEN     160
#define URI_BUF_LEN       128
#define CONNECTED_BIT     BIT0

static char s_device_id[40];
static char s_firmware_version[40];
static char s_topic_prefix[PREFIX_BUF_LEN];
static char s_status_topic[TOPIC_BUF_LEN];
static char s_cmd_sub_topic[TOPIC_BUF_LEN]; // scadable/devices/{id}/cmd/#

// Live MQTT URI. Seeded at init from /v1/route (falling back to the
// compile-time default if the edge is unreachable on first boot), and
// updated by the route-maintainer task whenever the platform tells us
// to move to a different broker.
static char                      s_current_uri[URI_BUF_LEN] = SCADABLE_MQTT_URI;
static esp_mqtt_client_handle_t  s_mqtt        = NULL;
static EventGroupHandle_t        s_evt         = NULL;
static SemaphoreHandle_t         s_pubmux      = NULL;
static TaskHandle_t              s_route_task  = NULL;

// Cumulative byte counters. Approximate — we count topic + payload at
// the MQTT envelope level which excludes the ~2-byte protocol header
// per message. Good enough to show the dashboard a throughput trend.
static uint64_t s_tx_bytes = 0;
static uint64_t s_rx_bytes = 0;

uint64_t scadable_client_tx_bytes(void) { return s_tx_bytes; }
uint64_t scadable_client_rx_bytes(void) { return s_rx_bytes; }

static void publish_status_online(void) {
    if (!s_mqtt) return;
    const char *body = "online";
    esp_mqtt_client_publish(s_mqtt, s_status_topic, body, strlen(body), 1, 1);
}

// Topic suffix tail matching — returns true if `topic` (length tlen) ends
// with `suffix`. Topic strings from esp-mqtt are not null-terminated; we
// have to use the explicit length.
static bool topic_ends_with(const char *topic, int tlen, const char *suffix) {
    int slen = (int)strlen(suffix);
    if (tlen < slen) return false;
    return memcmp(topic + tlen - slen, suffix, slen) == 0;
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *data) {
    (void)args; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to %s", s_current_uri);
            publish_status_online();
            // Subscribe to per-device command topics (cmd/ota, future cmd/*).
            int sub_id = esp_mqtt_client_subscribe(s_mqtt, s_cmd_sub_topic, 1);
            ESP_LOGI(TAG, "subscribed to %s (msg_id=%d)", s_cmd_sub_topic, sub_id);
            xEventGroupSetBits(s_evt, CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected (will auto-reconnect)");
            xEventGroupClearBits(s_evt, CONNECTED_BIT);
            // Re-check the edge router before esp-mqtt's auto-reconnect
            // fires. If the operator toggled the device to a different
            // region, the kick that disconnected us is paired with a
            // changed /v1/route response — the maintainer will swap the
            // URI in time for the next connection attempt.
            if (s_route_task) {
                xTaskNotifyGive(s_route_task);
            }
            break;
        case MQTT_EVENT_DATA:
            if (event && event->topic && event->data) {
                s_rx_bytes += (uint64_t)event->topic_len + (uint64_t)event->data_len;
                if (topic_ends_with(event->topic, event->topic_len, "/cmd/ota")) {
                    ESP_LOGI(TAG, "OTA command received (%d bytes)", event->data_len);
                    ota_dispatch(event->data, (size_t)event->data_len);
                } else if (topic_ends_with(event->topic, event->topic_len, "/cmd/config")) {
                    ESP_LOGI(TAG, "config command received (%d bytes)", event->data_len);
                    config_apply(event->data, (size_t)event->data_len);
                } else if (topic_ends_with(event->topic, event->topic_len, "/cmd/test")) {
                    ESP_LOGI(TAG, "test command received (%d bytes)", event->data_len);
                    scadable_tests_dispatch(event->data, (size_t)event->data_len);
                } else {
                    ESP_LOGI(TAG, "unhandled msg on topic (len=%d)", event->topic_len);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            if (event && event->error_handle) {
                ESP_LOGE(TAG, "mqtt error type=%d tls_last_err=0x%x",
                         event->error_handle->error_type,
                         event->error_handle->esp_tls_last_esp_err);
            }
            break;
        default:
            break;
    }
}

// Re-fetch /v1/route and, if the broker host changed, point esp-mqtt
// at the new URI and force a reconnect. Runs in its own task because
// esp_http_client_perform blocks on the network — we don't want to
// stall the MQTT event loop or the heartbeat task.
static void route_maintainer_task(void *arg) {
    (void)arg;
    for (;;) {
        // Wait for an explicit poke (boot, disconnect). No periodic
        // polling — the kick is what triggers re-check, and a healthy
        // connection has no reason to ask "did my home broker move?"
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char host[64];
        char region[16];
        int  port = 0;
        if (edge_route_fetch(host, sizeof(host), &port, region, sizeof(region)) != ESP_OK) {
            // Edge unreachable — keep the URI we have. esp-mqtt's own
            // backoff will retry, and the next disconnect will poke us
            // again.
            continue;
        }
        if (port <= 0) port = 8883;

        char new_uri[URI_BUF_LEN];
        snprintf(new_uri, sizeof(new_uri), "mqtts://%s:%d", host, port);

        if (strcmp(new_uri, s_current_uri) == 0) {
            continue;  // no-op
        }

        ESP_LOGI(TAG, "edge: broker change %s -> %s (region=%s)",
                 s_current_uri, new_uri, region);
        strncpy(s_current_uri, new_uri, sizeof(s_current_uri) - 1);
        s_current_uri[sizeof(s_current_uri) - 1] = '\0';

        if (s_mqtt) {
            esp_mqtt_client_set_uri(s_mqtt, s_current_uri);
            // Force an immediate reconnect attempt with the new URI
            // rather than wait the full reconnect_timeout_ms.
            esp_mqtt_client_reconnect(s_mqtt);
        }
    }
}

void scadable_client_init(const char *device_id, const char *firmware_version) {
    strncpy(s_device_id,         device_id,         sizeof(s_device_id) - 1);
    strncpy(s_firmware_version,  firmware_version,  sizeof(s_firmware_version) - 1);

    snprintf(s_topic_prefix,  sizeof(s_topic_prefix),  "scadable/devices/%s", s_device_id);
    snprintf(s_status_topic,  sizeof(s_status_topic),  "%s/status",  s_topic_prefix);
    snprintf(s_cmd_sub_topic, sizeof(s_cmd_sub_topic), "%s/cmd/#",   s_topic_prefix);

    s_evt    = xEventGroupCreate();
    s_pubmux = xSemaphoreCreateMutex();

    // Resolve the broker via the edge router. The device CN is what
    // the backend keys on, so make sure edge_route knows it.
    edge_route_init(s_device_id);

    char host[64]; char region[16]; int port = 0;
    if (edge_route_fetch(host, sizeof(host), &port, region, sizeof(region)) == ESP_OK) {
        if (port <= 0) port = 8883;
        snprintf(s_current_uri, sizeof(s_current_uri), "mqtts://%s:%d", host, port);
        ESP_LOGI(TAG, "edge: initial broker = %s (region=%s)", s_current_uri, region);
    } else {
        ESP_LOGW(TAG, "edge unreachable at boot — falling back to %s", s_current_uri);
    }

    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri = s_current_uri,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .client_id = s_device_id,
        },
        .session = {
            // 3s keepalive so EMQX detects a dead client within ~4.5s
            // (3 * default 1.5 multiplier). Combined with webhook + Redis
            // pub/sub + SSE delivery (~150ms), this puts UI offline
            // detection well under the 5s hard SLA.
            .keepalive = 3,
            .last_will = {
                .topic   = s_status_topic,
                .msg     = "offline",
                .msg_len = 7,
                .qos     = 1,
                .retain  = 1,
            },
        },
        .network = {
            .reconnect_timeout_ms = 5000,
            .timeout_ms           = 10000,
        },
    };

    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);

    // Spawn the route maintainer AFTER the client is up so a poke
    // landing immediately has a real handle to call set_uri on.
    xTaskCreate(route_maintainer_task, "scadable_route", 4096, NULL, 5, &s_route_task);

    ESP_LOGI(TAG, "init device=%s fw=%s broker=%s prefix=%s",
             s_device_id, s_firmware_version, s_current_uri, s_topic_prefix);
}

const char *scadable_client_device_id(void)        { return s_device_id; }
const char *scadable_client_firmware_version(void) { return s_firmware_version; }

esp_err_t scadable_client_post(const char *path, const char *json_body, size_t body_len) {
    if (!s_mqtt) return ESP_FAIL;

    // Brief wait for connection; if still not connected we drop the publish
    // rather than block heartbeat/log tasks. esp-mqtt buffers in-flight on
    // its own anyway once connected.
    EventBits_t bits = xEventGroupWaitBits(s_evt, CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(2000));
    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGW(TAG, "drop publish to %s: mqtt not connected", path);
        return ESP_ERR_TIMEOUT;
    }

    char topic[TOPIC_BUF_LEN];
    snprintf(topic, sizeof(topic), "%s%s", s_topic_prefix, path);

    xSemaphoreTake(s_pubmux, portMAX_DELAY);
    int msg_id = esp_mqtt_client_publish(s_mqtt, topic, json_body, body_len, 1, 0);
    if (msg_id >= 0) {
        // Count topic + payload bytes outbound. Excludes the ~2-byte
        // fixed MQTT header — close enough for a throughput indicator.
        s_tx_bytes += (uint64_t)strlen(topic) + (uint64_t)body_len;
    }
    xSemaphoreGive(s_pubmux);

    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish %s failed", topic);
        return ESP_FAIL;
    }
    return ESP_OK;
}
