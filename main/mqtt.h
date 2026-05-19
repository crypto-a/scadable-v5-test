#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mqtt_message_handler_t)(const char *topic, const char *data, int data_len);

esp_err_t mqtt_start(const char *broker_uri);
bool mqtt_is_connected(void);
int mqtt_publish(const char *topic, const char *payload, int len);

// Subscribe to a topic. Safe to call before the client is connected; queued
// subscriptions are replayed on every (re)connect.
int mqtt_subscribe(const char *topic);

// Install a global message handler. Called for every subscribed topic.
void mqtt_set_message_handler(mqtt_message_handler_t handler);

#ifdef __cplusplus
}
#endif
