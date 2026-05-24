#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Edge router client. The device asks the platform "which broker
// should I talk to?" via a small HTTPS GET. The result is used to
// build the MQTT URI at boot and re-checked after every disconnect so
// operator-driven region migrations land within one reconnect cycle.
//
// Identity is the device cert CN ("SC-<id>"), passed in the
// X-Device-CN header. Same value we use as MQTT client_id.

void edge_route_init(const char *common_name);

// Fetches the current route. Blocks on the network call (typically
// <300ms on a healthy WAN, 8s timeout otherwise). Caller buffers must
// be sized for the longest expected hostname; 64/16 are plenty.
//
// Returns ESP_OK on a successful 2xx with a parseable body. On any
// other outcome — DNS failure, TLS failure, non-2xx, malformed JSON
// — returns an error and leaves the out buffers untouched, so callers
// can keep using the previous route.
esp_err_t edge_route_fetch(char *host_out, size_t host_n,
                           int  *port_out,
                           char *region_out, size_t region_n);

#ifdef __cplusplus
}
#endif
