#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// SCADABLE device identity loaded from NVS namespace "scadable_cfg".
// The provision flow on the dashboard mints this and the browser
// flasher writes it to flash at 0x9000 before the device ever boots.
//
// If NVS doesn't contain a valid identity (legacy devices that
// haven't been re-provisioned yet), identity_load() returns false
// and callers should fall back to a MAC-derived client id so the
// device still comes online — just without cert-backed auth.
typedef struct {
    bool        ok;            // true if NVS held a complete identity
    const char *common_name;   // MQTT client_id; "SC-<device_id>"
    const char *device_id;
    const char *cert_pem;      // device certificate (PEM, NUL-terminated)
    const char *key_pem;       // device private key (PEM)
    const char *ca_pem;        // SCADABLE root CA (PEM)
} scadable_identity_t;

// Reads identity into static storage and returns a pointer to it. Safe
// to call repeatedly; the returned strings live for the program's
// lifetime. If NVS is empty or partially populated, .ok is false.
const scadable_identity_t *identity_load(void);

#ifdef __cplusplus
}
#endif
