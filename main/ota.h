#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ota_dispatch is called from scadable_client.c when an MQTT publish
// arrives on scadable/devices/{id}/cmd/ota. The payload is expected to
// be a JSON object: {"url":"http://...", "deployment_id":"...", "sha256":"..."}.
//
// It spawns a FreeRTOS task that downloads the new firmware via
// esp_https_ota and reboots the device on success. Status updates are
// published back via scadable_client_post("/ota_status", ...).
void ota_dispatch(const char *json_payload, size_t len);

#ifdef __cplusplus
}
#endif
