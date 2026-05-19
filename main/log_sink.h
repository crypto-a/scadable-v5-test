#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Install the vprintf-style log sink. All subsequent ESP_LOGx calls are
// captured into an internal ring buffer in addition to being forwarded to the
// previous (serial) sink.
void log_sink_install(void);

// Start the flush task. Requires MQTT to be initialized; the task itself
// waits for the connection to come up before publishing.
void log_sink_start_flush_task(const char *device_id);

#ifdef __cplusplus
}
#endif
