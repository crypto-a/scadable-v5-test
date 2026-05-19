#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the OTA agent. Subscribes to scadable/<device_id>/ota/command, performs
// HTTP downloads with esp_https_ota, publishes status to scadable/<device_id>/ota/status,
// and restarts on success.
void ota_start(const char *device_id);

#ifdef __cplusplus
}
#endif
