#pragma once

// These two functions intentionally contain bugs the SCADABLE dashboard
// should catch. They exist so a fresh deploy can be observed misbehaving in
// realistic ways — slow heap leak, occasional crash — without us having to
// fake the data on the backend.

#ifdef __cplusplus
extern "C" {
#endif

// Starts the "diagnostics" task. Runs forever, leaking heap on a schedule
// and (rarely) triggering an intentional crash so the anomaly detector has
// real input to flag.
void buggy_diagnostics_start(void);

#ifdef __cplusplus
}
#endif
