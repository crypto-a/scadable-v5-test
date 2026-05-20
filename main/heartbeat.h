#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the heartbeat task. POSTs JSON to /api/v1/ingest/heartbeat every 5 s
// with device_id, firmware_sha, uptime, free_heap.
void heartbeat_start(void);

#ifdef __cplusplus
}
#endif
