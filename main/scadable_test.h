#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One test result. Lifetime of pointers is the caller's responsibility
// — these typically come from the test function's own static strings.
typedef struct {
    bool        ok;         // true → pass, false → fail
    const char *message;    // human-readable description, never NULL
} scadable_test_result_t;

typedef scadable_test_result_t (*scadable_test_fn_t)(void);

typedef struct {
    const char *id;             // stable id (matches .scadable/tests.yml)
    const char *description;
    scadable_test_fn_t fn;
} scadable_test_t;

// Helpers for test functions.
scadable_test_result_t scadable_test_pass(void);
scadable_test_result_t scadable_test_fail(const char *msg);

// The compiled-in test table. Caller doesn't own the pointer.
const scadable_test_t *scadable_tests_list(size_t *count_out);

// Called from scadable_client on MQTT_EVENT_DATA for cmd/test.
// Payload: {"test_id":"...","run_id":"...","run_all":bool}
// When run_all is true, runs every test sequentially.
// Each test result is published on scadable/devices/{id}/test_result.
void scadable_tests_dispatch(const char *json_payload, size_t len);

#ifdef __cplusplus
}
#endif
