#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up Wi-Fi using credentials stored in NVS. If none are stored,
// or stored credentials fail repeatedly, drops into AP-mode
// provisioning (provisioning.h) and reboots once the user submits
// new creds. Blocks until we have an IP on success.
esp_err_t wifi_connect_or_provision(void);

// Legacy signature — no longer reads its arguments; kept so older call
// sites still compile. Routes to wifi_connect_or_provision().
esp_err_t wifi_start_and_wait(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
