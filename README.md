# scadable-v5-test (firmware) 

Demo ESP-IDF v5.1 firmware for ESP32 classic. This repo is the build target
for the SCADABLE dashboard — link it from any namespace's Settings page and
the graph populates with commits, components, and (eventually) the live
device data it produces.

## What this firmware does

On boot it connects to Wi-Fi and starts three tasks:

- **`heartbeat`** — every 5 s, POSTs a JSON heartbeat to
  `${SCADABLE_API_BASE}/api/v1/ingest/heartbeat` with the device ID,
  firmware SHA, uptime, and free heap. Authenticated with the namespace's
  bearer token (compile-time `SCADABLE_API_TOKEN`).
- **`log_flush`** — a custom `esp_log_set_vprintf` sink captures every
  `ESP_LOGx` call into a 16 KB RAM ring buffer. Every 30 s (or at 75 %
  full) the buffer flushes to `/api/v1/ingest/logs` as a parsed array of
  `{level, tag, message, uptime_ms}`.
- **`diagnostics`** — a "feature" task that exists so the dashboard has
  something interesting to detect. See the next section.

There is **no MQTT and no OTA in this branch.** The build pipeline parks
both intentionally; ingest is plain HTTPS. The original MQTT/OTA path lives
in the project history if we ever turn it back on.

## Intentional bugs (so the anomaly detector has work to do)

`main/buggy.c` contains three deliberate bugs documented inline:

1. **Heap leak** in `metrics_sample()` — caches a 64-byte buffer on every
   call and "loses" the previous pointer. The SCADABLE backend's sliding-
   window linear regression on `free_heap_bytes` flags this as a
   `heap_leak` warning within minutes; once the projected OOM is under an
   hour it escalates to `critical`.
2. **Stack overflow risk** in `telemetry_explore()` — unbounded recursion
   bounded only by a synthetic budget. When the budget is large enough the
   task's 4 KB stack runs out, the watchdog fires, the device reboots, and
   SCADABLE picks it up via the uptime regression on the next heartbeat
   (`reboots` anomaly, escalating to `boot_loop` after three in an hour).
3. **Unchecked `strcpy`** in `tag_record()` — copies an untrusted tag into
   a fixed 16-byte buffer with no length check. A code-analysis pass (or
   Grok during repo indexing) surfaces this without the device ever having
   to misbehave.

Together these give a real, observable timeline:

```
t+0      device boots, heartbeats flow
t+3m     heap_leak warning fires (~64 B/3s leak, slope detected)
t+15m    OOM projection drops below 1 h → escalates to critical
t+~?     stack overflow → reboot → anomaly fires
t+~?     reboot count ≥ 3 in 60 min → boot_loop critical
```

## Build

```bash
. ~/esp/esp-idf/export.sh

# wiring (the dashboard's Settings page shows these for your namespace)
export SCADABLE_API_BASE=http://146.190.253.86:8080
export SCADABLE_API_TOKEN=sk_...

idf.py build
idf.py -p /dev/cu.usbserial-XXX flash monitor
```

The firmware version baked into each binary is the git short SHA, which is
what shows up in the dashboard graph under "Firmware".

## Layout

```
.
├── CMakeLists.txt        # top-level; bakes git SHA + env into binary
├── sdkconfig.defaults    # 2 MB flash, default partition table, no OTA
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── main.c
    ├── wifi.c / .h       # Wi-Fi association + retry
    ├── scadable_client.c / .h    # HTTPS POST wrapper, bearer auth
    ├── heartbeat.c / .h          # 5 s heartbeat task
    ├── log_sink.c / .h           # 16 KB ring + 30 s flush task
    └── buggy.c / .h              # the deliberate bugs
```

That's the whole demo firmware.
