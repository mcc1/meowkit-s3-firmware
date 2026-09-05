/**
 * @file ir_flipper_codec.h
 * @brief Flipper Zero `.ir` (protocol, address, command) <-> IRremoteESP8266
 *        (send kind, data, nbits) codec.
 *
 * Pure C++17. No Arduino / IRremoteESP8266 headers so the module compiles and
 * is unit-tested on the host (tools/test-ir-codec.ps1). The bit-level rules
 * implemented here are documented, with sources, in
 * docs/app09-flipper-protocol-map.md; the two must agree.
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace irfc {

/* Flipper protocol names as they appear after `protocol:` in a .ir file. */
enum class Proto : uint8_t {
    NEC, NECext, NEC42, NEC42ext,
    Samsung32,
    SIRC, SIRC15, SIRC20,
    RC5, RC5X, RC6,
    Kaseikyo,
    RCA,
    Pioneer,
    Unknown
};

/* One `type: parsed` entry. address/command are the uint32 formed from the
 * four little-endian hex bytes in the file ("07 00 00 00" -> 0x00000007). */
struct Flipper {
    Proto    proto   = Proto::Unknown;
    uint32_t address = 0;
    uint32_t command = 0;
};

/* Which IRsend method the app must call. RC5X is sent with the RC5 kind and
 * nbits = 13. Kaseikyo maps to the 48-bit Panasonic frame (sendPanasonic64).
 * NEC42/NEC42ext map to the library's 42-bit SANYO_LC7461 frame
 * (sendSanyoLC7461). Pioneer is a 32-bit NEC-timed frame at 40 kHz sent with
 * sendPioneer(data, 32). RCA has no native IRremoteESP8266 support and is sent
 * with sendGeneric() + rcaTiming(). */
enum class TxKind : uint8_t {
    NEC, SAMSUNG, SONY, RC5, RC6, PANASONIC64, PIONEER, SANYO_LC7461,
    GENERIC_RCA, None
};

struct TxFrame {
    TxKind   kind  = TxKind::None;
    uint64_t data  = 0;
    uint16_t nbits = 0;
    uint16_t khz   = 38;
};

/* Mark/space timings for protocols sent through IRsend::sendGeneric(). */
struct GenericTiming {
    uint16_t hdrMark, hdrSpace;
    uint16_t oneMark, oneSpace;
    uint16_t zeroMark, zeroSpace;
    uint16_t footerMark;
    uint32_t gapUs;
    uint16_t khz;
    bool     msbFirst;
};

/* Name <-> enum. protoFromName is case-insensitive and returns Unknown for
 * anything not in the table. protoName returns the canonical Flipper spelling. */
Proto       protoFromName(const char* flipperName);
const char* protoName(Proto p);

/* Flipper -> transmit frame. Returns false (and leaves `out` untouched) when
 * the protocol is Unknown or the address/command exceed the protocol's width. */
bool toTx(const Flipper& in, TxFrame& out);

/* Receiver result -> Flipper fields (Learn). `kind` is the app's mapping of
 * decode_type_t; `nbits` disambiguates SIRC 12/15/20 and RC5 12 vs RC5X 13.
 * Returns false when the frame cannot be expressed as a Flipper protocol
 * (caller then stores the signal as raw). Must satisfy
 *   toTx(x) -> fromDecode -> == x   for every representable x. */
bool fromDecode(TxKind kind, uint64_t data, uint16_t nbits, Flipper& out);

/* Timings for TxKind::GENERIC_RCA. */
const GenericTiming& rcaTiming();

/* "07 00 00 00" -> 0x00000007. Accepts 1..4 bytes, extra whitespace, CR/LF.
 * Missing bytes are zero. */
uint32_t parseHexBytes(const char* s);

/* 0x00000007 -> "07 00 00 00" (always four bytes, upper-case, no trailing
 * newline). outSize must be >= 12. */
void formatHexBytes(uint32_t value, char* out, size_t outSize);

}  // namespace irfc
