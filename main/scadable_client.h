#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the SCADABLE HTTP client. Reads the API base URL and bearer
// token from compile-time defines (see top-level CMakeLists.txt).
void scadable_client_init(const char *device_id, const char *firmware_version);

// Post a JSON document to the given ingest path. `path` is the suffix —
// e.g. "/heartbeat", "/logs", "/event". Thread-safe; serializes internally.
esp_err_t scadable_client_post(const char *path, const char *json_body, size_t body_len);

const char *scadable_client_device_id(void);
const char *scadable_client_firmware_version(void);

// Cumulative byte counters tracked through the MQTT event pipeline.
// Used by the heartbeat task to surface network throughput in the
// dashboard. Both counters reset to zero on reboot.
uint64_t scadable_client_tx_bytes(void);
uint64_t scadable_client_rx_bytes(void);

#ifdef __cplusplus
}
#endif
