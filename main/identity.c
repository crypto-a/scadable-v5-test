// Loads the SCADABLE device identity from NVS namespace
// "scadable_cfg". Keys written by the provision flow:
//
//   common_name    "SC-<device_id>"
//   device_id      hex
//   cert_pem       PEM cert with trailing NUL
//   key_pem        PEM private key
//   ca_pem         SCADABLE root CA PEM
//
// We allocate space for the values once and never free — these are
// boot-time configuration, not runtime data. Allocation lives in the
// IDF default heap.

#include "identity.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "identity";
static const char *NS  = "scadable_cfg";

static scadable_identity_t s_id;
static bool                s_loaded;

// Allocates and reads a NUL-terminated NVS string. Returns NULL on
// any failure (key absent, etc.) so callers can detect partial data.
static char *load_str(nvs_handle_t h, const char *key) {
    size_t n = 0;
    if (nvs_get_str(h, key, NULL, &n) != ESP_OK) return NULL;
    if (n == 0) return NULL;
    char *buf = malloc(n);
    if (!buf) return NULL;
    if (nvs_get_str(h, key, buf, &n) != ESP_OK) {
        free(buf);
        return NULL;
    }
    return buf;
}

const scadable_identity_t *identity_load(void) {
    if (s_loaded) return &s_id;
    s_loaded = true;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' missing — no provisioned identity", NS);
        return &s_id; // .ok stays false
    }

    s_id.common_name = load_str(h, "common_name");
    s_id.device_id   = load_str(h, "device_id");
    s_id.cert_pem    = load_str(h, "cert_pem");
    s_id.key_pem     = load_str(h, "key_pem");
    s_id.ca_pem      = load_str(h, "ca_pem");
    nvs_close(h);

    // Mark as valid only if EVERY field is present — partial state
    // would mean inconsistent flashing and we shouldn't trust it.
    s_id.ok = (s_id.common_name && s_id.device_id &&
               s_id.cert_pem    && s_id.key_pem   && s_id.ca_pem);

    if (s_id.ok) {
        ESP_LOGI(TAG, "loaded identity %s (cert %d bytes, key %d bytes, ca %d bytes)",
                 s_id.common_name,
                 (int)strlen(s_id.cert_pem),
                 (int)strlen(s_id.key_pem),
                 (int)strlen(s_id.ca_pem));
    } else {
        ESP_LOGW(TAG, "NVS identity incomplete — falling back to MAC-derived client id");
    }
    return &s_id;
}
