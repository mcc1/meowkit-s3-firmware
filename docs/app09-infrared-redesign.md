# App 09 — Infrared: redesign spec

Status: **design accepted 2026-09-05, implementation in progress.**
Owner decisions recorded in this file are final unless the owner changes them.

## 1. Why

The shipped App 09 (`src/app/app_09/infrared.cpp`) has four defects, all
confirmed by code reading on 2026-09-05:

| # | Symptom | Root cause |
|---|---------|------------|
| 1 | A learned signal replays fine before saving, but a saved signal does nothing when sent later. | `writeSignalEntry` writes only `protocol/address/command`; `value`/`bits` used by `IRsend::send()` are never persisted. `_loadRemote` rebuilds them with `bits = 32; value = address \| command << 16`, which matches no real protocol encoding. Only `type: raw` entries round-trip. The same code path makes ~60 % of the bundled universal library (2,399 parsed entries) dead. |
| 2 | Every Learn creates a new device file holding one signal. | The save screen has a single name field that is used both as the signal name and as the file name (`/infrared/<name>.ir`). Append only happens if a file with the *signal* name already exists. |
| 3 | No rename / delete for devices or signals. | Never implemented. Only Create and Read exist. |
| 4 | Universal Remote looks unrelated to Saved Remotes and shows an unreadable grey "No .ir files found". | It reads `/infrared/universal/`, blasts every brand's signal of the chosen name (TV-B-Gone style), and its empty state is drawn in `COL_DIM`. The docs never tell users to copy `sd files/infrared/universal/` to the card, so most cards are empty. |

Owner decisions (2026-09-05):

- **1A** — keep the Flipper `.ir` format on disk and add a correct per-protocol
  codec between Flipper `(protocol, address, command)` and IRremoteESP8266
  `(type, data, nbits)`. No private extension lines.
- **2A** — device-centric flow: a device file is a container, signals are its
  buttons; Learn asks *which device* before asking the button name.
- Universal Remote stays, is renamed so its "all brands" nature is visible,
  gets a readable empty state, progress feedback, and no per-press full-file
  reload.
- 2026-09-05 (owner): Universal is a *discovery* tool, not the daily remote.
  A sweep can be paused on the entry that made the device react, stepped back
  and re-sent to confirm, and saved as a normal device containing every button
  of that protocol/address, so later use goes through Remotes and is instant.
  Default sweep gap is 1.0 s so a human can react; 0.25 s remains selectable.
- IRremoteESP8266 stays pinned at `3390e72` (v2.8.6+24). A bump to v2.9.0 is a
  separate, later commit.

## 2. Storage model (unchanged on disk, fixed in code)

```text
/infrared/<Device>.ir            one device = one file = N signals (Flipper "IR signals file")
/infrared/universal/<cat>.ir     bundled multi-brand library (Flipper "IR library file")
```

Entry grammar (Flipper compatible, unchanged):

```text
#
name: <button>
type: parsed
protocol: <FlipperProtoName>
address: XX XX XX XX        # little-endian bytes of a uint32
command: XX XX XX XX
```

```text
#
name: <button>
type: raw
frequency: 38000
duty_cycle: 0.330000
data: <space separated µs timings>
```

Rules:

- Device file name = sanitised device name + `.ir`. Sanitiser: trim, collapse
  spaces to `_`, drop `\ / : * ? " < > |`, max 31 chars, never empty.
- Signal names are unique within a file, case-insensitive. Saving a new button
  under an existing name asks Replace / Rename / Cancel; renaming onto an
  existing name shows "Name exists" and stays in the editor. A case-only device
  rename is allowed (FAT is case-insensitive, so it goes through a temp name).
- Entries whose protocol the codec does not know keep their original
  `protocol:` string verbatim across rewrites; they are listed but not sendable.
- Files are written with LF line endings only (never `println`, which emits
  CRLF on the ESP32 core).
- Writes are atomic: write `<file>.tmp`, then remove the original and rename
  the temp over it. A failed write never truncates the original.
- Rename device = `SD_MMC.rename`; refuse if the target exists.
- Delete device = `SD_MMC.remove` after a confirm dialog.
- Rename/delete signal = load, edit in memory, atomic rewrite.

## 3. Codec module (`src/app/app_09/ir_flipper_codec.{h,cpp}`)

Pure C++17, **no Arduino / IRremoteESP8266 headers** so it compiles on the
host. Public surface:

```cpp
namespace irfc {
enum class Proto : uint8_t { NEC, NECext, NEC42, NEC42ext, Samsung32,
                             SIRC, SIRC15, SIRC20, RC5, RC5X, RC6,
                             Kaseikyo, RCA, Pioneer, Unknown };
struct Flipper { Proto proto; uint32_t address; uint32_t command; };
enum class TxKind : uint8_t { NEC, SAMSUNG, SONY, RC5, RC6, PANASONIC64, PIONEER, SANYO_LC7461, GENERIC_RCA, None };
// NEC42/NEC42ext -> SANYO_LC7461 (42 bits); Pioneer -> PIONEER with nbits 32, not 64;
// RC5X -> RC5 with nbits 13; Kaseikyo -> PANASONIC64 with nbits 48.
struct TxFrame { TxKind kind; uint64_t data; uint16_t nbits; uint16_t khz; };

Proto        protoFromName(const char* flipperName);   // case-insensitive
const char*  protoName(Proto);
bool         toTx(const Flipper&, TxFrame& out);          // false = unsupported
bool         fromDecode(TxKind, uint64_t data, uint16_t nbits, Flipper& out); // Learn side
uint32_t     parseHexBytes(const char* s);                // "07 00 00 00" -> 0x00000007
void         formatHexBytes(uint32_t v, char* out, size_t n); // -> "07 00 00 00" (always 4 bytes)
}
```

The app maps `TxKind` to `IRsend` calls in one switch (`_txFrame`). RCA has no
native IRremoteESP8266 support and is sent through `IRsend::sendGeneric` with
the timing constants from the protocol map.

Bit-level semantics per protocol live in
`docs/app09-flipper-protocol-map.md` (generated from Flipper firmware sources
and the vendored library; every row cites its source). The codec must agree
with that document; the unit tests encode its test vectors.

**Learn-side self-check.** After decoding, the app converts
`decode_results → Flipper` with `fromDecode`, then immediately re-encodes with
`toTx`. If the re-encoded `data/nbits` differ from what the receiver reported,
the signal is stored as `type: raw` instead. A saved parsed signal is therefore
guaranteed to replay identically to the in-memory replay.

Protocols the decoder reports that have no `Proto` mapping (e.g. AC protocols,
`UNKNOWN`) are stored as raw. Raw capture clamps each timing to 65535 µs.

**Raw capture quality (added 2026-09-05 after the Hitachi RAS-22NK case).** The
IR receiver's AGC narrows its output pulses on long frames: a 280-bit
air-conditioner frame came back with bit marks shrinking from 450 µs to
100 µs and stopped after 202 bits, while mark+space per bit stayed constant.
Two safeguards therefore run on every raw capture:

- *Fade detection*: tail-mark mean / head-mark mean < 0.70 flags the capture
  FADED; the summary and a toast tell the user to retry at 20–50 cm.
- *Period-preserving normalisation*: only for UNKNOWN captures whose leading
  marks are tight (stdev ≤ 12 %) and whose spaces are bimodal (pulse-distance
  coding), every bit mark is rewritten to the leading mean and the space
  absorbs the difference so each bit period is unchanged. Pulse-width
  protocols (Sony, RC5/RC6) never match the gate. A normalised entry carries a
  Flipper-safe comment line `# meowkit: marks normalised to <M0> us`.

**Receiver limit and the Hitachi workaround (2026-09-06).** Measured on the
owner's unit with two remotes: the receiver's output marks narrow by 20–30 %
per ~165 ms of continuous carrier and vanish at ~200 ms. Protocols whose frame
is split into bursts with ≥ 25 ms gaps (Daikin: 161 + 153 bits) capture fine;
a single continuous frame longer than ~200 ms (Hitachi 264/296/344/424, some
Mitsubishi/Fujitsu variants) can never be learned on this hardware, whatever
the distance or battery. Air-conditioner remotes transmit the complete state
(power, mode, temperature, fan, swing) on every press, so a frame can be
*completed* instead of captured: `tools/complete-hitachi344.py` decodes the
captured ~25 bytes of a HITACHI_AC344 (RAS-22NK) frame, fills the tail from
the library's `IRHitachiAc344` layout, recomputes the inverted byte pairs and
writes a device file with ON/OFF/mode variants. Verified on the owner's
RAS-22NK: ON_COOL and OFF both accepted. A general solution (an IRac-based
"AC Remote" mode that synthesises frames for ~50 brands) remains an option.

### Host unit tests

`test/ir_codec/test_ir_codec.cpp` — self-contained assertions (no framework),
built by `tools/test-ir-codec.ps1` with MSVC (`vcvars64.bat` + `cl`) and run
on the host. Vectors: every VERIFIED vector from the protocol map, plus
round-trip `fromDecode(toTx(x)) == x` for each protocol, plus hex-byte
parse/format round-trips. The script exits non-zero on any failure.

## 4. Screens and input

Framework: Mooncake `AppAbility` + LovyanGFX immediate mode through
`src/app/app_common/hp_ui.h`. Lists are drawn with the hp_ui list primitives
rather than `AppUI::ListMenu` (decided during implementation: ListMenu owns
its own key polling, theme and status bar and cannot host the `[>]` options
footer). Input is the 5-way joystick plus A/B. Global: long-press B exits the
app (unchanged).

Footer convention everywhere: `[^v]` move, `[A]` primary, `[>]Opts` (only
where an options menu exists), `[B]` back. Use `drawFooter4`. Labels are
ASCII (`...` not `…`) because hp_ui measures text as `strlen * 8`.

```text
MainMenu
 ├─ Learn
 ├─ Remotes                (saved devices)
 └─ Universal (all brands)
```

### 4.1 Learn

```text
LearnWait      "Point the remote at MeowKit and press a button"   [B] back
LearnResult    slot 0: one-line summary "<Proto> · A:xx C:xx"; raw captures show
               "RAW (<DECODER_PROTO> <bits>b) · N" when the library decoded a protocol the
               codec cannot store (AC protocols), or "RAW · N samples" for UNKNOWN
               list from slot 2: Send test / Save... / Learn again   [A] select [B] back
               NEC-style repeat frames, captures with < 8 samples and captures that overflowed
               the 2048-entry receive buffer are ignored (toast), not stored
               Every capture also logs one Serial line: IR_LEARN,decode_type=,bits=,rawlen=,overflow=
SaveTarget     list: "+ New device…", then every /infrared/*.ir (stem, "N buttons")
NameEditor     (device name, only for New device)   keyboard with an [OK] key; [A] key, [OK] commits, [B] cancels
NameEditor     (button name)                        default text "BTN_<n+1>" where n = buttons already in the device
 → append → toast "Saved: <Device> / <Button>" → RemoteView(<Device>) with the new row selected
```

`Send test` sends via the exact frame the file will contain (i.e. through the
codec), not the raw decoder value, so the test is honest.

### 4.2 Remotes

```text
RemoteList     rows: <Device>  sub: "N buttons"          [A] open [>] options [B] back
               empty state: centred COL_FG text "No saved remotes" / "Use Learn to add one"
DeviceOptions  Learn new button / Rename / Delete
RemoteView     rows: <Button> sub: "<Proto> A:xx C:xx" or "RAW N"; last row "+ Learn new button"
               [A] send (toast "Sent <Button>", 600 ms, non-blocking)  [>] options  [B] back
SignalOptions  Rename / Delete
Confirm        drawDialog "Delete <X>?"  [A] yes [B] no
```

Selecting "+ Learn new button" or "Learn new button" enters LearnWait with the
device preselected; after save it returns to that RemoteView.

### 4.3 Universal (all brands)

```text
UniversalMenu  rows: category display name  sub: file name
               empty state (bright, full content area):
                 "Universal library not on SD card"
                 "Copy  sd files/infrared/universal/"
                 "from the firmware repo to"
                 "SD:/infrared/universal/"
               header shows "Universal · all brands"
UniversalCat   known category → existing fixed button grid; unknown file → list of unique names
               header "<Category> · all brands"
               [A] blast, [B] back
Blast          drawProgressPopup "<Button>" / "<i>/<total> brands · skipped <k> · gap <g>s"
               [<>] gap 0.25 s <-> 1.0 s (default 1.0 s; the value persists while the app is open)
               [B] PAUSE (not cancel) -> Identify screen
               a sweep that reaches the end shows "No more brands" and returns after 1.2 s
Identify       header "<Button> · #<i>/<total>"; body: "<Proto>  A:<addr>  C:<cmd>" (or "RAW · N samples")
               list: Resend this / Previous / Next / Save as device... / Resume sweep / Stop
               Previous/Next move the cursor one entry and send it immediately, so the user
               can step back to the code that made the device react (reaction lag) and confirm
               it with Resend.
SaveAsDevice   NameEditor prefilled "<Cat>_<Proto>_<addr hex>" (e.g. TV_NEC_04); on OK:
               collect every entry of the category file whose protocol AND address equal the
               identified entry (that is "the same remote" in a brand-less library), one per
               unique name (first occurrence wins), write /infrared/<name>.ir, then open
               RemoteView(<name>). Toast "Saved N buttons". For a RAW hit only that single
               signal can be saved ("RAW: only this button saved").
               Grid buttons carry alias lists (e.g. Power = {Power, Power_on}) so one layout
               resolves against every bundled file; every state (position, done flag, LED) is
               reset on entry.
```

Indexing: on entering a category the file is scanned **once**, in 512-byte
chunks, into an index `name → vector<file offset>` shown behind
`drawLoadingBegin/Tick`. A blast iterates the offsets and parses one entry at a
time, so memory is bounded by one signal. Entries whose protocol has no codec
mapping are counted as *skipped* and shown in the popup rather than silently
dropped. The gap between sends defaults to 1.0 s and can be toggled to 250 ms
with left/right during a sweep; TV-B-Gone keeps 250 ms.

TV-B-Gone mode (iterate every signal in `tv.ir`) is kept as the last button of
the TV category grid.

## 5. Removals and hygiene

- Delete the never-reached `IrScene::Sending`, `_runUniversalBlast`,
  `_sendLabel`, `_tvbgPaused`.
- Replace every blocking `delay(...)` message box with a timed toast handled in
  `onRunning`.
- Fix the self-aliasing `_loadRemote(_currentRemote.path, _currentRemote, …)`
  call (source and destination are the same object).
- `_listIrFiles` keeps skipping directories; both `.ir` and `.IR` are accepted.
- `IRrecv(HAL_PIN_IR_RX, 2048, 50, true)`: 2048 entries because air-conditioner
  remotes send 280–424-bit frames, often twice within the 50 ms timeout, which
  overflowed the original 1024-entry buffer and produced truncated raw captures.
  Raw carrier follows the decoder's protocol (Panasonic / DAIKIN2 36.7 kHz,
  everything else 38 kHz).

## 6. Docs to update (meowkit-s3-docs)

- `microsd-card-setup.mdx`: add a step "copy `sd files/` from the firmware repo
  to the card root", listing `/infrared/universal/`.
- `apps-capability-audit.mdx` App 09 section: new menu names and flow.

## 7. Verification

| Layer | How | Status |
|-------|-----|--------|
| Codec | `tools/test-ir-codec.ps1` host tests, all vectors pass | pending |
| Firmware | `python -m platformio run -e esp32s3box` succeeds with no new warnings in app_09 | pending |
| Review | fresh-context reviewer reads the diff against this spec | pending |
| Device | see §7.1, performed by the owner | pending |

### 7.1 Device test script (owner)

1. Flash. Open Infrared → Learn. Point a known NEC or Samsung remote, press
   Power. LearnResult shows a protocol name and `A:xx C:xx`. Note them.
2. Send test → the target device reacts.
3. Save… → + New device… → name `TESTTV` → button `POWER`. Toast appears and
   RemoteView(TESTTV) opens with POWER.
4. Exit the app, re-enter, Remotes → TESTTV → POWER → A. **The target must
   react.** (This is defect 1.)
5. In RemoteView choose "+ Learn new button", learn Volume+, name `VOL_UP`.
   TESTTV must now list 2 buttons; the SD file `/infrared/TESTTV.ir` has two
   `name:` entries. (Defect 2.)
6. Options → Rename VOL_UP → `VOLUP`; Options → Delete POWER → confirm. File
   reflects both. Rename device to `TV2`, then delete it. (Defect 3.)
7. With an empty `/infrared/universal/`, Universal shows the bright four-line
   message. Copy the folder, re-enter, choose TV → Power. The popup counts
   brands one per second; a Samsung/LG/Sony TV in range turns off. (Defect 4
   + codec.)
8. Press B the moment the TV reacts. The Identify screen shows the last code;
   use Previous / Resend to find the exact one that toggles the TV. Save as
   device… → accept the prefilled name. RemoteView opens with that brand's
   buttons; Power / Vol_up / Mute must all work on the TV. Exit and re-enter
   the app: the device is listed under Remotes.
