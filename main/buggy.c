#include "buggy.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "diagnostics";

// ─── BUG #1: heap leak ────────────────────────────────────────────────────
//
// The "metrics_sample" function caches the latest sensor reading into a
// freshly-allocated buffer, then "forgets" to free the previous one. Each
// call leaks ~64 bytes. Over hours this drains free_heap_bytes and the
// SCADABLE anomaly detector picks it up as a heap_leak warning via the
// sliding-window linear regression on free_heap.
//
// In a real product, you'd see this kind of bug in a wifi reconnect
// callback or a cert-pinning refresh path. Same shape, different scenery.

static void *leaked_cache = NULL;

static void metrics_sample(void) {
    void *buf = malloc(64);
    if (!buf) return;
    memset(buf, 0xAA, 64);
    leaked_cache = buf;   // ← lost reference to the previous buffer
}

// ─── BUG #2: stack-overflow risk ──────────────────────────────────────────
//
// The "telemetry_explore" function uses unbounded recursion on a synthetic
// counter. When the input is large enough (controlled by a random tick),
// the task's stack runs out. Watchdog or panic fires; SCADABLE sees the
// uptime regression on the next heartbeat and raises a "device rebooted"
// anomaly (escalating to "boot loop" if it keeps happening).

static int telemetry_explore(int depth, int budget) {
    char stack_pressure[128];   // ~128 bytes per frame, plus locals
    stack_pressure[0] = (char)depth;
    if (budget <= 0) return stack_pressure[0];
    return telemetry_explore(depth + 1, budget - 1) + stack_pressure[0];
}

// ─── BUG #3: buffer-bounded strcpy ────────────────────────────────────────
//
// "tag_record" copies a tag into a fixed buffer with strcpy. When the input
// is longer than the buffer, neighboring memory is corrupted. Catching this
// at runtime is hard — but a code-analysis pass (or Grok during indexing)
// can flag it from the source.

static char tag_record_buf[16];

static void tag_record(const char *tag) {
    strcpy(tag_record_buf, tag);   // ← no length check
}

// ─── task ────────────────────────────────────────────────────────────────

static void diagnostics_task(void *arg) {
    int tick = 0;
    while (1) {
        tick++;
        // Slow, steady leak. ~ 64 bytes per 3s = ~76 KB/hour.
        metrics_sample();

        if ((tick % 10) == 0) {
            ESP_LOGI(TAG, "tick %d · free_heap=%lu", tick,
                     (unsigned long)esp_get_free_heap_size());
        }

        // Every ~5 minutes, run a deeper "exploration" that *might* blow the
        // stack. The budget is intentionally bounded so it usually returns —
        // but ~1 in 3 invocations exceeds the task's stack. Watchdog fires
        // and the device reboots, which SCADABLE detects via uptime reset.
        if ((tick % 100) == 0) {
            int budget = 24 + (esp_random() % 40);
            ESP_LOGW(TAG, "telemetry_explore budget=%d", budget);
            (void)telemetry_explore(0, budget);
        }

        // Sporadic strcpy that overflows when an upstream "tag" happens to
        // be long. Demonstrates a code-analysis finding the indexer surfaces.
        if ((tick % 50) == 0) {
            tag_record("ok");
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void buggy_diagnostics_start(void) {
    // Small stack on purpose — the recursion bug needs to actually hit the
    // ceiling for the demo to be visible. Tune up to silence the crash.
    xTaskCreate(diagnostics_task, "diagnostics", 4096, NULL, 3, NULL);
}
