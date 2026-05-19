# scadable-v5-test (firmware)

ESP-IDF v5.1 firmware for ESP32 classic. This repo is the build target for the
SCADABLE deployment platform — paste this URL into the dashboard's
"New deployment" form to trigger a build + OTA push.

## What it does on boot

1. Connects to Wi-Fi (creds hardcoded in `main/main.c` for V1).
2. Resolves the MQTT broker — either via mDNS (`scadable.local`) for local dev
   or directly via the `SCADABLE_BROKER_HOST` compile-time define when set.
3. Connects to EMQX (anonymous, no TLS) and starts three loops:
   - **Heartbeat** every 5 s on `scadable/<device_id>/heartbeat`.
   - **Log batch** every 60 s (or at 75 % of a 16 KB ring) on
     `scadable/<device_id>/logs`.
   - **OTA agent** subscribed to `scadable/<device_id>/ota/command`. Downloads
     the URL with `esp_https_ota` and publishes progress on
     `scadable/<device_id>/ota/status`. Restarts on success.

## Build

```bash
. ~/esp/esp-idf/export.sh
export SCADABLE_BROKER_HOST=<broker-ip>   # bakes broker into binary
idf.py build
idf.py -p /dev/cu.usbserial-XXX flash monitor
```

`firmware_version` is the git short SHA, baked at configure time.

## Partition layout

`partitions.csv` provisions two 1.5 MB OTA slots on 4 MB flash (ota_0 + ota_1)
so the device can apply updates without bricking — failed images roll back.

## OTA command payload

```json
{
  "version": "abc1234",
  "url": "http://<minio-host>:9000/scadable-firmware/abc1234.bin"
}
```

## OTA status payloads

```json
{ "state": "downloading", "version": "abc1234" }
{ "state": "progress",    "version": "abc1234", "details": "40% (350000/877000)" }
{ "state": "success",     "version": "abc1234", "details": "restarting" }
{ "state": "failed",      "version": "abc1234", "details": "<error name>" }
```
