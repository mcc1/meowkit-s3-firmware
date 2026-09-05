# App 11 — AC Remote: design

Status: **design accepted 2026-09-06 00:48 (owner: 3A, 4A, 5A, icon A — an
air-conditioner unit glyph).** The `DECISION` blocks below keep the rejected
options for the record; the chosen option is the first one in each block.

## 1. Why a separate app

Air-conditioner remotes do not send "buttons"; every press transmits the whole
machine state (power, mode, temperature, fan, swing, timers) in one long
frame. MeowKit's IR receiver fades out after ~200 ms of continuous carrier
(measured 2026-09-05/06, see `docs/app09-infrared-redesign.md` §3), so single
long frames such as Hitachi's 344-bit message can never be learned, whatever
the distance. IRremoteESP8266's `IRac` class synthesises complete frames for
~50 AC protocols from a common state structure, so the state can simply be
edited on the device and transmitted without any capture.

That is a different product from the Infrared app: the data model is a *state*
per air-conditioner, not a list of learned signals, and the interaction is a
remote's LCD + buttons, not learn/replay. Keeping it separate leaves Infrared's
menus untouched and lets the AC app grow (timers, presets) on its own.

Verified precedent: `tools/complete-hitachi344.py` rebuilt a full frame from a
faded capture and the owner's RAS-22NK accepted ON_COOL and OFF (2026-09-06).

## 2. Scope of v1

- New app in the `app_11` slot (replaces the stub; registration order and
  `APP_BUILTIN_ICONS[]` stay aligned). Name: **AC Remote**.
- Multiple AC devices, each = name + protocol + optional model + remembered
  state. Owner has at least two: Hitachi RAS-22NK (HITACHI_AC344) and a Daikin.
- Controls: power, mode, temperature, fan speed, vertical swing, horizontal
  swing; advanced toggles quiet / turbo / econo / light behind an options
  menu (protocols that lack a feature ignore it, IRac handles that).
- Transmission through one `IRac` instance on `HAL_PIN_IR_TX` (GPIO 7). The
  Infrared app and this app never run at the same time (Mooncake runs one
  foreground app), so sharing the pin is safe.
- Out of scope for v1: timers, clock, sleep, iFeel/sensor temperature,
  Fahrenheit, protocol auto-detection from a faded capture.

## 3. Data model and storage

```text
struct AcDevice {
  char          name[32];      // file stem, sanitised like Infrared device names
  decode_type_t protocol;      // e.g. HITACHI_AC344
  int16_t       model;         // -1 when the protocol has no models
  stdAc::state_t state;        // power, mode, degrees, fanspeed, swingv, swingh,
                               // quiet, turbo, econo, light (others left default)
};
```

`DECISION 3 — where devices live`

- **A. SD card, `/ac/<Name>.cfg`, key=value text (recommended).**
  Unlimited devices, editable and backup-able on a PC, same atomic
  tmp → .bak → rename writer discipline as the Infrared app. Requires a card,
  which the IR apps need anyway.
- **B. NVS through `src/system/persist.h`.** No card needed, but string keys
  are capped, a handful of devices at most, and nothing is visible from a PC.

State is written back after every successful send, so the app reopens on the
last transmitted state (like a real remote's LCD).

Example file:

```text
protocol: HITACHI_AC344
model: -1
power: on
mode: cool
temp: 23
fan: auto
swingv: off
swingh: off
quiet: off
turbo: off
econo: off
light: off
```

Parsing/serialising this file is a pure C++ module (`ac_store.{h,cpp}`) with
no Arduino headers, so it joins the MSVC host tests.

## 4. Screens

Framework: same as App09/App10 — Mooncake `AppAbility` + LovyanGFX through
`src/app/app_common/hp_ui.h`; footer convention `[^v] [A] [>]Opts [B]`;
long-press B exits.

```text
DeviceList     rows: "<Name>"  sub: "<PROTOCOL> · <model>"      [A] open [>]Opts [B] exit
               last row "+ New device..."
               empty state (bright): "No air-conditioners yet" / "Add one with + New device"
DeviceOptions  Rename / Change protocol / Delete (confirm)
NewDevice      ProtocolPicker -> (ModelPicker if the protocol has models) -> NameEditor
               -> device created with IRac::initState defaults, opens Control
ProtocolPicker see DECISION 4
Control        see DECISION 5
Advanced       toggles: Quiet / Turbo / Econo / Light   (each toggle sends immediately)
```

`DECISION 4 — protocol picker`

- **A. Curated first, then everything (recommended).** First screen lists the
  common household brands with their protocol variants spelled out
  (Hitachi: AC / AC1 / AC264 / AC296 / AC344 / AC424; Daikin: DAIKIN /
  DAIKIN2 / DAIKIN216 / ...; Panasonic, Mitsubishi, Fujitsu, Gree, Toshiba,
  Sharp, LG, Samsung, Midea, Carrier, Haier, Kelvinator, Coolix), plus a last
  row "All supported protocols (N)" that lists every `decode_type_t` for which
  `IRac::isProtocolSupported()` is true, alphabetically with letter jumps on
  Left/Right. Models come from `irutils::modelToStr()`.
- **B. Alphabetical list of all supported protocols only.** Less code, but the
  user must already know that RAS-22NK means HITACHI_AC344.

`DECISION 5 — control screen`

- **A. Remote-style grid, every press transmits (recommended).**
  Top third: a status panel imitating a remote's LCD — big temperature, mode
  glyph/word, fan bars, swing markers, power state, protocol name small.
  Bottom two thirds: virtual buttons `Power`, `Temp -`, `Temp +`, `Mode`,
  `Fan`, `Swing`, navigated with the joystick, A presses the highlighted
  button. Each press updates the state, sends the full frame at once, flashes
  the LED and refreshes the panel. This is exactly how a physical AC remote
  behaves, so there is no "did I send it yet?" ambiguity.
- **B. Field list with explicit send.** Rows Power / Mode / Temp / Fan / Swing
  V / Swing H; Up/Down picks a row, Left/Right changes the value, A sends.
  Lets the user set several fields and send once; more presses per change and
  the on-screen state can differ from the machine until A is pressed.
- **C. A with an "edit without sending" hold:** as A, but holding A for a
  second toggles a "compose" mode where presses only edit and the next A
  sends. Most flexible, most code.

Temperature range and fan levels come from the protocol (IRac clamps); the
panel shows what will actually be sent.

## 5. Transmit path

```text
IRac ac(HAL_PIN_IR_TX);           // created in onOpen, deleted in onClose
ac.next = device.state;           // protocol, model and the fields above
ac.sendAc();                      // library encodes, checksums, repeats
ac.markAsSent();                  // keeps prev for toggle-style protocols
```

`IRac` already carries the previous state, which some protocols need for
toggle bits (Hitachi's "button" byte, Daikin's swing toggles), so one instance
per app session is kept and re-seeded from the stored state on open. Sending
takes 100–500 ms and runs synchronously; the LED is lit for the duration and a
toast "Sent" follows. Failures (`sendAc()` false) toast "Protocol not
supported" — only possible if the picker lists something the build excludes.

**Hitachi 424/344 need the key code (found on hardware 2026-09-06).** These
frames carry a "Button" byte saying which key was pressed (0x13 power/mode,
0x42 fan, 0x43 temp down, 0x44 temp up, 0x81 swing V, 0x8C swing H). The
indoor unit applies a field only when the matching key code arrives: the
owner's RAS-22NK changed power and mode but kept showing 24 °C because
`IRac::hitachi344()` ends with `setPower()`, which overwrites the Button byte
with power/mode on every frame. For `HITACHI_AC424`, `HITACHI_AC344` (and the
`IRHitachiAc424`-derived 264 variant) the app therefore drives the protocol
class directly: apply every field, then `setButton()` according to the pad
that was pressed (Temp ± → 0x44/0x43, Fan → 0x42, Swing → 0x81, Power/Mode →
0x13), then `send()`. All other protocols keep the generic `IRac` path.

Flash budget: pulling `IRac` in compiles every AC protocol. Expected
+300–500 KB on a 12.5 MB app partition currently 55 % used; the build report
must state the actual delta and stay under 70 %.

## 6. Interplay with the Infrared app

- Infrared keeps its raw Learn path; nothing moves.
- v1.1 candidate: when Infrared's LearnResult shows a FADED capture whose
  decoder protocol is an AC protocol, add a hint row "Use AC Remote for this
  air-conditioner" (no navigation across apps in v1).

## 7. Icon

Icons are 70×70 LVGL `TRUE_COLOR_ALPHA` C arrays under `src/ui/images/`
generated from PNGs (SquareLine). A new app needs `ui_img_ac_remote_png.c`
plus the `APP_BUILTIN_ICONS[]` entry.

`DECISION 7 — icon source`

- **A. Draw one to match the existing set (recommended).** Flat glyph in the
  same palette as the current icons (snowflake or an AC unit with airflow
  lines), rendered to PNG, converted with the existing icon conversion
  approach, preview shown to the owner before it is committed.
- **B. Reuse `ui_img_smarthome_png`** (currently on the app_12 stub). Zero
  work, generic look.
- **C. Owner supplies a 70×70 PNG.**

## 8. Verification

| Layer | How | Status |
| ----- | --- | ------ |
| ac_store | host tests: parse/serialise round trip, unknown keys ignored, clamping | **done** 2026-09-06 — `src/app/app_11/ac_store.cpp` joined `tools/test-ir-codec.ps1`; 110 new assertion sites in `test/ir_codec/test_ir_codec.cpp::testAcStore()` (round trip, defaults, clamping, unknown keys, CRLF + missing final newline, bad enum fallback, token tables, buffer refusal). Whole suite passes, 0 failures. |
| firmware | `esp32s3box` build, zero warnings, flash delta reported | **done** 2026-09-06 — build SUCCESS, zero warnings. Flash 55.1 % → **56.0 %** (7 342 073 / 13 107 200 B, ≈ +117 KB), well under the 70 % ceiling. RAM 38.6 %. |
| review | fresh-context reviewer against this document | **done** 2026-09-06 — no blockers; 3 major + 10 minor findings fixed (failed-send rollback, `_repaintOnly` scene race — also patched in App09 `infrared.cpp`, load-failure guard). |
| device (owner) | Hitachi RAS-22NK: Power on cool 23 → beep; Temp+ twice → 25 shown on unit; Mode heat; Fan high; Power off. Daikin: same sequence. Reopen app: last state restored. Infrared app still learns/sends TV codes afterwards (pin sharing). | pending — not flashed |

## 9. Milestones

1. **M1 — skeleton + Hitachi/Daikin. BUILT 2026-09-06** (pending owner's
   hardware test). app_11 registered with `&ui_img_ac_remote_png`, DeviceList
   (+ bright empty state), DeviceOptions (Rename / Change protocol / Delete),
   NewDevice → picker → ModelPicker → NameEditor, Control screen, `/ac/*.cfg`
   storage with the atomic tmp → .bak → rename writer, send through `IRac`.
2. **M2 — full picker + models + advanced toggles + icon. BUILT 2026-09-06.**
   Curated picker ships all 37 brand rows of DECISION 4A (filtered by
   `IRac::isProtocolSupported()`) plus "All supported protocols (N)" —
   alphabetical, Left/Right jump to the next/previous initial letter.
   ModelPicker appears only when `irutils::modelToStr()` names a model
   ("Default" = -1). Advanced screen carries Quiet / Turbo / Econo / Light,
   each toggle transmitting at once. Icon pixel data is replaced separately.
3. **M3 (optional) — Infrared FADED hint, presets/timers.** Not started.

4. **M1b — Hitachi 424-family key codes. BUILT 2026-09-06** after the owner's
   RAS-22NK ignored Temp ±. `src/app/app_11/app_11_hitachi.{h,cpp}` keeps one
   `IRHitachiAc424`/`344`/`264` object alive per opened device, applies the
   fields, stamps `setButton()` last and calls `send()`. Auto is dropped from
   the Mode cycle and the Advanced toggles read `n/a` for these protocols,
   because the classes carry neither. Every other protocol still goes through
   `IRac`.

Deviations from the screens above, all deliberate:

- The Control screen carries a **seventh, full-width `ADVANCED` key** below the
  six of DECISION 5A. Left/Right are the pad's own navigation there, so the
  `[>]Opts` footer convention has no free direction on that screen; the key is
  the visible replacement. `[>]` still opens DeviceOptions from DeviceList.
- The **protocol name sits in the Control header badge**, not inside the status
  panel, leaving the panel's 72 px for the values that change per press.
- The DeviceList sub-line spells `PROTOCOL - model` (ASCII hyphen) rather than
  a middot; nothing else in the firmware prints non-ASCII glyphs.
