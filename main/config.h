#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the runtime config from NVS. Call after nvs_flash_init().
// Falls back to defaults if no values are stored yet.
void config_init(void);

// Apply a JSON config blob received over MQTT (cmd/config topic).
// Persists the new values to NVS so they survive reboots.
//
// Recognized fields:
//   heartbeat_interval_ms    int  1000 ..  300000
//   log_flush_interval_ms    int  5000 ..  600000
void config_apply(const char *json_payload, size_t len);

// Current runtime values. Heartbeat and log_sink tasks read these on
// each iteration so updates take effect immediately.
int config_heartbeat_interval_ms(void);
int config_log_flush_interval_ms(void);

#ifdef __cplusplus
}
#endif
