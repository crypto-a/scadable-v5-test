#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Block until we have an IP. Retries the connect with exponential backoff
// internally. Returns ESP_OK once associated and IP-bound.
esp_err_t wifi_start_and_wait(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
