#include "mqtt.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

#define MAX_TOPICS 8
#define MAX_TOPIC_LEN 96

static esp_mqtt_client_handle_t s_client = NULL;
static atomic_bool s_connected = ATOMIC_VAR_INIT(false);

static SemaphoreHandle_t s_subs_mux = NULL;
static char s_topics[MAX_TOPICS][MAX_TOPIC_LEN];
static int s_topic_count = 0;
static mqtt_message_handler_t s_handler = NULL;

static void replay_subscriptions(void) {
    if (!s_client) return;
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    for (int i = 0; i < s_topic_count; i++) {
        esp_mqtt_client_subscribe_single(s_client,s_topics[i], 0);
        ESP_LOGI(TAG, "(re)subscribed: %s", s_topics[i]);
    }
    xSemaphoreGive(s_subs_mux);
}

static void event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        atomic_store(&s_connected, true);
        replay_subscriptions();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        atomic_store(&s_connected, false);
        break;
    case MQTT_EVENT_DATA:
        if (s_handler && event->topic_len > 0) {
            // Topic/data buffers in event_handle are not null-terminated.
            char topic[MAX_TOPIC_LEN];
            int n = event->topic_len < MAX_TOPIC_LEN - 1 ? event->topic_len : MAX_TOPIC_LEN - 1;
            memcpy(topic, event->topic, n);
            topic[n] = '\0';
            s_handler(topic, event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (event && event->error_handle) {
            ESP_LOGE(TAG, "error: type=%d", event->error_handle->error_type);
        }
        break;
    default:
        break;
    }
}

esp_err_t mqtt_start(const char *broker_uri) {
    s_subs_mux = xSemaphoreCreateMutex();
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = broker_uri,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 16384,
        .buffer.out_size = 16384,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

bool mqtt_is_connected(void) {
    return atomic_load(&s_connected);
}

int mqtt_publish(const char *topic, const char *payload, int len) {
    if (!s_client || !atomic_load(&s_connected)) {
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, payload, len, 0, 0);
}

int mqtt_subscribe(const char *topic) {
    if (!topic) return -1;
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    bool already = false;
    for (int i = 0; i < s_topic_count; i++) {
        if (strcmp(s_topics[i], topic) == 0) {
            already = true;
            break;
        }
    }
    if (!already && s_topic_count < MAX_TOPICS) {
        strncpy(s_topics[s_topic_count], topic, MAX_TOPIC_LEN - 1);
        s_topic_count++;
    }
    xSemaphoreGive(s_subs_mux);

    if (s_client && atomic_load(&s_connected)) {
        return esp_mqtt_client_subscribe_single(s_client,topic, 0);
    }
    return 0;  // deferred to next connect
}

void mqtt_set_message_handler(mqtt_message_handler_t handler) {
    s_handler = handler;
}
