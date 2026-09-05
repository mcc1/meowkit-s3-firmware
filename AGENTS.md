# MeowKit-S3 firmware agent instructions

This public repository is a personal derivative of the vendor firmware. It is
not an official release. Keep the upstream GitHub repository as `origin` and
use the personal public mirror `mcc1/meowkit-s3-firmware` as the `github`
remote when it is configured locally.

## Personal modifications

- `src/ui/screens/ui_t9_keyboard.c` contains the expanded printable-ASCII
  symbol pages used by the Wi-Fi password UI.
- `.gitmodules` pins Mooncake, IRremoteESP8266, ESP32-BLE-Mouse, and arduinoFFT
  to the commits documented in `README.md`.
- `tools/embed_gif.py` and `src/splash/splash_gif_data.S` keep splash embedding
  independent of the original developer's filesystem path.
- `tools/publish-firmware.ps1` builds and packages installer artifacts without
  flashing, committing, pushing, deploying, or creating a hosted release.
- `src/app/app_09/` (Infrared) was rewritten in September 2026 against
  `docs/app09-infrared-redesign.md`; the Flipper `.ir` <-> IRremoteESP8266
  bit rules are in `docs/app09-flipper-protocol-map.md`. Read the spec before
  changing the app. `src/app/app_09/ir_flipper_codec.*` must stay free of
  Arduino headers so `tools/test-ir-codec.ps1` (MSVC host unit tests, no
  PlatformIO) keeps running; run it after any codec change.
- `tools/serial-monitor.ps1` defaults to `COM6` at `9600` baud because the
  current source calls `Serial.begin(9600)`. `platformio.ini` still declares
  `monitor_speed=115200`; keep this discrepancy explicit until authoritative
  source or device testing resolves it.

## Change and verification rules

- Read `platformio.ini`, the BSP files, `MeowKit`, app registration, and
  `src/system/` before changing firmware behavior.
- Do not modify vendored code under `lib/` unless the task explicitly requires
  a dependency change; update the pin and verification record together.
- After source changes, run the relevant `esp32s3box` build and inspect the
  exact diff. Source inspection alone does not prove device behavior.
- Treat upload, browser installation, and hardware feature tests as pending
  until they are performed on the physical device.
- Do not execute Bad USB, BLE HID/BLE Spam, or infrared payloads without a
  narrow, explicit test request.
- Keep this public repository transparent about personal changes and never
  include Wi-Fi passwords, tokens, or other private device data.
