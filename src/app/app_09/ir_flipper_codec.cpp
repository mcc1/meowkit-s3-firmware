/**
 * @file ir_flipper_codec.cpp
 * @brief Flipper Zero `.ir` (type: parsed) <-> IRremoteESP8266 codec.
 *
 * Every bit-level rule here is taken from docs/app09-flipper-protocol-map.md
 * (which in turn cites the Flipper firmware sources and the vendored
 * IRremoteESP8266 tree). The module deliberately includes no Arduino and no
 * library header, so the maths the library performs in its encode*() helpers is
 * re-implemented below; each re-implementation cites the library file:line it
 * mirrors. Line numbers refer to lib/IRremoteESP8266/ (library.json 2.8.6,
 * git head 3390e728).
 *
 * Two facts everything rests on (map 0.1 / 0.2):
 *   - Flipper transmits its encoder buffer LSB-of-byte-0 first.
 *   - IRremoteESP8266 transmits `data` MSB-first.
 *   => the library `data` word is the Flipper wire bit stream read big-endian,
 *      i.e. reverseBits() applied per field, in field order.
 */
#include "ir_flipper_codec.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace irfc {

namespace {

/* ------------------------------------------------------------------ *
 * reverseBits -- faithful re-implementation of
 * lib/IRremoteESP8266/src/IRutils.cpp:46-58. Note the library re-appends the
 * bits above `nbits` on top of the reversed field, so every call site below
 * masks its input first (map 0.2).
 * ------------------------------------------------------------------ */
inline uint64_t rb(uint64_t input, uint16_t nbits) {
    if (nbits <= 1) return input;                 // IRutils.cpp:47
    if (nbits > 64) nbits = 64;                   // IRutils.cpp:49
    uint64_t output = 0;
    for (uint16_t i = 0; i < nbits; i++) {        // IRutils.cpp:51-55
        output <<= 1;
        output |= (input & 1);
        input >>= 1;
    }
    return (input << nbits) | output;             // IRutils.cpp:57
}

/* Field widths as the Flipper writer enforces them --
 * FZ/lib/infrared/signal/infrared_signal.c:51-72 masks address to
 * infrared_get_protocol_address_length() bits and command to
 * ...command_length() bits and rejects anything wider (map 0.4 and the 1
 * master table). `khz` is the Flipper carrier for that protocol. */
struct Width { uint8_t addrBits; uint8_t cmdBits; uint16_t khz; };

/* Indexed by Proto (declaration order in ir_flipper_codec.h). */
constexpr Width kWidth[] = {
    /* NEC       */ { 8,  8,  38 },
    /* NECext    */ { 16, 16, 38 },
    /* NEC42     */ { 13, 8,  38 },
    /* NEC42ext  */ { 26, 16, 38 },
    /* Samsung32 */ { 8,  8,  38 },
    /* SIRC      */ { 5,  7,  40 },
    /* SIRC15    */ { 8,  7,  40 },
    /* SIRC20    */ { 13, 7,  40 },
    /* RC5       */ { 5,  6,  36 },
    /* RC5X      */ { 5,  7,  36 },  /* writer allows 7, encoder keeps 6: 3.10 */
    /* RC6       */ { 8,  8,  36 },
    /* Kaseikyo  */ { 26, 10, 38 },
    /* RCA       */ { 4,  8,  38 },
    /* Pioneer   */ { 8,  8,  40 },
};
constexpr size_t kProtoCount = sizeof(kWidth) / sizeof(kWidth[0]);

struct NameRow { const char* name; Proto proto; };
constexpr NameRow kNames[] = {
    { "NEC",       Proto::NEC       }, { "NECext",   Proto::NECext   },
    { "NEC42",     Proto::NEC42     }, { "NEC42ext", Proto::NEC42ext },
    { "Samsung32", Proto::Samsung32 },
    { "SIRC",      Proto::SIRC      }, { "SIRC15",   Proto::SIRC15   },
    { "SIRC20",    Proto::SIRC20    },
    { "RC5",       Proto::RC5       }, { "RC5X",     Proto::RC5X     },
    { "RC6",       Proto::RC6       },
    { "Kaseikyo",  Proto::Kaseikyo  },
    { "RCA",       Proto::RCA       },
    { "Pioneer",   Proto::Pioneer   },
};

bool ieq(const char* a, const char* b) {
    for (;; ++a, ++b) {
        int ca = std::tolower(static_cast<unsigned char>(*a));
        int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
}

inline bool fits(uint32_t v, uint8_t bits) {
    return bits >= 32 || (v >> bits) == 0;
}

/* Kaseikyo wire bytes d[0..5] -- verbatim from
 * FZ/lib/infrared/encoder_decoder/kaseikyo/infrared_encoder_kaseikyo.c:13-25
 * (map 3.12). `addr` is 26 bits, `cmd` 10 bits. */
void kaseikyoBytes(uint32_t addr, uint32_t cmd, uint8_t d[6]) {
    const uint8_t  id     = static_cast<uint8_t>((addr >> 24) & 3);
    const uint16_t vendor = static_cast<uint16_t>((addr >> 8) & 0xFFFF);
    const uint8_t  genre1 = static_cast<uint8_t>((addr >> 4) & 0xF);
    const uint8_t  genre2 = static_cast<uint8_t>(addr & 0xF);
    d[0] = static_cast<uint8_t>(vendor & 0xFF);
    d[1] = static_cast<uint8_t>(vendor >> 8);
    uint8_t vp = static_cast<uint8_t>(d[0] ^ d[1]);
    vp = static_cast<uint8_t>((vp & 0xF) ^ (vp >> 4));
    d[2] = static_cast<uint8_t>((vp & 0xF) | (genre1 << 4));
    d[3] = static_cast<uint8_t>((genre2 & 0xF) | ((cmd & 0xF) << 4));
    d[4] = static_cast<uint8_t>((id << 6) | ((cmd >> 4) & 0x3F));
    d[5] = static_cast<uint8_t>(d[2] ^ d[3] ^ d[4]);
}

}  // namespace

Proto protoFromName(const char* flipperName) {
    if (!flipperName) return Proto::Unknown;
    for (const auto& row : kNames)
        if (ieq(row.name, flipperName)) return row.proto;
    return Proto::Unknown;
}

const char* protoName(Proto p) {
    for (const auto& row : kNames)
        if (row.proto == p) return row.name;
    return "Unknown";
}

/* ====================================================================== *
 * Send direction -- map sections 1 / 2 / 3.
 * ====================================================================== */
bool toTx(const Flipper& in, TxFrame& out) {
    const size_t idx = static_cast<size_t>(in.proto);
    if (in.proto == Proto::Unknown || idx >= kProtoCount) return false;

    const Width w = kWidth[idx];
    if (!fits(in.address, w.addrBits) || !fits(in.command, w.cmdBits))
        return false;

    TxFrame f;
    f.khz = w.khz;

    /* Shorthands used by the NEC family / Samsung / RCA / Pioneer. */
    const uint64_t rA8 = rb(in.address & 0xFF, 8);
    const uint64_t rC8 = rb(in.command & 0xFF, 8);

    switch (in.proto) {
    case Proto::NEC:
        /* 3.1 -- mirrors IRsend::encodeNEC()'s `else` branch,
         * lib/IRremoteESP8266/src/ir_NEC.cpp:48-60. */
        f.kind  = TxKind::NEC;
        f.nbits = 32;
        f.data  = (rA8 << 24) | ((rA8 ^ 0xFF) << 16) | (rC8 << 8) | (rC8 ^ 0xFF);
        break;

    case Proto::NECext:
        /* 3.2 -- encodeNEC() must NOT be used: ir_NEC.cpp:49-52 truncates the
         * command to 8 bits and fabricates an inverse. */
        f.kind  = TxKind::NEC;
        f.nbits = 32;
        f.data  = (rb(in.address & 0xFFFF, 16) << 16) | rb(in.command & 0xFFFF, 16);
        break;

    case Proto::NEC42: {
        /* 3.3 -- the layout IRsend::encodeSanyoLC7461() builds at
         * ir_Sanyo.cpp:98-114, fed with pre-reversed fields. */
        const uint64_t rA13 = rb(in.address & 0x1FFF, 13);
        f.kind  = TxKind::SANYO_LC7461;
        f.nbits = 42;
        f.data  = (rA13 << 29) | ((rA13 ^ 0x1FFF) << 16) | (rC8 << 8) | (rC8 ^ 0xFF);
        break;
    }

    case Proto::NEC42ext:
        /* 3.4 -- no library helper. */
        f.kind  = TxKind::SANYO_LC7461;
        f.nbits = 42;
        f.data  = (rb(in.address & 0x3FFFFFF, 26) << 16) | rb(in.command & 0xFFFF, 16);
        break;

    case Proto::Samsung32:
        /* 3.5 -- mirrors IRsend::encodeSAMSUNG(), ir_Samsung.cpp:110-115. */
        f.kind  = TxKind::SAMSUNG;
        f.nbits = 32;
        f.data  = (rA8 << 24) | (rA8 << 16) | (rC8 << 8) | (rC8 ^ 0xFF);
        break;

    case Proto::SIRC:
    case Proto::SIRC15:
    case Proto::SIRC20: {
        /* 3.6-3.8 -- mirrors IRsend::encodeSony(), ir_Sony.cpp:89-108:
         * result = (address << 7) | (command & 0x7F), then
         * reverseBits(result, nbits). The library splits SIRC20's 13-bit
         * address into address(5) + extended(8); `address | extended << 5` is
         * the same 13-bit value. */
        const uint16_t n    = (in.proto == Proto::SIRC)   ? 12u
                            : (in.proto == Proto::SIRC15) ? 15u : 20u;
        const uint32_t mask = (in.proto == Proto::SIRC)   ? 0x1Fu
                            : (in.proto == Proto::SIRC15) ? 0xFFu : 0x1FFFu;
        f.kind  = TxKind::SONY;
        f.nbits = n;
        f.data  = rb((static_cast<uint64_t>(in.address & mask) << 7) |
                     (in.command & 0x7F), n);
        break;
    }

    case Proto::RC5:
        /* 3.9 -- mirrors IRsend::encodeRC5(), ir_RC5_RC6.cpp:115-119, with the
         * toggle (bit 11) left at 0: `.ir` files carry no toggle bit and the
         * app owns that state. Address/command go out MSB-first here. */
        f.kind  = TxKind::RC5;
        f.nbits = 12;
        f.data  = (static_cast<uint64_t>(in.address & 0x1F) << 6) | (in.command & 0x3F);
        break;

    case Proto::RC5X:
        /* 3.10 -- Flipper's RC5X always has field bit 0 on the wire, which the
         * library models as data bit 12 = 1 (ir_RC5_RC6.cpp:69-73 inverts it;
         * encodeRC5X() at :127-134 sets it from command bit 6). Equivalent to
         * encodeRC5X(A, (C & 0x3F) | 0x40, T).
         * Flipper's encoder keeps only 6 command bits
         * (FZ/.../rc5/infrared_encoder_rc5.c:26), so command bit 6 is dropped
         * here on purpose -- reproducing the Flipper, not fixing it (map 3.10
         * and the 8 "Flipper RC5X drops command bit 6" row). */
        f.kind  = TxKind::RC5;
        f.nbits = 13;
        f.data  = 0x1000ULL | (static_cast<uint64_t>(in.address & 0x1F) << 6) |
                  (in.command & 0x3F);
        break;

    case Proto::RC6:
        /* 3.11 -- mode 0, toggle 0. Mirrors IRsend::encodeRC6(address, command,
         * kRC6Mode0Bits) at ir_RC5_RC6.cpp:171-182 where `address` carries the
         * field/mode/toggle bits: ((addr & 0xFFF) << 8) | (cmd & 0xFF). */
        f.kind  = TxKind::RC6;
        f.nbits = 20;
        f.data  = (static_cast<uint64_t>(in.address & 0xFF) << 8) | (in.command & 0xFF);
        break;

    case Proto::Kaseikyo: {
        /* 3.12 -- build Flipper's six wire bytes, then read them MSB-first.
         * Equivalent to IRsend::encodePanasonic() (ir_Panasonic.cpp:104-113)
         * with every field pre-reversed. */
        uint8_t d[6];
        kaseikyoBytes(in.address & 0x3FFFFFF, in.command & 0x3FF, d);
        uint64_t v = 0;
        for (int i = 0; i < 6; ++i) v = (v << 8) | rb(d[i], 8);
        f.kind  = TxKind::PANASONIC64;
        f.nbits = 48;
        f.data  = v;
        break;
    }

    case Proto::RCA: {
        /* 3.13 -- no RCA in decode_type_t; the app sends this through
         * IRsend::sendGeneric() with rcaTiming(). Wire = A(4) C(8) !A(4) !C(8),
         * each field lsb-first. */
        const uint64_t rA4  = rb(in.address & 0x0F, 4);
        const uint64_t rnA4 = rb((~in.address) & 0x0F, 4);
        const uint64_t rnC8 = rb((~in.command) & 0xFF, 8);
        f.kind  = TxKind::GENERIC_RCA;
        f.nbits = 24;
        f.data  = (rA4 << 20) | (rC8 << 12) | (rnA4 << 8) | rnC8;
        break;
    }

    case Proto::Pioneer:
        /* 3.14 -- a single NEC-shaped 32-bit frame at 40 kHz.
         * IRsend::encodePioneer() (ir_Pioneer.cpp:82-85) is the WRONG helper:
         * it builds the 64-bit two-sub-code form from 16-bit published codes. */
        f.kind  = TxKind::PIONEER;
        f.nbits = 32;
        f.data  = (rA8 << 24) | ((rA8 ^ 0xFF) << 16) | (rC8 << 8) | (rC8 ^ 0xFF);
        break;

    default:
        return false;
    }

    out = f;
    return true;
}

/* ====================================================================== *
 * Learn direction -- map section 6. `data` is the receiver's raw MSB-first
 * frame (decode_results::value), never its address/command fields: only
 * SAMSUNG, RC5 and plain NEC hand those back in Flipper coordinates (6.2 #1).
 * ====================================================================== */
bool fromDecode(TxKind kind, uint64_t data, uint16_t nbits, Flipper& out) {
    Flipper f;

    switch (kind) {
    case TxKind::NEC: {
        if (nbits != 32) return false;
        const uint8_t b3 = static_cast<uint8_t>((data >> 24) & 0xFF);
        const uint8_t b2 = static_cast<uint8_t>((data >> 16) & 0xFF);
        const uint8_t b1 = static_cast<uint8_t>((data >>  8) & 0xFF);
        const uint8_t b0 = static_cast<uint8_t>(data & 0xFF);
        /* Flipper's decoder picks NEC only when BOTH inversions hold
         * (FZ/.../nec/infrared_decoder_nec.c:26-32, map 3.2); otherwise the
         * frame is NECext. A NEC-shaped NECext value is therefore not
         * representable -- documented aliasing, map section 8. */
        if (static_cast<uint8_t>(b3 ^ b2) == 0xFF &&
            static_cast<uint8_t>(b1 ^ b0) == 0xFF) {
            f.proto   = Proto::NEC;
            f.address = static_cast<uint32_t>(rb(b3, 8));
            f.command = static_cast<uint32_t>(rb(b1, 8));
        } else {
            f.proto   = Proto::NECext;
            f.address = static_cast<uint32_t>(rb((data >> 16) & 0xFFFF, 16));
            f.command = static_cast<uint32_t>(rb(data & 0xFFFF, 16));
        }
        break;
    }

    case TxKind::SANYO_LC7461: {
        /* 6.2 #5 -- decodeSanyoLC7461 overwrites address/command with the
         * *unreversed* raw fields (ir_Sanyo.cpp:163-168, 184-186), so work from
         * the frame. Same self-inverting split for NEC42 vs NEC42ext (3.3/3.4). */
        if (nbits != 42) return false;
        const uint32_t a13 = static_cast<uint32_t>((data >> 29) & 0x1FFF);
        const uint32_t i13 = static_cast<uint32_t>((data >> 16) & 0x1FFF);
        const uint8_t  c8  = static_cast<uint8_t>((data >> 8) & 0xFF);
        const uint8_t  i8  = static_cast<uint8_t>(data & 0xFF);
        if ((a13 ^ i13) == 0x1FFF && static_cast<uint8_t>(c8 ^ i8) == 0xFF) {
            f.proto   = Proto::NEC42;
            f.address = static_cast<uint32_t>(rb(a13, 13));
            f.command = static_cast<uint32_t>(rb(c8, 8));
        } else {
            f.proto   = Proto::NEC42ext;
            f.address = static_cast<uint32_t>(rb((data >> 16) & 0x3FFFFFF, 26));
            f.command = static_cast<uint32_t>(rb(data & 0xFFFF, 16));
        }
        break;
    }

    case TxKind::SAMSUNG: {
        /* 3.5 -- wire is A A C !C; refuse anything else, because no Flipper
         * Samsung32 signal can produce it. */
        if (nbits != 32) return false;
        const uint8_t b3 = static_cast<uint8_t>((data >> 24) & 0xFF);
        const uint8_t b2 = static_cast<uint8_t>((data >> 16) & 0xFF);
        const uint8_t b1 = static_cast<uint8_t>((data >>  8) & 0xFF);
        const uint8_t b0 = static_cast<uint8_t>(data & 0xFF);
        if (b3 != b2 || static_cast<uint8_t>(b1 ^ b0) != 0xFF) return false;
        f.proto   = Proto::Samsung32;
        f.address = static_cast<uint32_t>(rb(b3, 8));
        f.command = static_cast<uint32_t>(rb(b1, 8));
        break;
    }

    case TxKind::SONY: {
        /* Section 6 -- undo encodeSony(): d = reverseBits(value, nbits) is
         * (address << 7) | command. Width picks the Flipper variant; the
         * library's results.address/command are unusable for SIRC20 (6.2 #1). */
        if (nbits != 12 && nbits != 15 && nbits != 20) return false;
        const uint64_t d = rb(data & ((1ULL << nbits) - 1), nbits);
        f.proto   = (nbits == 12) ? Proto::SIRC
                  : (nbits == 15) ? Proto::SIRC15 : Proto::SIRC20;
        f.address = static_cast<uint32_t>(d >> 7);
        f.command = static_cast<uint32_t>(d & 0x7F);
        break;
    }

    case TxKind::RC5:
        /* Section 6 / 3.10 -- decodeRC5 (ir_RC5_RC6.cpp:310-370) sets is_rc5x
         * when the wire field bit is 0 and seeds `data = 1`, so an RC5X capture
         * always reports 13 bits with bit 12 set and an RC5 capture 12 bits.
         * Flipper stores only 6 command bits for RC5X (6.2 #4). */
        if (nbits == 12) {
            f.proto = Proto::RC5;
        } else if (nbits == 13) {
            if (((data >> 12) & 1) == 0) return false;  /* field bit 1 => RC5 */
            f.proto = Proto::RC5X;
        } else {
            return false;
        }
        f.address = static_cast<uint32_t>((data >> 6) & 0x1F);
        f.command = static_cast<uint32_t>(data & 0x3F);
        break;

    case TxKind::RC6:
        /* 6.2 #3 -- results.address is mode(3)<<9 | toggle<<8 | addr(8);
         * Flipper implements mode 0 only
         * (FZ/.../rc6/infrared_decoder_rc6.c:26-28). */
        if (nbits != 20) return false;
        if (((data >> 17) & 0x7) != 0) return false;   /* mode != 0 */
        f.proto   = Proto::RC6;
        f.address = static_cast<uint32_t>((data >> 8) & 0xFF);
        f.command = static_cast<uint32_t>(data & 0xFF);
        break;

    case TxKind::PANASONIC64: {
        /* 6.1 -- results.address/command are the manufacturer and the low 32
         * bits (ir_Panasonic.cpp:154-172); rebuild from the six wire bytes and
         * verify both Flipper integrity fields
         * (FZ/.../kaseikyo/infrared_decoder_kaseikyo.c:20-24). */
        if (nbits != 48) return false;
        uint8_t d[6];
        for (int i = 0; i < 6; ++i)
            d[i] = static_cast<uint8_t>(rb((data >> (8 * (5 - i))) & 0xFF, 8));
        uint8_t vp = static_cast<uint8_t>(d[0] ^ d[1]);
        vp = static_cast<uint8_t>((vp & 0xF) ^ (vp >> 4));
        if ((d[2] & 0xF) != vp) return false;
        if (d[5] != static_cast<uint8_t>(d[2] ^ d[3] ^ d[4])) return false;
        const uint16_t vendor = static_cast<uint16_t>((d[1] << 8) | d[0]);
        const uint8_t  g1 = static_cast<uint8_t>(d[2] >> 4);
        const uint8_t  g2 = static_cast<uint8_t>(d[3] & 0xF);
        const uint8_t  id = static_cast<uint8_t>(d[4] >> 6);
        f.proto   = Proto::Kaseikyo;
        f.address = (static_cast<uint32_t>(id) << 24) |
                    (static_cast<uint32_t>(vendor) << 8) |
                    static_cast<uint32_t>((g1 << 4) | g2);
        f.command = static_cast<uint32_t>(d[3] >> 4) |
                    (static_cast<uint32_t>(d[4] & 0x3F) << 4);
        break;
    }

    case TxKind::PIONEER: {
        /* 3.14 -- Flipper's Pioneer is one NEC-shaped 32-bit frame.
         * (6.2 #6: a real capture usually surfaces as NEC instead, because
         * IRrecv cannot measure the 40 kHz carrier. That is the app's problem,
         * not the codec's.) */
        if (nbits != 32) return false;
        const uint8_t b3 = static_cast<uint8_t>((data >> 24) & 0xFF);
        const uint8_t b2 = static_cast<uint8_t>((data >> 16) & 0xFF);
        const uint8_t b1 = static_cast<uint8_t>((data >>  8) & 0xFF);
        const uint8_t b0 = static_cast<uint8_t>(data & 0xFF);
        if (static_cast<uint8_t>(b3 ^ b2) != 0xFF ||
            static_cast<uint8_t>(b1 ^ b0) != 0xFF) return false;
        f.proto   = Proto::Pioneer;
        f.address = static_cast<uint32_t>(rb(b3, 8));
        f.command = static_cast<uint32_t>(rb(b1, 8));
        break;
    }

    case TxKind::GENERIC_RCA:
        /* 3.13 / 6.2 #7 / section 8 -- IRremoteESP8266 2.8.6 has no RCA
         * decoder; captures fall through to decodeHash. Nothing to express. */
        return false;

    case TxKind::None:
    default:
        return false;
    }

    out = f;
    return true;
}

const GenericTiming& rcaTiming() {
    /* map 3.13, from FZ/.../rca/infrared_protocol_rca_i.h:5-12. 38 kHz is what
     * the Flipper actually emits (infrared_protocol_rca.c:28) even though the
     * canonical RCA spec says 56 kHz. */
    static const GenericTiming t{
        /* hdrMark    */ 4000, /* hdrSpace  */ 4000,
        /* oneMark    */  500, /* oneSpace  */ 2000,
        /* zeroMark   */  500, /* zeroSpace */ 1000,
        /* footerMark */  500, /* gapUs     */ 8000,
        /* khz        */   38, /* msbFirst  */ true
    };
    return t;
}

uint32_t parseHexBytes(const char* s) {
    uint32_t v = 0;
    int byteIdx = 0;
    while (s && *s && byteIdx < 4) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
        if (!*s) break;
        char* end = nullptr;
        unsigned long b = std::strtoul(s, &end, 16);
        if (end == s) break;
        v |= static_cast<uint32_t>(b & 0xFF) << (8 * byteIdx);
        ++byteIdx;
        s = end;
    }
    return v;
}

void formatHexBytes(uint32_t value, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    std::snprintf(out, outSize, "%02X %02X %02X %02X",
                  static_cast<unsigned>(value & 0xFF),
                  static_cast<unsigned>((value >> 8) & 0xFF),
                  static_cast<unsigned>((value >> 16) & 0xFF),
                  static_cast<unsigned>((value >> 24) & 0xFF));
}

}  // namespace irfc
