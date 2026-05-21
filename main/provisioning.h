#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wi-Fi credential helpers backed by NVS namespace "scadable_cfg".
// Keys: wifi_ssid (string), wifi_pass (string).

// Reads stored credentials into the provided buffers. Returns true if
// BOTH ssid and password are present; false (and leaves buffers empty)
// otherwise. ssid_n and pass_n must be at least 33 and 65 respectively
// to fit the IEEE limits + null.
bool provisioning_load_credentials(char *ssid, size_t ssid_n,
                                   char *pass, size_t pass_n);

// Persist credentials to NVS. Pass empty strings to clear. Returns ESP_OK
// on success.
esp_err_t provisioning_save_credentials(const char *ssid, const char *password);

// Run the provisioning portal: bring up SoftAP "SCADABLE-XXXXXX" plus
// an HTTP server on 192.168.4.1 serving a credential-entry form.
// Blocks until the user submits credentials, at which point the
// function persists them, tears down the AP, and returns ESP_OK.
//
// If you'd prefer the device to reboot into STA mode after submission
// (cleaner state machine), call esp_restart() after this returns.
esp_err_t provisioning_run_until_credentials(void);

#ifdef __cplusplus
}
#endif
