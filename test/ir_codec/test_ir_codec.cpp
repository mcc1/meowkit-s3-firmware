/**
 * @file test_ir_codec.cpp
 * @brief Host unit tests for src/app/app_09/ir_flipper_codec.cpp.
 *
 * Self-contained: no gtest, no Unity. Build+run with
 *   pwsh -File tools/test-ir-codec.ps1
 * Exit code 0 = every assertion passed.
 *
 * Vectors are the 24 VERIFIED rows of docs/app09-flipper-protocol-map.md
 * section 4, plus round-trip / property / range / name / hex tests, plus the
 * raw-capture helpers of src/app/app_09/ir_raw_tools.cpp (AGC fade detection
 * and period-preserving mark normalisation), plus the App11 AC Remote config
 * codec of src/app/app_11/ac_store.cpp (docs/app11-ac-remote.md section 3).
 */
#include "ir_flipper_codec.h"
#include "ir_raw_tools.h"
#include "ac_store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace irfc;

/* ---------------------------------------------------------------- checks */
static long long g_checks = 0;
static long long g_fails = 0;
static const long long kMaxPrinted = 25;

static void reportFail(const char* file, int line, const char* what,
                       unsigned long long got, unsigned long long want,
                       const char* ctx) {
    ++g_fails;
    if (g_fails > kMaxPrinted) return;
    std::printf("%s:%d: FAIL %s%s%s  expected 0x%llX (%llu), actual 0x%llX (%llu)\n",
                file, line, what, ctx ? " | " : "", ctx ? ctx : "",
                want, want, got, got);
    if (g_fails == kMaxPrinted)
        std::printf("  ... further failures suppressed ...\n");
}

#define CHECK_EQ(got, want, ctx)                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        unsigned long long g_ = (unsigned long long)(got);                    \
        unsigned long long w_ = (unsigned long long)(want);                   \
        if (g_ != w_) reportFail(__FILE__, __LINE__, #got " == " #want, g_, w_, (ctx)); \
    } while (0)

#define CHECK_TRUE(cond, ctx)                                                 \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) reportFail(__FILE__, __LINE__, #cond, 0, 1, (ctx));      \
    } while (0)

#define CHECK_FALSE(cond, ctx)                                                \
    do {                                                                      \
        ++g_checks;                                                           \
        if ((cond)) reportFail(__FILE__, __LINE__, "!(" #cond ")", 1, 0, (ctx)); \
    } while (0)

/* ------------------------------------------------------- map section 4 */
struct Vec {
    const char* id;        /* row number + protocol, for failure context */
    Proto       proto;
    uint32_t    address;
    uint32_t    command;
    TxKind      kind;
    uint64_t    data;
    uint16_t    nbits;
    uint16_t    khz;
};

static const Vec kVectors[] = {
    /*  1 */ { "#1 NEC LG Power",        Proto::NEC,       0x04,       0x08,  TxKind::NEC,          0x20DF10EFull,   32, 38 },
    /*  2 */ { "#2 NEC FZT",             Proto::NEC,       0x00,       0x02,  TxKind::NEC,          0x00FF40BFull,   32, 38 },
    /*  3 */ { "#3 NECext FZT",          Proto::NECext,    0x7984,     0xED12,TxKind::NEC,          0x219E48B7ull,   32, 38 },
    /*  4 */ { "#4 NEC42 LIB",           Proto::NEC42,     0x0589,     0x6A,  TxKind::SANYO_LC7461, 0x2468DCB56A9ull,42, 38 },
    /*  5 */ { "#5 NEC42 zero",          Proto::NEC42,     0x0000,     0x00,  TxKind::SANYO_LC7461, 0x1FFF00FFull,   42, 38 },
    /*  6 */ { "#6 NEC42ext FZT",        Proto::NEC42ext,  0x00,       0x00,  TxKind::SANYO_LC7461, 0x0ull,          42, 38 },
    /*  7 */ { "#7 Samsung32 Power",     Proto::Samsung32, 0x07,       0x02,  TxKind::SAMSUNG,      0xE0E040BFull,   32, 38 },
    /*  8 */ { "#8 Samsung32 FZT",       Proto::Samsung32, 0x0E,       0x0C,  TxKind::SAMSUNG,      0x707030CFull,   32, 38 },
    /*  9 */ { "#9 SIRC Sony Power",     Proto::SIRC,      0x01,       0x15,  TxKind::SONY,         0xA90ull,        12, 40 },
    /* 10 */ { "#10 SIRC FZT",           Proto::SIRC,      0x0A,       0x55,  TxKind::SONY,         0xAAAull,        12, 40 },
    /* 11 */ { "#11 SIRC15",             Proto::SIRC15,    0x01,       0x15,  TxKind::SONY,         0x5480ull,       15, 40 },
    /* 12 */ { "#12 SIRC20 LIB",         Proto::SIRC20,    0x0021,     0x01,  TxKind::SONY,         0x81080ull,      20, 40 },
    /* 13 */ { "#13 SIRC20 FZT",         Proto::SIRC20,    0x0FB5,     0x53,  TxKind::SONY,         0xCB5BEull,      20, 40 },
    /* 14 */ { "#14 RC5 FZT",            Proto::RC5,       0x13,       0x10,  TxKind::RC5,          0x4D0ull,        12, 36 },
    /* 15 */ { "#15 RC5 LIB",            Proto::RC5,       0x05,       0x35,  TxKind::RC5,          0x175ull,        12, 36 },
    /* 16 */ { "#16 RC5X FZT",           Proto::RC5X,      0x13,       0x10,  TxKind::RC5,          0x14D0ull,       13, 36 },
    /* 17 */ { "#17 RC6 FZT decoder",    Proto::RC6,       0x94,       0xA0,  TxKind::RC6,          0x94A0ull,       20, 36 },
    /* 18 */ { "#18 RC6 FZT encoder",    Proto::RC6,       0x93,       0xA0,  TxKind::RC6,          0x93A0ull,       20, 36 },
    /* 19 */ { "#19 RC6 LIB",            Proto::RC6,       0x01,       0x75,  TxKind::RC6,          0x175ull,        20, 36 },
    /* 20 */ { "#20 Kaseikyo LIB",       Proto::Kaseikyo,  0x02200289, 0x370, TxKind::PANASONIC64,  0x40040190ED7Cull,48,38 },
    /* 21 */ { "#21 Kaseikyo FZT",       Proto::Kaseikyo,  0x00325441, 0x01B, TxKind::PANASONIC64,  0x2A4C028D800Full,48,38 },
    /* 22 */ { "#22 RCA FZT",            Proto::RCA,       0x0F,       0x54,  TxKind::GENERIC_RCA,  0xF2A0D5ull,     24, 38 },
    /* 23 */ { "#23 Pioneer LIB",        Proto::Pioneer,   0xAF,       0x20,  TxKind::PIONEER,      0xF50A04FBull,   32, 40 },
    /* 24 */ { "#24 Pioneer FZT",        Proto::Pioneer,   0xAF,       0x36,  TxKind::PIONEER,      0xF50A6C93ull,   32, 40 },
};
static const size_t kVectorCount = sizeof(kVectors) / sizeof(kVectors[0]);

static void testVectorsForward() {
    for (size_t i = 0; i < kVectorCount; ++i) {
        const Vec& v = kVectors[i];
        Flipper in;
        in.proto = v.proto;
        in.address = v.address;
        in.command = v.command;
        TxFrame tx;
        CHECK_TRUE(toTx(in, tx), v.id);
        CHECK_EQ(static_cast<unsigned>(tx.kind), static_cast<unsigned>(v.kind), v.id);
        CHECK_EQ(tx.data, v.data, v.id);
        CHECK_EQ(tx.nbits, v.nbits, v.id);
        CHECK_EQ(tx.khz, v.khz, v.id);
    }
}

static void testVectorsRoundTrip() {
    for (size_t i = 0; i < kVectorCount; ++i) {
        const Vec& v = kVectors[i];
        Flipper back;
        const bool ok = fromDecode(v.kind, v.data, v.nbits, back);
        if (v.kind == TxKind::GENERIC_RCA) {
            /* map 3.13 / 6.2 #7: no RCA decoder exists in IRremoteESP8266. */
            CHECK_FALSE(ok, v.id);
            continue;
        }
        CHECK_TRUE(ok, v.id);
        CHECK_EQ(static_cast<unsigned>(back.proto), static_cast<unsigned>(v.proto), v.id);
        CHECK_EQ(back.address, v.address, v.id);
        CHECK_EQ(back.command, v.command, v.id);
    }
}

/* --------------------------------------------------------- property sweep */
static long long g_aliased = 0;

/* Sweep a protocol over a grid of legal (address, command) values and assert
 * toTx -> fromDecode is the identity. `aStep`/`cStep` of 1 means exhaustive. */
static void sweep(Proto p, uint32_t aMax, uint32_t aStep,
                  uint32_t cMax, uint32_t cStep) {
    const char* ctx = protoName(p);
    for (uint64_t a = 0; a <= aMax; a += aStep) {
        for (uint64_t c = 0; c <= cMax; c += cStep) {
            Flipper in;
            in.proto = p;
            in.address = static_cast<uint32_t>(a);
            in.command = static_cast<uint32_t>(c);
            TxFrame tx;
            if (!toTx(in, tx)) { CHECK_TRUE(false, ctx); continue; }
            CHECK_TRUE(tx.nbits > 0 && tx.khz > 0, ctx);
            Flipper back;
            if (p == Proto::RCA) {
                /* No decoder; only the send direction is defined. */
                CHECK_FALSE(fromDecode(tx.kind, tx.data, tx.nbits, back), ctx);
                continue;
            }
            if (!fromDecode(tx.kind, tx.data, tx.nbits, back)) {
                CHECK_TRUE(false, ctx);
                continue;
            }
            /* Documented aliasing (map 3.2 and section 8): an ext frame whose
             * bytes happen to be self-inverting is indistinguishable from the
             * short form and always reads back as the short form. */
            if ((p == Proto::NECext   && back.proto == Proto::NEC) ||
                (p == Proto::NEC42ext && back.proto == Proto::NEC42)) {
                ++g_aliased;
                continue;
            }
            CHECK_EQ(static_cast<unsigned>(back.proto), static_cast<unsigned>(p), ctx);
            CHECK_EQ(back.address, static_cast<uint32_t>(a), ctx);
            CHECK_EQ(back.command, static_cast<uint32_t>(c), ctx);
        }
    }
}

static void testPropertySweeps() {
    sweep(Proto::NEC,       0xFF,       1,      0xFF,   1);       /* exhaustive */
    sweep(Proto::NECext,    0xFFFF,     199,    0xFFFF, 199);
    sweep(Proto::NEC42,     0x1FFF,     7,      0xFF,   1);
    sweep(Proto::NEC42ext,  0x3FFFFFF,  131071, 0xFFFF, 199);
    sweep(Proto::Samsung32, 0xFF,       1,      0xFF,   1);       /* exhaustive */
    sweep(Proto::SIRC,      0x1F,       1,      0x7F,   1);       /* exhaustive */
    sweep(Proto::SIRC15,    0xFF,       1,      0x7F,   1);       /* exhaustive */
    sweep(Proto::SIRC20,    0x1FFF,     1,      0x7F,   1);       /* exhaustive */
    sweep(Proto::RC5,       0x1F,       1,      0x3F,   1);       /* exhaustive */
    /* RC5X: only 6 command bits reach the wire (map 3.10), so the identity
     * domain is C <= 0x3F. Bit 6 is checked separately below. */
    sweep(Proto::RC5X,      0x1F,       1,      0x3F,   1);
    sweep(Proto::RC6,       0xFF,       1,      0xFF,   1);       /* exhaustive */
    sweep(Proto::Kaseikyo,  0x3FFFFFF,  65521,  0x3FF,  1);
    sweep(Proto::RCA,       0x0F,       1,      0xFF,   1);       /* exhaustive */
    sweep(Proto::Pioneer,   0xFF,       1,      0xFF,   1);       /* exhaustive */
}

/* --------------------------------------------------------- range / refusal */
static bool txFails(Proto p, uint32_t a, uint32_t c) {
    Flipper in;
    in.proto = p;
    in.address = a;
    in.command = c;
    TxFrame tx;
    tx.kind = TxKind::None;
    tx.data = 0xDEADBEEFull;
    tx.nbits = 4242;
    const bool ok = toTx(in, tx);
    if (!ok) {
        /* Contract in ir_flipper_codec.h:72-73 -- `out` is left untouched. */
        CHECK_EQ(static_cast<unsigned>(tx.kind), static_cast<unsigned>(TxKind::None), "untouched");
        CHECK_EQ(tx.data, 0xDEADBEEFull, "untouched");
        CHECK_EQ(tx.nbits, 4242u, "untouched");
    }
    return !ok;
}

static void testOutOfRange() {
    CHECK_TRUE(txFails(Proto::Unknown,   0, 0),          "Unknown");
    CHECK_TRUE(txFails(Proto::NEC,       0x100, 0x00),   "NEC addr");
    CHECK_TRUE(txFails(Proto::NEC,       0x00, 0x100),   "NEC cmd");
    CHECK_TRUE(txFails(Proto::NECext,    0x10000, 0),    "NECext addr");
    CHECK_TRUE(txFails(Proto::NECext,    0, 0x10000),    "NECext cmd");
    CHECK_TRUE(txFails(Proto::NEC42,     0x2000, 0),     "NEC42 addr");
    CHECK_TRUE(txFails(Proto::NEC42,     0, 0x100),      "NEC42 cmd");
    CHECK_TRUE(txFails(Proto::NEC42ext,  0x4000000, 0),  "NEC42ext addr");
    CHECK_TRUE(txFails(Proto::NEC42ext,  0, 0x10000),    "NEC42ext cmd");
    CHECK_TRUE(txFails(Proto::Samsung32, 0x100, 0),      "Samsung32 addr");
    CHECK_TRUE(txFails(Proto::Samsung32, 0, 0x100),      "Samsung32 cmd");
    CHECK_TRUE(txFails(Proto::SIRC,      0x20, 0),       "SIRC addr");
    CHECK_TRUE(txFails(Proto::SIRC,      0, 0x80),       "SIRC cmd");
    CHECK_TRUE(txFails(Proto::SIRC15,    0x100, 0),      "SIRC15 addr");
    CHECK_TRUE(txFails(Proto::SIRC20,    0x2000, 0),     "SIRC20 addr");
    CHECK_TRUE(txFails(Proto::RC5,       0x20, 0),       "RC5 addr");
    CHECK_TRUE(txFails(Proto::RC5,       0, 0x40),       "RC5 cmd");
    CHECK_TRUE(txFails(Proto::RC5X,      0x20, 0),       "RC5X addr");
    CHECK_TRUE(txFails(Proto::RC5X,      0, 0x80),       "RC5X cmd");
    CHECK_TRUE(txFails(Proto::RC6,       0x100, 0),      "RC6 addr");
    CHECK_TRUE(txFails(Proto::RC6,       0, 0x100),      "RC6 cmd");
    CHECK_TRUE(txFails(Proto::Kaseikyo,  0x4000000, 0),  "Kaseikyo addr");
    CHECK_TRUE(txFails(Proto::Kaseikyo,  0, 0x400),      "Kaseikyo cmd");
    CHECK_TRUE(txFails(Proto::RCA,       0x10, 0),       "RCA addr");
    CHECK_TRUE(txFails(Proto::RCA,       0, 0x100),      "RCA cmd");
    CHECK_TRUE(txFails(Proto::Pioneer,   0x100, 0),      "Pioneer addr");
    CHECK_TRUE(txFails(Proto::Pioneer,   0, 0x100),      "Pioneer cmd");

    /* Largest legal value of each field must still succeed. */
    CHECK_TRUE(!txFails(Proto::NEC42ext, 0x3FFFFFF, 0xFFFF), "NEC42ext max");
    CHECK_TRUE(!txFails(Proto::Kaseikyo, 0x3FFFFFF, 0x3FF),  "Kaseikyo max");
    CHECK_TRUE(!txFails(Proto::SIRC20,   0x1FFF, 0x7F),      "SIRC20 max");
}

static void testFromDecodeRefusals() {
    Flipper f;
    CHECK_FALSE(fromDecode(TxKind::None, 0, 0, f),              "None");
    CHECK_FALSE(fromDecode(TxKind::GENERIC_RCA, 0xF2A0D5, 24, f), "GENERIC_RCA");
    /* Wrong widths. */
    CHECK_FALSE(fromDecode(TxKind::NEC, 0x20DF10EF, 42, f),     "NEC/42");
    CHECK_FALSE(fromDecode(TxKind::SANYO_LC7461, 0, 32, f),     "SANYO/32");
    CHECK_FALSE(fromDecode(TxKind::SAMSUNG, 0xE0E040BF, 20, f), "SAMSUNG/20");
    CHECK_FALSE(fromDecode(TxKind::SONY, 0xA90, 13, f),         "SONY/13");
    CHECK_FALSE(fromDecode(TxKind::RC5, 0x4D0, 14, f),          "RC5/14");
    CHECK_FALSE(fromDecode(TxKind::RC6, 0x94A0, 36, f),         "RC6/36");
    CHECK_FALSE(fromDecode(TxKind::PANASONIC64, 0, 32, f),      "PANASONIC64/32");
    CHECK_FALSE(fromDecode(TxKind::PIONEER, 0, 64, f),          "PIONEER/64");
    /* RC5X with the library's field bit clear cannot be an RC5X capture
     * (ir_RC5_RC6.cpp:334 seeds data = 1 whenever is_rc5x). */
    CHECK_FALSE(fromDecode(TxKind::RC5, 0x04D0, 13, f),         "RC5X bit12 clear");
    /* Samsung frame that is not A A C !C. */
    CHECK_FALSE(fromDecode(TxKind::SAMSUNG, 0xE0E140BF, 32, f), "SAMSUNG addr mismatch");
    CHECK_FALSE(fromDecode(TxKind::SAMSUNG, 0xE0E040BE, 32, f), "SAMSUNG cmd inverse");
    /* Pioneer frame that is not NEC-shaped. */
    CHECK_FALSE(fromDecode(TxKind::PIONEER, 0x219E48B7, 32, f), "PIONEER not self-inverting");
    /* RC6 with mode != 0 (map 6.2 #3): data bits 19-17 carry the mode. */
    CHECK_FALSE(fromDecode(TxKind::RC6, 0x294A0, 20, f),        "RC6 mode 1");
    /* Kaseikyo with a broken XOR checksum byte and a broken vendor parity. */
    /* Vector #20 is d[] = 02 20 80 09 B7 3E. Bumping d[5] to 0x3F breaks only
     * the XOR byte; d[2] -> 0x81 with a matching d[5] = 0x3F breaks only the
     * vendor-parity nibble. */
    CHECK_FALSE(fromDecode(TxKind::PANASONIC64, 0x40040190EDFCull, 48, f), "Kaseikyo checksum");
    CHECK_FALSE(fromDecode(TxKind::PANASONIC64, 0x40048190EDFCull, 48, f), "Kaseikyo vendor parity");
    /* ...and the intact frame still decodes. */
    CHECK_TRUE(fromDecode(TxKind::PANASONIC64, 0x40040190ED7Cull, 48, f), "Kaseikyo intact");
}

/* ------------------------------------------------------- protocol details */
static void testProtocolDetails() {
    Flipper in;
    TxFrame tx;

    /* map 3.10 -- Flipper's RC5X encoder drops command bit 6, so 0x50 must
     * transmit exactly like 0x10 (reproduce, do not fix). */
    in.proto = Proto::RC5X; in.address = 0x13; in.command = 0x50;
    CHECK_TRUE(toTx(in, tx), "RC5X bit6");
    CHECK_EQ(tx.data, 0x14D0ull, "RC5X bit6 dropped");
    CHECK_EQ(tx.nbits, 13u, "RC5X nbits");

    /* map 3.3 vs 3.4 self-proof: NEC42 re-expressed as NEC42ext with
     * A26 = A13 | (~A13 << 13), C16 = C8 | (~C8 << 8) yields the same frame. */
    in.proto = Proto::NEC42; in.address = 0x0589; in.command = 0x6A;
    TxFrame t42;
    CHECK_TRUE(toTx(in, t42), "NEC42 selfproof");
    in.proto = Proto::NEC42ext;
    in.address = 0x0589u | ((~0x0589u & 0x1FFFu) << 13);
    in.command = 0x6Au | ((~0x6Au & 0xFFu) << 8);
    TxFrame t42e;
    CHECK_TRUE(toTx(in, t42e), "NEC42ext selfproof");
    CHECK_EQ(t42e.data, t42.data, "NEC42 == NEC42ext restatement");

    /* map 3.12 -- Panasonic's own vendor_id 0x2002 must appear as the
     * library's documented manufacturer constant 0x4004. */
    in.proto = Proto::Kaseikyo; in.address = 0x00200200u; in.command = 0;
    CHECK_TRUE(toTx(in, tx), "Kaseikyo vendor");
    CHECK_EQ((tx.data >> 32) & 0xFFFF, 0x4004ull, "manufacturer 0x4004");

    /* map 3.13 -- RCA generic timings. */
    const GenericTiming& t = rcaTiming();
    CHECK_EQ(t.hdrMark, 4000u, "rca hdrMark");
    CHECK_EQ(t.hdrSpace, 4000u, "rca hdrSpace");
    CHECK_EQ(t.oneMark, 500u, "rca oneMark");
    CHECK_EQ(t.oneSpace, 2000u, "rca oneSpace");
    CHECK_EQ(t.zeroMark, 500u, "rca zeroMark");
    CHECK_EQ(t.zeroSpace, 1000u, "rca zeroSpace");
    CHECK_EQ(t.footerMark, 500u, "rca footerMark");
    CHECK_EQ(t.gapUs, 8000u, "rca gap");
    CHECK_EQ(t.khz, 38u, "rca khz (Flipper emits 38, not the 56 kHz spec)");
    CHECK_TRUE(t.msbFirst, "rca msbFirst");

    /* Carrier frequency per protocol (map section 1). */
    struct KhzRow { Proto p; uint16_t khz; };
    static const KhzRow kKhz[] = {
        { Proto::NEC, 38 },   { Proto::NECext, 38 }, { Proto::NEC42, 38 },
        { Proto::NEC42ext, 38 }, { Proto::Samsung32, 38 },
        { Proto::SIRC, 40 },  { Proto::SIRC15, 40 }, { Proto::SIRC20, 40 },
        { Proto::RC5, 36 },   { Proto::RC5X, 36 },   { Proto::RC6, 36 },
        { Proto::Kaseikyo, 38 }, { Proto::RCA, 38 }, { Proto::Pioneer, 40 },
    };
    for (size_t i = 0; i < sizeof(kKhz) / sizeof(kKhz[0]); ++i) {
        Flipper q;
        q.proto = kKhz[i].p;
        q.address = 0;
        q.command = 0;
        TxFrame qt;
        CHECK_TRUE(toTx(q, qt), protoName(kKhz[i].p));
        CHECK_EQ(qt.khz, kKhz[i].khz, protoName(kKhz[i].p));
    }
}

/* ------------------------------------------------------------ name table */
static void testNames() {
    static const Proto kAll[] = {
        Proto::NEC, Proto::NECext, Proto::NEC42, Proto::NEC42ext,
        Proto::Samsung32, Proto::SIRC, Proto::SIRC15, Proto::SIRC20,
        Proto::RC5, Proto::RC5X, Proto::RC6, Proto::Kaseikyo,
        Proto::RCA, Proto::Pioneer
    };
    for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
        const char* n = protoName(kAll[i]);
        CHECK_EQ(static_cast<unsigned>(protoFromName(n)),
                 static_cast<unsigned>(kAll[i]), n);
    }
    CHECK_EQ(static_cast<unsigned>(protoFromName("samsung32")),
             static_cast<unsigned>(Proto::Samsung32), "lower");
    CHECK_EQ(static_cast<unsigned>(protoFromName("NECEXT")),
             static_cast<unsigned>(Proto::NECext), "upper");
    CHECK_EQ(static_cast<unsigned>(protoFromName("nEc42ExT")),
             static_cast<unsigned>(Proto::NEC42ext), "mixed");
    CHECK_EQ(static_cast<unsigned>(protoFromName("rc5x")),
             static_cast<unsigned>(Proto::RC5X), "rc5x");
    CHECK_EQ(static_cast<unsigned>(protoFromName("PIONEER")),
             static_cast<unsigned>(Proto::Pioneer), "PIONEER");
    CHECK_EQ(static_cast<unsigned>(protoFromName("Sony")),
             static_cast<unsigned>(Proto::Unknown), "Sony is not a Flipper name");
    CHECK_EQ(static_cast<unsigned>(protoFromName("")),
             static_cast<unsigned>(Proto::Unknown), "empty");
    CHECK_EQ(static_cast<unsigned>(protoFromName(nullptr)),
             static_cast<unsigned>(Proto::Unknown), "null");
    CHECK_EQ(static_cast<unsigned>(protoFromName("NEC ")),
             static_cast<unsigned>(Proto::Unknown), "trailing space");
    CHECK_TRUE(std::strcmp(protoName(Proto::Unknown), "Unknown") == 0, "Unknown name");
    CHECK_TRUE(std::strcmp(protoName(Proto::Samsung32), "Samsung32") == 0, "canonical spelling");
    CHECK_TRUE(std::strcmp(protoName(Proto::NECext), "NECext") == 0, "canonical spelling");
}

/* ------------------------------------------------------------ hex helpers */
static void checkFormat(uint32_t v, const char* want) {
    char buf[16];
    std::memset(buf, 'x', sizeof(buf));
    formatHexBytes(v, buf, sizeof(buf));
    ++g_checks;
    if (std::strcmp(buf, want) != 0) {
        ++g_fails;
        if (g_fails <= kMaxPrinted)
            std::printf("%s:%d: FAIL formatHexBytes(0x%08lX)  expected \"%s\", actual \"%s\"\n",
                        __FILE__, __LINE__, static_cast<unsigned long>(v), want, buf);
    }
}

static void testHexBytes() {
    CHECK_EQ(parseHexBytes("07 00 00 00"), 0x00000007u, "parse 7");
    CHECK_EQ(parseHexBytes("89 02 20 02"), 0x02200289u, "parse kaseikyo addr");
    CHECK_EQ(parseHexBytes("84 79 00 00"), 0x00007984u, "parse necext addr");
    CHECK_EQ(parseHexBytes("12 ED 00 00"), 0x0000ED12u, "parse necext cmd");
    CHECK_EQ(parseHexBytes("FF FF FF FF"), 0xFFFFFFFFu, "parse all ones");
    CHECK_EQ(parseHexBytes("07 00 00 00\r\n"), 0x00000007u, "trailing CRLF");
    CHECK_EQ(parseHexBytes("  07   00\t00 00  "), 0x00000007u, "extra whitespace");
    CHECK_EQ(parseHexBytes("07"), 0x00000007u, "one byte");
    CHECK_EQ(parseHexBytes("41 54"), 0x00005441u, "two bytes (legacy)");
    CHECK_EQ(parseHexBytes("41 54 32"), 0x00325441u, "three bytes");
    CHECK_EQ(parseHexBytes(""), 0u, "empty");
    CHECK_EQ(parseHexBytes(nullptr), 0u, "null");
    CHECK_EQ(parseHexBytes("\r\n"), 0u, "whitespace only");
    CHECK_EQ(parseHexBytes("0f 00 00 00"), 0x0000000Fu, "lower-case hex");

    checkFormat(0x02200289u, "89 02 20 02");
    checkFormat(0x00000007u, "07 00 00 00");
    checkFormat(0x00007984u, "84 79 00 00");
    checkFormat(0x00000000u, "00 00 00 00");
    checkFormat(0xFFFFFFFFu, "FF FF FF FF");
    checkFormat(0xDEADBEEFu, "EF BE AD DE");

    /* Round trip both ways over every vector's fields. */
    for (size_t i = 0; i < kVectorCount; ++i) {
        char buf[16];
        formatHexBytes(kVectors[i].address, buf, sizeof(buf));
        CHECK_EQ(parseHexBytes(buf), kVectors[i].address, kVectors[i].id);
        formatHexBytes(kVectors[i].command, buf, sizeof(buf));
        CHECK_EQ(parseHexBytes(buf), kVectors[i].command, kVectors[i].id);
        /* Flipper always writes four bytes -- map section 7. */
        CHECK_EQ(std::strlen(buf), 11u, kVectors[i].id);
    }
}

/* ------------------------------------------------------- ir_raw_tools */

/* Absolute-tolerance comparison for the floating-point results; the reporter
 * takes integers, so both sides are shown scaled by 10000. */
#define CHECK_NEAR(got, want, tol, ctx)                                       \
    do {                                                                      \
        ++g_checks;                                                           \
        double d_ = (double)(got) - (double)(want);                           \
        if (d_ < 0) d_ = -d_;                                                 \
        if (d_ > (double)(tol))                                               \
            reportFail(__FILE__, __LINE__, #got " ~= " #want,                 \
                       (unsigned long long)((double)(got) * 10000.0),         \
                       (unsigned long long)((double)(want) * 10000.0), (ctx));\
    } while (0)

/* 100 bits whose marks decay linearly 450 -> 153 us while each bit period
 * stays 900 us (a zero) or 1700 us (a one): the Hitachi RAS-22NK failure
 * mode, reproduced synthetically. */
static size_t buildDecayingPulseDistance(uint16_t* out) {
    size_t n = 0;
    out[n++] = 3400; out[n++] = 1700;               /* header */
    for (int i = 0; i < 100; ++i) {
        const uint16_t mark   = (uint16_t)(450 - 3 * i);
        const uint16_t period = (uint16_t)((i % 2 == 0) ? 900 : 1700);
        out[n++] = mark;
        out[n++] = (uint16_t)(period - mark);
    }
    return n;                                        /* 202 entries */
}

static void testRawTools() {
    uint16_t buf[256];

    /* 1. The decay is detected. The head/tail windows are 15 % of the 100 bit
     *    marks = 15 samples, giving headMean 429 and tailMean 174. */
    size_t n = buildDecayingPulseDistance(buf);
    irraw::FadeInfo fade = irraw::analyseFade(buf, n);
    CHECK_EQ(fade.bitMarks, 100, "decay: bit mark count");
    CHECK_NEAR(fade.headMean, 429.0, 0.01, "decay: head mean");
    CHECK_NEAR(fade.tailMean, 174.0, 0.01, "decay: tail mean");
    CHECK_NEAR(fade.ratio, 0.4056, 0.001, "decay: fade ratio");
    CHECK_TRUE(fade.faded, "decay: flagged as faded");
    /* The first mark under 0.75 x 429 = 321.75 us is bit 43 (321 us); the
     * timings before it are 5100 us of header + 22 x 900 + 21 x 1700. */
    CHECK_EQ(fade.fadeUs, 60600u, "decay: time of first faded mark");

    /* 2. Normalising it yields one constant mark and untouched bit periods. */
    uint16_t before[256];
    std::memcpy(before, buf, n * sizeof(uint16_t));
    uint16_t m0 = 0;
    CHECK_TRUE(irraw::canNormalise(buf, n, m0), "decay: qualifies for normalising");
    CHECK_EQ(m0, 422u, "decay: measured clean mark");
    const int changed = irraw::normaliseMarks(buf, n, m0);
    CHECK_EQ(changed, 100, "decay: every bit pair rewritten");
    CHECK_EQ(buf[0], 3400u, "decay: header mark untouched");
    CHECK_EQ(buf[1], 1700u, "decay: header space untouched");
    for (size_t i = 2; i + 1 < n; i += 2) {
        CHECK_EQ(buf[i], m0, "decay: mark is constant after normalising");
        CHECK_EQ((unsigned)(buf[i] + buf[i + 1]),
                 (unsigned)(before[i] + before[i + 1]), "decay: period preserved");
    }

    /* 3. A pulse-width coding (Sony: 600/1200 us marks, constant 600 us
     *    spaces) carries its bits in the mark and must never be normalised. */
    n = 0;
    buf[n++] = 2400; buf[n++] = 600;
    for (int i = 0; i < 60; ++i) {
        buf[n++] = (uint16_t)((i % 2) ? 1200 : 600);
        buf[n++] = 600;
    }
    m0 = 0;
    CHECK_FALSE(irraw::canNormalise(buf, n, m0), "sony: pulse width refused");
    CHECK_EQ(m0, 0u, "sony: no mark reported");

    /* 3b. Tight marks with a single space cluster are not pulse distance. */
    n = 0;
    buf[n++] = 2400; buf[n++] = 600;
    for (int i = 0; i < 60; ++i) { buf[n++] = 600; buf[n++] = 600; }
    CHECK_FALSE(irraw::canNormalise(buf, n, m0), "unimodal spaces refused");

    /* 4. A healthy NEC-like capture (constant 560 us marks) is not faded. */
    n = 0;
    buf[n++] = 9000; buf[n++] = 4500;
    for (int i = 0; i < 60; ++i) {
        buf[n++] = 560;
        buf[n++] = (uint16_t)((i % 3) ? 1690 : 560);
    }
    buf[n++] = 560;                                  /* trailing mark */
    fade = irraw::analyseFade(buf, n);
    CHECK_EQ(fade.bitMarks, 60, "healthy: bit mark count");
    CHECK_NEAR(fade.ratio, 1.0, 0.0001, "healthy: no decay");
    CHECK_FALSE(fade.faded, "healthy: not flagged");
    CHECK_EQ(fade.fadeUs, 0u, "healthy: no fade point");
}


/* ------------------------------------------------------- app_11 ac_store */

/* String equality with the same failure reporting style as CHECK_EQ. */
#define CHECK_STR(got, want, ctx)                                             \
    do {                                                                      \
        ++g_checks;                                                           \
        const char* gs_ = (got);                                              \
        const char* ws_ = (want);                                             \
        if (std::strcmp(gs_, ws_) != 0) {                                     \
            ++g_fails;                                                        \
            if (g_fails <= kMaxPrinted)                                       \
                std::printf("%s:%d: FAIL %s | %s  expected \"%s\", actual \"%s\"\n", \
                            __FILE__, __LINE__, #got, (ctx), ws_, gs_);       \
        }                                                                     \
    } while (0)

static void testAcStore() {
    using namespace acstore;

    /* 1. Round trip of a full file: the spec's own example (section 3),
     *    preceded by the header comment serialise() writes. */
    {
        const char kFile[] =
            "# MeowKit AC Remote device. Saving from the handheld rewrites this file\n"
            "# from the keys below; any other key you add here is discarded.\n"
            "protocol: HITACHI_AC344\n"
            "model: -1\n"
            "power: on\n"
            "mode: cool\n"
            "temp: 23\n"
            "fan: auto\n"
            "swingv: off\n"
            "swingh: off\n"
            "quiet: off\n"
            "turbo: off\n"
            "econo: off\n"
            "light: off\n";
        Device d;
        CHECK_TRUE(parse(kFile, sizeof(kFile) - 1, d), "example: parsed");
        CHECK_STR(d.protocol, "HITACHI_AC344", "example: protocol");
        CHECK_EQ(d.model, (int16_t)-1, "example: model");
        CHECK_TRUE(d.power, "example: power on");
        CHECK_EQ(d.mode, kModeCool, "example: mode cool");
        CHECK_EQ(d.temp, 23, "example: temp");
        CHECK_EQ(d.fan, kFanAuto, "example: fan auto");
        CHECK_EQ(d.swingv, kSwingOff, "example: swingv off");
        CHECK_EQ(d.swingh, kSwingOff, "example: swingh off");

        char buf[kFileBufSize];
        const size_t n = serialise(d, buf, sizeof(buf));
        CHECK_EQ(n, sizeof(kFile) - 1, "example: serialised length");
        CHECK_STR(buf, kFile, "example: byte-identical round trip");

        Device again;
        CHECK_TRUE(parse(buf, n, again), "example: reparsed");
        CHECK_STR(again.protocol, d.protocol, "reparse: protocol");
        CHECK_EQ(again.mode, d.mode, "reparse: mode");
        CHECK_EQ(again.temp, d.temp, "reparse: temp");
        CHECK_EQ(again.power, d.power, "reparse: power");

        /* The header is a comment, so a file written by hand without it
         * parses to exactly the same device. */
        const char* headerless = std::strstr(kFile, "protocol:");
        CHECK_TRUE(headerless != nullptr, "example: header is separable");
        Device bare;
        parse(headerless, std::strlen(headerless), bare);
        CHECK_STR(bare.protocol, d.protocol, "headerless: protocol");
        CHECK_EQ(bare.mode, d.mode, "headerless: mode");
        CHECK_EQ(bare.temp, d.temp, "headerless: temp");
        CHECK_TRUE(std::strstr(kFile, kFileHeader) == kFile,
                   "example: file opens with kFileHeader");
    }

    /* 2. A fully-populated non-default state also survives the round trip. */
    {
        Device d;
        std::snprintf(d.protocol, sizeof(d.protocol), "%s", "DAIKIN216");
        d.model = 3; d.power = true; d.mode = kModeHeat; d.temp = 30;
        d.fan = kFanMax; d.swingv = kSwingAuto; d.swingh = kSwingAuto;
        d.quiet = true; d.turbo = false; d.econo = true; d.light = true;

        char buf[kFileBufSize];
        const size_t n = serialise(d, buf, sizeof(buf));
        CHECK_TRUE(n > 0, "daikin: serialised");
        Device r;
        CHECK_TRUE(parse(buf, n, r), "daikin: reparsed");
        CHECK_STR(r.protocol, "DAIKIN216", "daikin: protocol");
        CHECK_EQ(r.model, (int16_t)3, "daikin: model");
        CHECK_EQ(r.mode, kModeHeat, "daikin: mode");
        CHECK_EQ(r.temp, 30, "daikin: temp");
        CHECK_EQ(r.fan, kFanMax, "daikin: fan");
        CHECK_EQ(r.swingv, kSwingAuto, "daikin: swingv");
        CHECK_EQ(r.swingh, kSwingAuto, "daikin: swingh");
        CHECK_TRUE(r.quiet,  "daikin: quiet");
        CHECK_FALSE(r.turbo, "daikin: turbo");
        CHECK_TRUE(r.econo,  "daikin: econo");
        CHECK_TRUE(r.light,  "daikin: light");
    }

    /* 3. Missing keys keep their defaults (only "protocol:" is present). */
    {
        const char kMinimal[] = "protocol: COOLIX\n";
        Device d;
        d.temp = 19;                       /* pre-set fields must be reset too */
        CHECK_TRUE(parse(kMinimal, sizeof(kMinimal) - 1, d), "minimal: parsed");
        CHECK_STR(d.protocol, "COOLIX", "minimal: protocol");
        CHECK_EQ(d.model, (int16_t)-1, "minimal: model default");
        CHECK_FALSE(d.power, "minimal: power default off");
        CHECK_EQ(d.mode, kModeAuto, "minimal: mode default auto");
        CHECK_EQ(d.temp, kTempDefault, "minimal: temp default");
        CHECK_EQ(d.fan, kFanAuto, "minimal: fan default auto");
        CHECK_EQ(d.swingv, kSwingOff, "minimal: swingv default off");
        CHECK_EQ(d.swingh, kSwingOff, "minimal: swingh default off");
        CHECK_FALSE(d.quiet || d.turbo || d.econo || d.light, "minimal: toggles off");
    }

    /* 4. Temperature is clamped to the app's 16..30 window, both ends. */
    {
        const char kHot[]  = "temp: 45\n";
        const char kCold[] = "temp: -7\n";
        Device hot, cold;
        parse(kHot,  sizeof(kHot) - 1,  hot);
        parse(kCold, sizeof(kCold) - 1, cold);
        CHECK_EQ(hot.temp,  kTempMax, "clamp: 45 -> 30");
        CHECK_EQ(cold.temp, kTempMin, "clamp: -7 -> 16");
        CHECK_EQ(clampTemp(30), kTempMax, "clamp: 30 stays");
        CHECK_EQ(clampTemp(16), kTempMin, "clamp: 16 stays");
        CHECK_EQ(clampTemp(22), 22,       "clamp: in range untouched");
    }

    /* 5. Unknown keys are ignored and never disturb the keys around them. */
    {
        const char kExtra[] =
            "protocol: GREE\n"
            "sleep: 90\n"
            "timer: 21\n"
            "# a comment line\n"
            "not a pair at all\n"
            "temp: 27\n"
            "beep: on\n"
            "mode: dry\n";
        Device d;
        CHECK_TRUE(parse(kExtra, sizeof(kExtra) - 1, d), "unknown: parsed");
        CHECK_STR(d.protocol, "GREE", "unknown: protocol survived");
        CHECK_EQ(d.temp, 27, "unknown: temp survived");
        CHECK_EQ(d.mode, kModeDry, "unknown: mode survived");
        CHECK_FALSE(d.power, "unknown: power untouched");
    }

    /* 6. CRLF files (edited on a PC) parse exactly like LF ones, and a file
     *    with no trailing newline still yields its last key. */
    {
        const char kCrlf[] =
            "protocol: MIDEA\r\n"
            "power: on\r\n"
            "mode: heat\r\n"
            "temp: 21\r\n"
            "fan: high\r\n"
            "swingv: auto";               /* no trailing newline on purpose */
        Device d;
        CHECK_TRUE(parse(kCrlf, sizeof(kCrlf) - 1, d), "crlf: parsed");
        CHECK_STR(d.protocol, "MIDEA", "crlf: protocol has no stray CR");
        CHECK_TRUE(d.power, "crlf: power");
        CHECK_EQ(d.mode, kModeHeat, "crlf: mode");
        CHECK_EQ(d.temp, 21, "crlf: temp");
        CHECK_EQ(d.fan, kFanHigh, "crlf: fan");
        CHECK_EQ(d.swingv, kSwingAuto, "crlf: last key without newline");
    }

    /* 7. A value the reader does not know falls back to the default; it never
     *    corrupts the field nor aborts the rest of the file. */
    {
        const char kBad[] =
            "protocol: SHARP_AC\n"
            "mode: banana\n"
            "fan: hurricane\n"
            "swingv: sideways\n"
            "power: maybe\n"
            "temp: warm\n"
            "quiet: on\n";
        Device d;
        CHECK_TRUE(parse(kBad, sizeof(kBad) - 1, d), "bad enum: parsed");
        CHECK_EQ(d.mode, kModeAuto, "bad enum: mode -> auto");
        CHECK_EQ(d.fan, kFanAuto, "bad enum: fan -> auto");
        CHECK_EQ(d.swingv, kSwingOff, "bad enum: swingv -> off");
        CHECK_EQ(d.swingh, kSwingOff, "bad enum: swingh default kept");
        CHECK_FALSE(d.power, "bad enum: power -> off");
        CHECK_EQ(d.temp, kTempDefault, "bad enum: non-numeric temp -> default");
        CHECK_TRUE(d.quiet, "bad enum: later keys still read");
    }

    /* 7b. A bad swingh token does not disturb a good swingv on the same file,
     *     and vice versa: each axis falls back on its own. */
    {
        const char kMixed[] =
            "protocol: DAIKIN\n"
            "swingv: auto\n"
            "swingh: diagonal\n";
        Device d;
        CHECK_TRUE(parse(kMixed, sizeof(kMixed) - 1, d), "swingh: parsed");
        CHECK_EQ(d.swingv, kSwingAuto, "swingh: good swingv survives");
        CHECK_EQ(d.swingh, kSwingOff,  "swingh: bad token -> default off");

        const char kMixed2[] = "swingv: upwards\nswingh: auto\n";
        Device e;
        parse(kMixed2, sizeof(kMixed2) - 1, e);
        CHECK_EQ(e.swingv, kSwingOff,  "swingv: bad token -> default off");
        CHECK_EQ(e.swingh, kSwingAuto, "swingv: good swingh survives");
    }

    /* 7c. `mode: off` is a real value (stdAc::opmode_t::kOff) and must survive
     *     a Device-level round trip, power state included. */
    {
        Device d;
        std::snprintf(d.protocol, sizeof(d.protocol), "%s", "PANASONIC_AC");
        d.mode = kModeOff;
        d.power = false;
        d.temp = 18;
        d.fan = kFanLow;
        char buf[kFileBufSize];
        const size_t n = serialise(d, buf, sizeof(buf));
        CHECK_TRUE(n > 0, "mode off: serialised");
        CHECK_TRUE(std::strstr(buf, "mode: off\n") != nullptr, "mode off: token written");
        Device r;
        CHECK_TRUE(parse(buf, n, r), "mode off: reparsed");
        CHECK_EQ(r.mode, kModeOff, "mode off: round trip");
        CHECK_FALSE(r.power, "mode off: power");
        CHECK_EQ(r.temp, 18, "mode off: temp");
        CHECK_EQ(r.fan, kFanLow, "mode off: fan");
        CHECK_STR(r.protocol, "PANASONIC_AC", "mode off: protocol");
    }

    /* 7d. A protocol name longer than the field truncates safely: no overrun,
     *     still NUL-terminated, and the truncated text is not a real protocol
     *     name, so the app resolves it to UNKNOWN and refuses to send. */
    {
        const char kLong[] =
            "protocol: MITSUBISHI_HEAVY_152_EXTENDED_EDITION\n"
            "temp: 24\n";
        Device d;
        CHECK_TRUE(parse(kLong, sizeof(kLong) - 1, d), "long protocol: parsed");
        CHECK_EQ(std::strlen(d.protocol), kProtocolLen - 1, "long protocol: truncated");
        CHECK_EQ((int)d.protocol[kProtocolLen - 1], 0, "long protocol: terminated");
        CHECK_TRUE(std::strncmp(d.protocol, "MITSUBISHI_HEAVY_152_EXT",
                                kProtocolLen - 1) == 0,
                   "long protocol: keeps the leading bytes");
        CHECK_EQ(d.temp, 24, "long protocol: following keys still read");

        /* And it survives serialise() without growing the field. */
        char buf[kFileBufSize];
        const size_t n = serialise(d, buf, sizeof(buf));
        Device r;
        parse(buf, n, r);
        CHECK_STR(r.protocol, d.protocol, "long protocol: stable round trip");
    }

    /* 8. Token tables: every name round-trips through its parser, the UI
     *    labels are never empty, and keys/values are case-insensitive. */
    {
        const int8_t modes[] = { kModeOff, kModeAuto, kModeCool, kModeHeat,
                                 kModeDry, kModeFan };
        for (int8_t m : modes) {
            CHECK_EQ(parseMode(modeName(m), kModeAuto), m, "token: mode round trip");
            CHECK_TRUE(modeLabel(m)[0] != '\0', "token: mode label non-empty");
        }
        const int8_t fans[] = { kFanAuto, kFanMin, kFanLow, kFanMedium,
                                kFanHigh, kFanMax };
        for (int8_t f : fans) {
            CHECK_EQ(parseFan(fanName(f), kFanAuto), f, "token: fan round trip");
            CHECK_TRUE(fanLabel(f)[0] != '\0', "token: fan label non-empty");
        }
        CHECK_EQ(parseSwing(swingName(kSwingOff),  kSwingAuto), kSwingOff,  "token: swing off");
        CHECK_EQ(parseSwing(swingName(kSwingAuto), kSwingOff),  kSwingAuto, "token: swing auto");
        CHECK_TRUE(parseBool(boolName(true), false),  "token: bool on");
        CHECK_FALSE(parseBool(boolName(false), true), "token: bool off");

        const char kUpper[] = "PROTOCOL: LG2\nMODE: COOL\nPOWER: ON\n";
        Device d;
        parse(kUpper, sizeof(kUpper) - 1, d);
        CHECK_STR(d.protocol, "LG2", "case: protocol");
        CHECK_EQ(d.mode, kModeCool, "case: mode");
        CHECK_TRUE(d.power, "case: power");
    }

    /* 9. serialise() refuses a buffer it cannot fill and leaves it empty. */
    {
        Device d;
        std::snprintf(d.protocol, sizeof(d.protocol), "%s", "KELVINATOR");
        char tiny[16] = { 'x', '\0' };
        CHECK_EQ(serialise(d, tiny, sizeof(tiny)), (size_t)0, "tiny buffer refused");
        CHECK_EQ((int)tiny[0], 0, "tiny buffer cleared");

        Device blank;
        char buf[kFileBufSize];
        CHECK_TRUE(serialise(blank, buf, sizeof(buf)) > 0, "blank: serialised");
        Device back;
        parse(buf, std::strlen(buf), back);
        CHECK_STR(back.protocol, "UNKNOWN", "blank: protocol placeholder");
    }
}

int main() {
    testVectorsForward();
    testVectorsRoundTrip();
    testPropertySweeps();
    testOutOfRange();
    testFromDecodeRefusals();
    testProtocolDetails();
    testNames();
    testHexBytes();
    testRawTools();
    testAcStore();

    std::printf("ir_codec: %lld checks, %lld failures (%lld ext frames skipped as "
                "documented NEC/NEC42 aliases)\n", g_checks, g_fails, g_aliased);
    return g_fails ? 1 : 0;
}
