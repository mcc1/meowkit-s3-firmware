# MeowKit-S3 Firmware

This is a public personal derivative of the MeowKit-S3 firmware. It is not an
official vendor release. The original upstream remote is kept as `origin`; the
personal GitHub mirror is `mcc1/meowkit-s3-firmware`.

## Personal modifications

The public mirror documents and preserves the following local changes:

- The Wi-Fi password keyboard includes the full printable ASCII punctuation
  set, including characters such as `;`, `\\`, `{`, `|`, and `~`.
- Firmware dependencies are recorded as pinned Git submodules so a fresh
  checkout does not silently use floating revisions.
- The splash GIF embedding path is portable across machines instead of pointing
  to the original developer's absolute filesystem path.
- PowerShell tools prepare local-test or stable installer artifacts and open a
  serial monitor; they never flash a device or publish a release by themselves.

These changes are provided so other MeowKit owners can inspect, reproduce, and
adapt the work. Review the hardware and firmware risks before using a generated
image.

## Reproducible source checkout

This repository records the firmware dependencies as Git submodules. After a
fresh clone, initialize them with:

```powershell
git submodule update --init --recursive
```

The firmware build currently expects these pinned commits:

| Dependency | Repository | Commit |
| --- | --- | --- |
| Mooncake | `Forairaaaaa/mooncake` | `0dfc177b72fcc55095fd222b0bdc69595b001d9f` |
| IRremoteESP8266 | `crankyoldgit/IRremoteESP8266` | `3390e72877ac15c74f602151de4a2fb9a154bdb0` |
| ESP32-BLE-Mouse | `T-vK/ESP32-BLE-Mouse` | `ba38caa695cece3cf06ff67b203e9400efe546f4` |
| arduinoFFT | `kosme/arduinoFFT` | `6b3ef9732db800bb2c658eefb7ad8939eb44e6e2` |

The release tool checks these pins before building, so a dependency update must
be an intentional source change rather than an accidental floating checkout.

## Build and prepare installer artifacts

Install Python, PlatformIO Core, and the ESP32-S3 platform used by
`platformio.ini`. Then run the tool from this directory:

```powershell
.\tools\publish-firmware.ps1 -Channel local-test -Force
```

This command initializes the submodules, checks their commits, builds the
`esp32s3box` environment, creates a merged factory image at flash offset `0`,
creates a channel manifest, metadata, and checksum under the installer's
ignored `generated/` directory. It never edits the installer's `index.html`.

The output directory can be overridden for CI packaging:

```powershell
.\tools\publish-firmware.ps1 -Channel local-test -OutputRoot .\ci-generated -Force
```

To prepare a stable version:

```powershell
.\tools\publish-firmware.ps1 -Channel stable -Version 1.0.1
```

The tool only prepares local artifacts. It does not flash a device, commit,
push, deploy, or create a hosted release. Review both repository statuses and
diffs before any of those actions.

Use `-SkipBuild` only when re-packaging already existing files under
`.pio\build\esp32s3box`.

## Verified workflow (2026-09-05)

The following checks passed in this workspace:

- A full `esp32s3box` PlatformIO build completed successfully with the pinned
  submodules.
- `publish-firmware.ps1 -Channel local-test -Version
  local-test-2026-09-05-symbol-keyboard -Force` completed factory-image
  packaging. The new output contract is the installer's
  `generated/local-test/` directory containing the image, manifest, metadata,
  and checksum.
- PowerShell syntax checks passed for all three project tools.
- `serial-monitor.ps1 -Port COM6 -BaudRate 9600` successfully opened the
  physical `COM6` during a short test. No serial output was observed during
  that test; device log content and longer runtime monitoring remain a
  hardware-level follow-up.

The browser Web Serial connection and actual device flash were not performed
by this verification pass.

## Serial monitor

With MeowKit in normal firmware mode, use the included monitor script:

```powershell
.\tools\serial-monitor.ps1
```

The defaults are `COM6` and `9600` baud. Override them when needed:

```powershell
.\tools\serial-monitor.ps1 -Port COM7 -BaudRate 115200 -LogPath .\logs\meowkit.log
```

The script default follows the current `Serial.begin(9600)` call in
`src/bsp/devices.cpp`. `platformio.ini` still declares `monitor_speed=115200`,
so the baud-rate discrepancy remains to be resolved by an authoritative source
or a device test; try the explicit `-BaudRate` override when diagnosing logs.

Close this monitor before opening another program that needs the same COM port,
including a PC Monitor telemetry sender or the browser installer.
