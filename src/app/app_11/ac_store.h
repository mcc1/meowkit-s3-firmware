/**
 * @file ac_store.h
 * @brief App11 AC Remote - `/ac/<Name>.cfg` key=value parse / serialise.
 *
 * Pure C++17: no Arduino, no SD, no IRremoteESP8266 headers, so this module
 * joins the MSVC host tests (tools/test-ir-codec.ps1). The app converts the
 * plain struct below to and from `stdAc::state_t`; the enum values below are
 * deliberately identical to the matching `stdAc` enumerators so the
 * conversion is a cast, not a table.
 *
 * On-disk grammar (docs/app11-ac-remote.md section 3), one `key: value` per line:
 *
 *     protocol: HITACHI_AC344
 *     model: -1
 *     power: on
 *     mode: cool
 *     temp: 23
 *     fan: auto
 *     swingv: off
 *     swingh: off
 *     quiet: off
 *     turbo: off
 *     econo: off
 *     light: off
 *
 * Reader rules: CR is tolerated (CRLF files), leading/trailing blanks are
 * trimmed, keys are case-insensitive, unknown keys are ignored, a missing key
 * keeps its default and an unparsable value falls back to that same default.
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace acstore {

/* -- Field values (same numbering as stdAc::opmode_t / fanspeed_t / swing) -- */
constexpr int8_t kModeOff    = -1;
constexpr int8_t kModeAuto   =  0;
constexpr int8_t kModeCool   =  1;
constexpr int8_t kModeHeat   =  2;
constexpr int8_t kModeDry    =  3;
constexpr int8_t kModeFan    =  4;

constexpr int8_t kFanAuto    =  0;
constexpr int8_t kFanMin     =  1;
constexpr int8_t kFanLow     =  2;
constexpr int8_t kFanMedium  =  3;
constexpr int8_t kFanHigh    =  4;
constexpr int8_t kFanMax     =  5;

constexpr int8_t kSwingOff   = -1;
constexpr int8_t kSwingAuto  =  0;

/* The app's own range; IRac clamps further to whatever the protocol allows. */
constexpr int8_t kTempMin    = 16;
constexpr int8_t kTempMax    = 30;
constexpr int8_t kTempDefault = 25;

/* Longest decode_type_t name is MITSUBISHI_HEAVY_152 (20 chars); anything
 * longer in a hand-edited file is truncated here and will not resolve to a
 * protocol, which the app reports rather than silently guessing. */
constexpr size_t kProtocolLen = 24;
constexpr size_t kFileBufSize = 512;  /* a serialised device always fits     */

/* One air-conditioner as it lives on the card (name = file stem, not here). */
struct Device {
    char    protocol[kProtocolLen] = {0};
    int16_t model  = -1;              /* -1 = protocol has no models / default */
    bool    power  = false;
    int8_t  mode   = kModeAuto;
    int8_t  temp   = kTempDefault;
    int8_t  fan    = kFanAuto;
    int8_t  swingv = kSwingOff;
    int8_t  swingh = kSwingOff;
    bool    quiet  = false;
    bool    turbo  = false;
    bool    econo  = false;
    bool    light  = false;
};

/* -- Value tokens ---------------------------------------------------------
 * *Name() spells the on-disk token (lowercase), *Label() the UI word. Both
 * return a static string and never null. */
const char* modeName(int8_t mode);
const char* fanName(int8_t fan);
const char* swingName(int8_t swing);
const char* boolName(bool value);

const char* modeLabel(int8_t mode);
const char* fanLabel(int8_t fan);

/* Parse one token; `def` is returned for anything unrecognised. */
int8_t parseMode(const char* text, int8_t def = kModeAuto);
int8_t parseFan(const char* text, int8_t def = kFanAuto);
int8_t parseSwing(const char* text, int8_t def = kSwingOff);
bool   parseBool(const char* text, bool def = false);

/* Clamp to kTempMin..kTempMax. */
int8_t clampTemp(long celsius);

/* -- File body ------------------------------------------------------------
 * parse() starts from a default-constructed Device, so every key the text
 * omits keeps its default. Returns false only when `text` is null. */
bool parse(const char* text, size_t len, Device& out);

/* Writes the full file body (a header comment plus every key) and
 * NUL-terminates it. The header warns the reader that a save from the
 * handheld rewrites the file from the struct above, so keys this firmware
 * does not know are dropped. Returns the number of bytes written, 0 when the
 * buffer is too small. */
size_t serialise(const Device& device, char* out, size_t outSize);

/* The comment block serialise() puts at the top of every file. */
extern const char kFileHeader[];

}  // namespace acstore
