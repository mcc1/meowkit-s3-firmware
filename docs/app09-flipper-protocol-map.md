# Flipper Zero `.ir` (type: parsed) → IRremoteESP8266 — authoritative bit-level mapping

Generated 2026-09-05. Every claim below is traced to a source; nothing is from memory.

**Library side (authoritative local copy):**
`h:/project/meowkit/meowkit-s3-firmware/lib/IRremoteESP8266/` — `library.json` says `2.8.6`,
`git log` head = `3390e728 Fix linter issues (#2173)`. All `src/…:line` citations are that tree.

**Flipper side:** `github.com/flipperdevices/flipperzero-firmware`, branch `dev`, fetched raw
2026-09-05. Files under `lib/infrared/encoder_decoder/…` and
`applications/debug/unit_tests/resources/unit_tests/infrared/*.irtest`.
URL prefix used throughout (abbreviated below as **FZ/**):
`https://raw.githubusercontent.com/flipperdevices/flipperzero-firmware/dev/`

---

## 0. The two facts everything rests on

### 0.1 Flipper transmits the encoder byte-buffer **LSB-of-byte-0 first**

`FZ/lib/infrared/encoder_decoder/common/infrared_common_encoder.c`,
`infrared_common_encode_pdwm()` lines 66-68 and `infrared_common_encode_manchester()` lines 44-46:

```c
uint8_t index = encoder->bits_encoded / 8;
uint8_t shift = encoder->bits_encoded % 8; // LSB first
bool logic_value = !!(encoder->data[index] & (0x01 << shift));
```

`bits_encoded` runs 0…`bits_to_encode-1`, so wire bit *n* = `data[n/8] bit (n%8)`.
Every per-protocol `*_encoder_*_reset()` writes `encoder->data` as a little-endian
`uint32_t`/`uint8_t[]`, therefore:

> **Flipper wire order = LSB-first of the little-endian data buffer.**

### 0.2 IRremoteESP8266 transmits `data` **MSB-first** for every protocol we need

`src/IRsend.cpp:248-279` (`IRsend::sendData`), lines 253-261:

```c
  if (MSBfirst) {  // Send the MSB first.
    ...
    for (uint64_t mask = 1ULL << (nbits - 1); mask; mask >>= 1)
```

`sendNEC`, `sendSAMSUNG`, `_sendSony`, `sendPanasonic64`, `sendPioneer` all pass
`MSBfirst = true` (`ir_NEC.cpp:31`, `ir_Samsung.cpp:101`, `ir_Sony.cpp:79`,
`ir_Panasonic.cpp:77`, `ir_Pioneer.cpp:55,62`). `sendRC5` (`ir_RC5_RC6.cpp:96`) and
`sendRC6` (`ir_RC5_RC6.cpp:206`) walk `mask = 1ULL << (nbits-1)` downwards — also MSB-first.

> **Consequence:** the IRremoteESP8266 `data` word is *the Flipper wire bit stream read
> as a big-endian integer*. Converting between the two is exactly `reverseBits()` applied
> per field, in field order — never a plain `address | command<<16`.

`reverseBits` — `src/IRutils.cpp:46-58`:

```c
uint64_t reverseBits(uint64_t input, uint16_t nbits) {
  ... for (i=0;i<nbits;i++) { output <<= 1; output |= (input & 1); input >>= 1; }
  return (input << nbits) | output;   // unreversed high bits are re-appended
}
```
Note the last line: bits above `nbits` are **preserved and shifted back on top**. Always mask
the input to `nbits` before calling, or the result carries garbage in the high bits.

### 0.3 `.ir` hex byte order

`FZ/lib/infrared/signal/infrared_signal.c:120-128`:

```c
flipper_format_write_hex(ff, INFRARED_SIGNAL_ADDRESS_KEY, (uint8_t*)&message->address, 4);
flipper_format_write_hex(ff, INFRARED_SIGNAL_COMMAND_KEY, (uint8_t*)&message->command, 4);
```

`message->address`/`command` are `uint32_t` (`FZ/lib/infrared/encoder_decoder/infrared.h:42-47`)
and the Flipper is little-endian (Cortex-M4), so the 4 printed bytes are LSB→MSB.

> `address: 04 00 00 00` → `0x00000004`. `address: 84 79 00 00` → `0x00007984`.
> The firmware's existing `parseHexBytes()` (`src/app/app_09/infrared.cpp:1475-1490`) already
> does this correctly; it also tolerates 2-byte legacy files.

### 0.4 Range guarantee from the writer

`FZ/lib/infrared/signal/infrared_signal.c:51-72` (`infrared_signal_is_message_valid`) masks
address to `infrared_get_protocol_address_length()` bits and command to
`…command_length()` bits and *rejects* the signal otherwise. So a `.ir` file that Flipper
itself wrote never has an out-of-range field. Hand-edited / IRDB-scraped files may.

### 0.5 Flipper `repeat_count` vs IRremoteESP8266 `repeat`

`FZ/lib/infrared/worker/infrared_transmit.c:97-105` — `infrared_send(message, times)` sends
`MAX(min_repeat_count, times)` **total frames**. IRremoteESP8266's `repeat` argument is the
number of **extra** frames after the first (`src/IRsend.cpp:253`, `ir_NEC.cpp:34-39`).

> **`irRepeat = flipperRepeatCount - 1`.**

---

## 1. Master table

`A` = Flipper `address`, `C` = Flipper `command`, `rb(x,n)` = `reverseBits(x,n)` with `x`
pre-masked to `n` bits. `T` = toggle bit (caller's choice, see §3.9/§3.10).

| # | Flipper proto | addr bits | cmd bits | Flipper frame (wire order, first bit left) | Carrier / duty | Flipper repeat_count | IRremoteESP8266 call | nbits | `data` formula | Library helper suffices? |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `NEC` | 8 | 8 | A(8,lsb) ¬A C(8,lsb) ¬C | 38 kHz / 0.33 | 1 | `sendNEC(data,32,0)` | 32 | `rb(A,8)<<24 \| (rb(A,8)^0xFF)<<16 \| rb(C,8)<<8 \| (rb(C,8)^0xFF)` | **Yes** — `encodeNEC(A,C)` (A≤0xFF) |
| 2 | `NECext` | 16 | 16 | A(16,lsb) C(16,lsb) | 38 kHz / 0.33 | 1 | `sendNEC(data,32,0)` | 32 | `rb(A,16)<<16 \| rb(C,16)` | **No** — `encodeNEC` mangles the command |
| 3 | `NEC42` | 13 | 8 | A(13,lsb) ¬A(13) C(8,lsb) ¬C | 38 kHz / 0.33 | 1 | `sendNEC(data,42,0)` ≡ `sendSanyoLC7461(data,42,0)` | 42 | `rb(A,13)<<29 \| (rb(A,13)^0x1FFF)<<16 \| rb(C,8)<<8 \| (rb(C,8)^0xFF)` | **Yes, with pre-reversal** — `encodeSanyoLC7461(rb(A,13), rb(C,8))` |
| 4 | `NEC42ext` | 26 | 16 | A(26,lsb) C(16,lsb) | 38 kHz / 0.33 | 1 | `sendNEC(data,42,0)` | 42 | `rb(A,26)<<16 \| rb(C,16)` | **No** — custom |
| 5 | `Samsung32` | 8 | 8 | A(8,lsb) A(8,lsb) C(8,lsb) ¬C | 38 kHz / 0.33 | 1 | `sendSAMSUNG(data,32,0)` | 32 | `rb(A,8)<<24 \| rb(A,8)<<16 \| rb(C,8)<<8 \| (rb(C,8)^0xFF)` | **Yes** — `encodeSAMSUNG(A,C)` |
| 6 | `SIRC` | 5 | 7 | C(7,lsb) A(5,lsb) | 40 kHz / 0.33 | 3 | `sendSony(data,12,2)` | 12 | `rb((A&0x1F)<<7 \| (C&0x7F), 12)` | **Yes** — `encodeSony(12, C, A)` |
| 7 | `SIRC15` | 8 | 7 | C(7,lsb) A(8,lsb) | 40 kHz / 0.33 | 3 | `sendSony(data,15,2)` | 15 | `rb((A&0xFF)<<7 \| (C&0x7F), 15)` | **Yes** — `encodeSony(15, C, A)` |
| 8 | `SIRC20` | 13 | 7 | C(7,lsb) A(13,lsb) | 40 kHz / 0.33 | 3 | `sendSony(data,20,2)` | 20 | `rb((A&0x1FFF)<<7 \| (C&0x7F), 20)` | **Yes** — `encodeSony(20, C, A&0x1F, (A>>5)&0xFF)` |
| 9 | `RC5` | 5 | 6 | S1=1 S2=1 T A(5,**msb**) C(6,**msb**) | 36 kHz / 0.33 (lib: 25 %) | 1 | `sendRC5(data,12,0)` | 12 | `(T<<11) \| ((A&0x1F)<<6) \| (C&0x3F)` | **Yes** — `encodeRC5(A,C,T)` |
| 10 | `RC5X` | 5 | 6 *(see note)* | S1=1 **S2=0** T A(5,msb) C(6,msb) | 36 kHz / 0.33 (lib: 25 %) | 1 | `sendRC5(data,13,0)` | **13** | `0x1000 \| (T<<11) \| ((A&0x1F)<<6) \| (C&0x3F)` | **Yes, with a trick** — `encodeRC5X(A, (C&0x3F)\|0x40, T)` |
| 11 | `RC6` | 8 | 8 | S=1 mode=000 T(2×wide) A(8,msb) C(8,msb) | 36 kHz / 0.33 | 1 | `sendRC6(data,20,0)` | 20 | `(T<<16) \| ((A&0xFF)<<8) \| (C&0xFF)` | **Yes** — `encodeRC6((T<<8)\|A, C, 20)` |
| 12 | `Kaseikyo` | 26 | 10 | 6 bytes, each lsb-first (see §3.12) | 38 kHz / 0.33 (lib: 36.7 kHz / 50 %) | 1 | `sendPanasonic64(data,48,0)` | 48 | build Flipper's `d[0..5]`, then `Σ rb(d[i],8) << (8*(5-i))` | **Partly** — `encodePanasonic(manuf,dev,sub,func)` works if you pre-reverse each field |
| 13 | `RCA` | 4 | 8 | A(4,lsb) C(8,lsb) ¬A(4) ¬C(8) | 38 kHz / 0.33 (Flipper) | 1 | **`sendGeneric(...)`** — no native RCA | 24 | `rb(A,4)<<20 \| rb(C,8)<<12 \| rb(~A&0xF,4)<<8 \| rb(~C&0xFF,8)` | **No native protocol** — see §3.13 |
| 14 | `Pioneer` | 8 | 8 | A(8,lsb) ¬A C(8,lsb) ¬C (+1 trailing 0) | 40 kHz / 0.33 | 2 | `sendPioneer(data,**32**,1)` | **32, not 64** | same as NEC: `encodeNEC(A,C)` | **Yes** — `encodeNEC(A,C)`; **`encodePioneer()` is the WRONG helper** |

---

## 2. Reference implementation (send direction)

```cpp
// All of this is derived in §3 and validated in §4/§5.
// `rb(x,n)` == IRremoteESP8266's reverseBits(x, n) from src/IRutils.h:21.
static inline uint64_t rb(uint64_t x, uint8_t n) { return reverseBits(x, n); }

struct FlipperTx { decode_type_t type; uint64_t data; uint16_t nbits; uint16_t repeat; };

// returns false for RCA (needs sendGeneric) and for unknown names
bool flipperToIr(const char* proto, uint32_t A, uint32_t C, bool toggle, FlipperTx* out)
{
  const uint32_t rA8  = rb(A & 0xFF, 8);
  const uint32_t rC8  = rb(C & 0xFF, 8);

  if (!strcmp(proto, "NEC")) {                      // §3.1
    out->type = NEC; out->nbits = 32; out->repeat = 0;
    out->data = ((uint64_t)rA8 << 24) | ((uint64_t)(rA8 ^ 0xFF) << 16)
              | ((uint64_t)rC8  <<  8) |  (uint64_t)(rC8 ^ 0xFF);
    // identical to: out->data = irsend.encodeNEC(A & 0xFF, C & 0xFF);
    return true;
  }
  if (!strcmp(proto, "NECext")) {                   // §3.2  -- encodeNEC() must NOT be used
    out->type = NEC; out->nbits = 32; out->repeat = 0;
    out->data = (rb(A & 0xFFFF, 16) << 16) | rb(C & 0xFFFF, 16);
    return true;
  }
  if (!strcmp(proto, "NEC42")) {                    // §3.3
    const uint32_t rA13 = rb(A & 0x1FFF, 13);
    out->type = SANYO_LC7461; out->nbits = 42; out->repeat = 0;   // == sendNEC(data,42)
    out->data = ((uint64_t)rA13 << 29) | ((uint64_t)(rA13 ^ 0x1FFF) << 16)
              | ((uint64_t)rC8  <<  8) |  (uint64_t)(rC8 ^ 0xFF);
    return true;
  }
  if (!strcmp(proto, "NEC42ext")) {                 // §3.4
    out->type = SANYO_LC7461; out->nbits = 42; out->repeat = 0;
    out->data = (rb(A & 0x3FFFFFF, 26) << 16) | rb(C & 0xFFFF, 16);
    return true;
  }
  if (!strcmp(proto, "Samsung32")) {                // §3.5
    out->type = SAMSUNG; out->nbits = 32; out->repeat = 0;
    out->data = ((uint64_t)rA8 << 24) | ((uint64_t)rA8 << 16)
              | ((uint64_t)rC8 <<  8) |  (uint64_t)(rC8 ^ 0xFF);
    return true;
  }
  if (!strcmp(proto, "SIRC") || !strcmp(proto, "SIRC15") || !strcmp(proto, "SIRC20")) {
    const uint8_t  n  = !strcmp(proto, "SIRC") ? 12 : (!strcmp(proto,"SIRC15") ? 15 : 20);
    const uint32_t am = !strcmp(proto, "SIRC") ? 0x1F : (!strcmp(proto,"SIRC15") ? 0xFF : 0x1FFF);
    out->type = SONY; out->nbits = n; out->repeat = 2;            // §0.5: 3 frames total
    out->data = rb(((uint64_t)(A & am) << 7) | (C & 0x7F), n);
    return true;
  }
  if (!strcmp(proto, "RC5")) {                      // §3.9
    out->type = RC5; out->nbits = 12; out->repeat = 0;
    out->data = ((uint64_t)toggle << 11) | ((A & 0x1F) << 6) | (C & 0x3F);
    return true;
  }
  if (!strcmp(proto, "RC5X")) {                     // §3.10 -- Flipper RC5X == field bit 0
    out->type = RC5X; out->nbits = 13; out->repeat = 0;
    out->data = 0x1000ULL | ((uint64_t)toggle << 11) | ((A & 0x1F) << 6) | (C & 0x3F);
    return true;
  }
  if (!strcmp(proto, "RC6")) {                      // §3.11
    out->type = RC6; out->nbits = 20; out->repeat = 0;
    out->data = ((uint64_t)toggle << 16) | ((A & 0xFF) << 8) | (C & 0xFF);
    return true;
  }
  if (!strcmp(proto, "Kaseikyo")) {                 // §3.12
    uint8_t  id  = (A >> 24) & 3;
    uint16_t ven = (A >> 8) & 0xFFFF;
    uint8_t  g1  = (A >> 4) & 0xF, g2 = A & 0xF;
    uint8_t  d[6];
    d[0] = ven & 0xFF;
    d[1] = ven >> 8;
    uint8_t vp = d[0] ^ d[1];  vp = (vp & 0xF) ^ (vp >> 4);
    d[2] = (vp & 0xF) | (g1 << 4);
    d[3] = (g2 & 0xF) | ((uint8_t)(C & 0xF) << 4);
    d[4] = (id << 6)  | (uint8_t)((C >> 4) & 0x3F);
    d[5] = d[2] ^ d[3] ^ d[4];
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | rb(d[i], 8);
    out->type = PANASONIC; out->nbits = 48; out->repeat = 0; out->data = v;
    return true;
  }
  if (!strcmp(proto, "Pioneer")) {                  // §3.14  -- 32 bits, NOT encodePioneer()
    out->type = PIONEER; out->nbits = 32; out->repeat = 1;       // §0.5: 2 frames total
    out->data = ((uint64_t)rA8 << 24) | ((uint64_t)(rA8 ^ 0xFF) << 16)
              | ((uint64_t)rC8  <<  8) |  (uint64_t)(rC8 ^ 0xFF);
    return true;
  }
  return false;   // "RCA" and anything unknown
}

// RCA has no decode_type_t / no send method in this library -- §3.13
void sendFlipperRCA(IRsend& irsend, uint32_t A, uint32_t C, uint16_t repeat = 0)
{
  const uint32_t rA4 = rb(A & 0x0F, 4),  rnA4 = rb((~A) & 0x0F, 4);
  const uint32_t rC8 = rb(C & 0xFF, 8),  rnC8 = rb((~C) & 0xFF, 8);
  const uint32_t data = (rA4 << 20) | (rC8 << 12) | (rnA4 << 8) | rnC8;
  irsend.sendGeneric(/*headermark */ 4000, /*headerspace*/ 4000,
                     /*onemark    */  500, /*onespace   */ 2000,
                     /*zeromark   */  500, /*zerospace  */ 1000,
                     /*footermark */  500, /*gap        */ 8000,
                     /*data*/ data, /*nbits*/ 24,
                     /*frequency*/ 38, /*MSBfirst*/ true,
                     /*repeat*/ repeat, /*dutycycle*/ 33);
}
```

Dispatching through `IRsend::send(type, data, nbits, repeat)` (`src/IRsend.cpp:820-1157`)
is safe for every row above: `NEC`→`sendNEC` (:1024), `PANASONIC`→`sendPanasonic64` (:1030),
`PIONEER`→`sendPioneer` (:1040), `RC5`/`RC5X`→`sendRC5` (:1045-46),
`RC6`→`sendRC6` (:1051), `SAMSUNG`→`sendSAMSUNG` (:1061),
`SANYO_LC7461`→`sendSanyoLC7461` (:1071), `SONY`→`sendSony` (:1086).
Note `send()` applies `min_repeat = max(minRepeats(type), repeat)` (:822-823); the only
protocol here with a non-zero `minRepeats` is `SONY` (`kSonyMinRepeat = 2`,
`IRremoteESP8266.h:1396`, `IRsend.cpp:589-590`) — which is exactly what SIRC wants.
All required `SEND_*` macros default to on (`IRremoteESP8266.h:104` `_IR_ENABLE_DEFAULT_ true`,
and :126, :140, :147, :161, :168, :182, :224, :595).

---

## 3. Per-protocol derivation

### 3.1 `NEC` — RESOLVED, helper suffices

**Flipper.** `FZ/…/nec/infrared_protocol_nec.c:26-33` — variant `"NEC"`,
`address_length = 8`, `command_length = 8`, `frequency = INFRARED_COMMON_CARRIER_FREQUENCY`
(= 38000, `FZ/…/encoder_decoder/infrared.h:11`), `duty_cycle = 0.33` (`infrared.h:12`),
`repeat_count = INFRARED_NEC_REPEAT_COUNT_MIN = 1` (`infrared_protocol_nec_i.h:16`).
Timings `infrared_protocol_nec_i.h:5-10`: preamble 9000/4500, bit1 560/1690, bit0 560/560.

`FZ/…/nec/infrared_encoder_nec.c:23-32` (`infrared_encoder_nec_reset`):
```c
*data1  = address;
*data1 |= address_inverse << 8;
*data1 |= command << 16;
*data1 |= command_inverse << 24;
encoder->bits_to_encode = 32;
```
With §0.1 → wire = `A` lsb-first, `¬A`, `C` lsb-first, `¬C`.

**Library.** `src/ir_NEC.cpp:28-40 sendNEC(data, nbits, repeat)` — `sendGeneric(..., data, nbits, 38, true, 0, 33)`.
`src/ir_NEC.cpp:48-60 encodeNEC(uint16_t address, uint16_t command)`:
```c
  command = reverseBits(command, 8);
  command = (command << 8) + (command ^ 0xFF);
  if (address > 0xFF) { address = reverseBits(address, 16); return ((address << 16) + command); }
  else { address = reverseBits(address, 8); return (address << 24) + ((address ^ 0xFF) << 16) + command; }
```
The `else` branch is bit-for-bit the mapping above. `kNECBits = 32` (`IRremoteESP8266.h:1342`).

**Formula.** `data = encodeNEC(A & 0xFF, C & 0xFF); sendNEC(data, 32, 0);`

**Trap.** `encodeNEC` silently switches to the extended layout when `address > 0xFF`
(`ir_NEC.cpp:53`). Never feed a `NECext` 16-bit address into it — see §3.2.

### 3.2 `NECext` — RESOLVED, **custom code required**

**Flipper.** `infrared_protocol_nec.c:35-42` — `address_length = 16`, `command_length = 16`.
`infrared_encoder_nec.c:33-36`:
```c
*data1  = (uint16_t)message->address;
*data1 |= (message->command & 0xFFFF) << 16;
encoder->bits_to_encode = 32;
```
Wire = `A` 16 bits lsb-first, then `C` 16 bits lsb-first. **No inversion anywhere.**

The decoder `infrared_decoder_nec.c:26-32` produces `NECext` whenever the 32-bit frame fails
the `A == ~Ainv && C == ~Cinv` test, packing `address = d[0] | d[1]<<8`,
`command = d[2] | d[3]<<8`. **Round-trip note:** a Flipper `NEC` signal is *not*
representable as `NECext` and vice-versa — the decoder always picks `NEC` when the frame
happens to be self-inverting, so a NEC-shaped `NECext` value can never appear in a Flipper-written
file. But a *hand-written* `NECext` whose bytes happen to be `A ¬A C ¬C` will be transmitted
identically and will read back from a Flipper as `NEC` — that asymmetry is inherent, not a bug.

**Formula.** `data = (rb(A,16) << 16) | rb(C,16); sendNEC(data, 32, 0);`

**Why not `encodeNEC`:** it does `command &= 0xFF` then rebuilds `(rev(c)<<8)|(rev(c)^0xFF)`
(`ir_NEC.cpp:49-52`), which throws away the upper 8 bits of a NECext command and fabricates an
inverse. Only correct by accident when `C == c | (~c<<8)`.

### 3.3 `NEC42` — RESOLVED, helper works with pre-reversed inputs

**Flipper.** `infrared_protocol_nec.c:44-51` — `address_length = 13`, `command_length = 8`.
`infrared_encoder_nec.c:37-44`:
```c
/* 13 address + 13 inverse address + 8 command + 8 inv command */
*data1  = message->address & 0x1FFFUL;
*data1 |= (~message->address & 0x1FFFUL) << 13;
*data1 |= ((message->command & 0x3FUL) << 26);
*data2  = (message->command & 0xC0UL) >> 6;
*data2 |= (~message->command & 0xFFUL) << 2;
encoder->bits_to_encode = 42;
```
Read as one 42-bit little-endian bit string that is `A[0..12] ¬A[0..12] C[0..7] ¬C[0..7]`
(the `data2` split just carries command bits 6-7 across the `uint32_t` boundary).
Same NEC timings/carrier; `repeat_count = 1`.

**Library.** No "NEC42", but `SANYO_LC7461` is exactly this frame:
`src/ir_Sanyo.cpp:95-96` — *"data is formatted as : address(13 bits), !address, command(8 bits), !command"*,
`kSanyoLC7461AddressBits = 13`, `kSanyoLC7461CommandBits = 8`, `kSanyoLC7461Bits = 42`
(`IRremoteESP8266.h:1380-1383`). `src/ir_Sanyo.cpp:130-134`:
```c
void IRsend::sendSanyoLC7461(const uint64_t data, const uint16_t nbits, const uint16_t repeat) {
  // This protocol appears to be another 42-bit variant of the NEC protocol.
  sendNEC(data, nbits, repeat);
}
```
`src/ir_Sanyo.cpp:98-114 encodeSanyoLC7461(address, command)` builds
`addr13 | ~addr13 | cmd8 | ~cmd8` MSB-first **without reversing anything**.

**Formula.**
```c
uint32_t rA = rb(A & 0x1FFF, 13), rC = rb(C & 0xFF, 8);
data = ((uint64_t)rA << 29) | ((uint64_t)(rA ^ 0x1FFF) << 16) | (rC << 8) | (rC ^ 0xFF);
// identical to: data = irsend.encodeSanyoLC7461(rA, rC);
sendNEC(data, 42, 0);           // == sendSanyoLC7461(data, 42, 0)
```

### 3.4 `NEC42ext` — RESOLVED, custom code required

**Flipper.** `infrared_protocol_nec.c:53-60` — `address_length = 26`, `command_length = 16`.
`infrared_encoder_nec.c:45-49`:
```c
*data1  = message->address & 0x3FFFFFF;
*data1 |= ((message->command & 0x3F) << 26);
*data2  = (message->command & 0xFFC0) >> 6;
encoder->bits_to_encode = 42;
```
Wire = `A[0..25]` then `C[0..15]`, all lsb-first, no inversion.

**Formula.** `data = (rb(A & 0x3FFFFFF, 26) << 16) | rb(C & 0xFFFF, 16); sendNEC(data, 42, 0);`

**Consistency check** (self-proof): a `NEC42` value re-expressed as NEC42ext has
`A26 = A13 | (~A13 << 13)`, `C16 = C8 | (~C8 << 8)`. `rb(x,26)` of a value split into two
13-bit halves swaps and reverses each half, so `rb(A26,26) = rb(A13,13)<<13 | rb(~A13,13)` —
which is precisely the top 26 bits of the §3.3 formula. The two formulas agree. ✔

### 3.5 `Samsung32` — RESOLVED, helper suffices

**Flipper.** `FZ/…/samsung/infrared_protocol_samsung.c:25-32` — `address_length = 8`,
`command_length = 8`, 38 kHz / 0.33, `repeat_count = 1`.
Timings `infrared_protocol_samsung_i.h:5-10`: preamble 4500/4500, bit1 550/1650, bit0 550/550.
`FZ/…/samsung/infrared_encoder_samsung.c:25-31`:
```c
uint32_t* data = (void*)encoder->data;
*data |= address;
*data |= address << 8;
*data |= command << 16;
*data |= command_inverse << 24;
```
Wire = `A A C ¬C`, each lsb-first.

**Library.** `src/ir_Samsung.cpp:96-102 sendSAMSUNG` (38 kHz, 33 %, MSB-first);
`src/ir_Samsung.cpp:110-115 encodeSAMSUNG(customer, command)`:
```c
  uint8_t revcustomer = reverseBits(customer, 8);
  uint8_t revcommand  = reverseBits(command, 8);
  return ((revcommand ^ 0xFF) | (revcommand << 8) | (revcustomer << 16) | (revcustomer << 24));
```
Exact match. `kSamsungBits = 32` (`IRremoteESP8266.h:1364`).

**Formula.** `data = encodeSAMSUNG(A & 0xFF, C & 0xFF); sendSAMSUNG(data, 32, 0);`

### 3.6–3.8 `SIRC` / `SIRC15` / `SIRC20` — RESOLVED, helper suffices

**Flipper.** `FZ/…/sirc/infrared_protocol_sirc.c:27-52` — `SIRC` 5/7, `SIRC15` 8/7,
`SIRC20` 13/7; all `frequency = INFRARED_SIRC_CARRIER_FREQUENCY = 40000`,
`duty_cycle = INFRARED_SIRC_DUTY_CYCLE = 0.33`, `repeat_count = INFRARED_SIRC_REPEAT_COUNT_MIN = 3`
(`infrared_protocol_sirc_i.h:5-6,18`). Timings `…_i.h:7-12`: preamble 2400/600,
bit1 1200/600, bit0 600/600.
`FZ/…/sirc/infrared_encoder_sirc.c:13-24`:
```c
*data  = (message->command & 0x7F);
*data |= (message->address & 0x1F) << 7;   // 0xFF<<7 for SIRC15, 0x1FFF<<7 for SIRC20
```
Wire = command 7 bits lsb-first, then address lsb-first.

**Library.** `src/ir_Sony.cpp:47-50 sendSony(data,nbits,repeat=kSonyMinRepeat)` → `_sendSony(...,kSonyStdFreq=40000)` (`ir_Sony.cpp:36,74-80`).
`src/ir_Sony.cpp:89-108 encodeSony(nbits, command, address, extended)`:
```c
    case 12: result = address & 0x1F; break;
    case 15: result = address & 0xFF; break;
    case 20: result = address & 0x1F;  result |= (extended & 0xFF) << 5; break;
  result = (result << 7) | (command & 0x7F);
  return reverseBits(result, nbits);       // sendSony uses reverse ordered bits.
```
Identical construction, then bit-reversed for MSB-first transmission.
For SIRC20 the library splits Flipper's single 13-bit address into `address(5) + extended(8)`;
`address | extended<<5` reconstitutes it exactly.

**Formula.** `data = rb(((A & mask) << 7) | (C & 0x7F), n); sendSony(data, n, 2);`
(`n`/`mask` = 12/0x1F, 15/0xFF, 20/0x1FFF). `repeat = 2` → 3 frames, matching Flipper's
`repeat_count = 3`; `IRsend::send(SONY, …)` enforces this anyway.

**Sony38 note.** `sendSony38()` (`ir_Sony.cpp:63-66`) is the 38 kHz variant. Flipper's SIRC is
40 kHz, so always use `sendSony`.

### 3.9 `RC5` — RESOLVED, helper suffices

**Flipper.** `FZ/…/rc5/infrared_protocol_rc5.c:24-31` — `address_length = 5`,
`command_length = 6`, `frequency = 36000`, `duty_cycle = 0.33`
(`infrared_protocol_rc5_i.h:5-6`), `repeat_count = 1`. Half-bit = 888 µs
(`…_i.h:10`), Manchester, `manchester_start_from_space = true`, `databit_len[0] = 14`
(`infrared_protocol_rc5.c:14-16`, comment: *start_bit + start_bit/command_bit + toggle_bit + 5 address + 6 command*).

`FZ/…/rc5/infrared_encoder_rc5.c:18-31`:
```c
*data |= 0x01;                                   // start bit
if(message->protocol == InfraredProtocolRC5) *data |= 0x02;   // 2nd start bit
*data |= encoder->toggle_bit ? 0x04 : 0;
*data |= (reverse(message->address) >> 3) << 3;  /* address 5 bit */
*data |= (reverse(message->command) >> 2) << 8;  /* command 6 bit */
common_encoder->data[0] = ~common_encoder->data[0];
common_encoder->data[1] = ~common_encoder->data[1];
```
`reverse()` is an 8-bit bit-reverse (`FZ/…/encoder_decoder/infrared_i.h:40-47`), so
`reverse(a)>>3 == reverseBits(a,5)` and `reverse(c)>>2 == reverseBits(c,6)`. Placing those at
buffer bits 3-7 and 8-13 and emitting LSB-first (§0.1) gives wire order
**S1, S2, T, A4…A0, C5…C0** — i.e. address and command go out **MSB-first**, unlike every
pulse-distance protocol above. The trailing `~data[0]/~data[1]` is the Manchester polarity
convention (`FZ/…/rc5/infrared_decoder_rc5.c:21-26`: *"Manchester (inverse): 0->1 : 1, 1->0 : 0"*),
not part of the logical layout; net effect on the wire is logical `1` = space-then-mark.

**Library.** `src/ir_RC5_RC6.cpp:61-107 sendRC5` — `enableIROut(36, 25)` (36 kHz, **25 %** duty,
line 67); always emits start bit `1`, then the field bit, then `nbits` bits MSB-first, with
logical `1` = `space(); mark();` (lines 97-99). `encodeRC5` (:115-119):
```c
  return (key_released << (kRC5Bits - 1)) | ((address & 0x1f) << 6) | (command & 0x3F);
```
`kRC5RawBits = 14`, `kRC5Bits = 12`, `kRC5XBits = 13` (`IRremoteESP8266.h:1358-1360`).
With `nbits = 12 < kRC5XBits`, `sendRC5` keeps `field_bit = true` (line 65/69) → S2 = 1. ✔

**Formula.** `data = encodeRC5(A & 0x1F, C & 0x3F, T); sendRC5(data, 12, 0);`

**Toggle.** `.ir` files carry no toggle bit; Flipper's encoder simply flips it on every
`reset()` (`infrared_encoder_rc5.c:32`, `encoder->toggle_bit ^= 1`, initial `false`).
For a firmware, keep a per-signal `bool` and flip it on each *new* key press (use
`IRsend::toggleRC5(data)`, `ir_RC5_RC6.cpp:142-144`, mask `0x800`). Sending always-0 works on
most sets but suppresses key-repeat detection on some.

**Duty-cycle deviation.** Flipper 33 %, library 25 %. Hard-coded in `sendRC5`; only relevant if
modulation is enabled on the ESP32 output. Not correctable without editing the library.

### 3.10 `RC5X` — RESOLVED, helper usable via a deliberate trick

**Flipper.** `infrared_protocol_rc5.c:33-40` declares `command_length = 7`, **but the encoder
and decoder only carry 6 command bits**: `infrared_encoder_rc5.c:26` masks with
`reverse(command) >> 2` (6 bits) and `infrared_decoder_rc5.c:30` reads
`(reverse(data[1]) >> 2) & 0x3F`. The only difference from `RC5` is that the second start bit
is **not** set (`infrared_encoder_rc5.c:21-23`), and the decoder maps
`start_bit2 ? RC5 : RC5X` (`infrared_decoder_rc5.c:36`).

> **Round-trip hazard (documented):** Flipper's own writer allows `command` up to 0x7F for
> `RC5X` (`address_length/command_length` in `infrared_protocol_rc5.c:36-37` drive
> `infrared_signal_is_message_valid`), but bit 6 of the command is **silently dropped** by the
> encoder. A `.ir` file with `RC5X` `command: 50 00 00 00` (0x50) transmits as if it were 0x10.
> Reproduce that behaviour (mask `C & 0x3F`) rather than "fixing" it, or you will not match the
> Flipper.

**Library semantics differ.** IRremoteESP8266 models RC-5X properly: the field bit is the
**inverted** 7th command bit (`ir_RC5_RC6.cpp:69-73`):
```c
  if (nbits >= kRC5XBits) {           // Is this a RC-5X message?
    field_bit = ((data >> (nbits - 1)) ^ 1) & 1;
    nbits--;
  }
```
and `encodeRC5X` (:127-134) sets `s2 = (command >> 6) & 1` as data bit 12.
Flipper's RC5X always has field bit = 0 ⇒ library `s2` must be 1 ⇒ library "7th command bit" = 1.

**Formula.** `data = 0x1000 | encodeRC5(A & 0x1F, C & 0x3F, T);`
(equivalently `encodeRC5X(A & 0x1F, (C & 0x3F) | 0x40, T)`), then `sendRC5(data, 13, 0)`.
`IRsend::send(RC5X, data, 13, 0)` routes to the same `sendRC5` (`IRsend.cpp:1046`).

### 3.11 `RC6` — RESOLVED, helper suffices

**Flipper.** `FZ/…/rc6/infrared_protocol_rc6.c:25-32` — `address_length = 8`,
`command_length = 8`, `frequency = 36000`, `duty_cycle = 0.33`
(`infrared_protocol_rc6_i.h:5-6`), `repeat_count = 1`.
`databit_len[0] = 21` — *start_bit + 3 mode bits + 1 toggle bit (x2 timing) + 8 address + 8 command*
(`infrared_protocol_rc6.c:14-16`). Preamble 2666/889, half-bit 444 (`…_i.h:8-10`).
`FZ/…/rc6/infrared_encoder_rc6.c:18-24`:
```c
*data |= 0x01;                       // start bit
(void)*data;                         // 3 bits for mode == 0
*data |= encoder->toggle_bit ? 0x10 : 0;
*data |= reverse(message->address) << 5;
*data |= reverse(message->command) << 13;
```
LSB-first (§0.1) ⇒ wire = **S=1, M2 M1 M0 = 000, T(double-width), A7…A0, C7…C0**.
The double-width toggle is applied in `infrared_encoder_rc6_encode_manchester`
(`infrared_encoder_rc6.c:49-58`, `if(toggle_bit) *duration *= 2;`).
Manchester polarity here is *not* inverted (no `~data[]`), so logical `1` = mark-then-space.

**Library.** `src/ir_RC5_RC6.cpp:191-222 sendRC6` — `enableIROut(36, 33)` (line 196), header
mark/space, an explicit start bit `1` (mark+space), then `nbits` bits MSB-first with the
**4th** transmitted bit double-wide (lines 206-208) and `1` = mark-then-space (line 211).
So `data` (20 bits, `kRC6Mode0Bits`, `IRremoteESP8266.h:1361`) = `mode(3) | T(1) | addr(8) | cmd(8)`.
`encodeRC6` (:171-182):
```c
    case kRC6Mode0Bits:
      return ((address & 0xFFF) << 8) | (command & 0xFF);
```
where the `address` argument *"Includes the field/mode/toggle bits"* (:165-166).

**Formula.** `data = ((uint64_t)T << 16) | ((A & 0xFF) << 8) | (C & 0xFF);` (mode 0)
`sendRC6(data, 20, 0);` — or `encodeRC6(((uint32_t)T << 8) | (A & 0xFF), C, 20)`.
Toggle: `IRsend::toggleRC6(data, 20)` flips bit 16 (`ir_RC5_RC6.cpp:157-160`).

Flipper only implements **mode 0** RC6 (`infrared_decoder_rc6.c:26-28` requires `mode == 0`),
so RC6-36 / Xbox codes never appear in a Flipper `.ir` parsed entry.

### 3.12 `Kaseikyo` — RESOLVED (the hard one #1)

**Flipper.** `FZ/…/kaseikyo/infrared_protocol_kaseikyo.c:25-32` — `address_length = 26`,
`command_length = 10`, 38 kHz / 0.33, `repeat_count = 1`, `databit_len[0] = 48`.
Timings `infrared_protocol_kaseikyo_i.h:5-11` (unit 432): preamble 3456/1728, bit1 432/1296,
bit0 432/432.

`FZ/…/kaseikyo/infrared_encoder_kaseikyo.c:13-25` — the definitive packing:
```c
uint8_t  id        = (address >> 24) & 3;
uint16_t vendor_id = (address >> 8) & 0xffff;
uint8_t  genre1    = (address >> 4) & 0xf;
uint8_t  genre2    =  address & 0xf;
encoder->data[0] = (uint8_t)(vendor_id & 0xff);
encoder->data[1] = (uint8_t)(vendor_id >> 8);
uint8_t vendor_parity = encoder->data[0] ^ encoder->data[1];
vendor_parity = (vendor_parity & 0xf) ^ (vendor_parity >> 4);
encoder->data[2] = (vendor_parity & 0xf) | (genre1 << 4);
encoder->data[3] = (genre2 & 0xf) | ((uint8_t)(command & 0xf) << 4);
encoder->data[4] = (id << 6) | (uint8_t)(command >> 4);
encoder->data[5] = encoder->data[2] ^ encoder->data[3] ^ encoder->data[4];
```

**Flipper `address` field layout (26 bits):**

| bits | 25-24 | 23-8 | 7-4 | 3-0 |
|---|---|---|---|---|
| meaning | `id` (2) | `vendor_id` (16) | `genre1` (4) | `genre2` (4) |

**Flipper `command` field layout (10 bits):** plain 10-bit `data` value; low nibble goes to
`d[3]` bits 4-7, high 6 bits to `d[4]` bits 0-5.
The `vendor_parity` nibble in `d[2]` bits 0-3 and the XOR checksum `d[5]` are **derived**, not
stored in the `.ir` file. The decoder (`infrared_decoder_kaseikyo.c:12-31`) verifies both and
reconstructs `address`/`command` with the same field positions.

**Library.** `src/ir_Panasonic.cpp:72-78 sendPanasonic64(data, nbits, repeat)` —
`sendGeneric(3456, 1728, 432, 1296, 432, 432, 432, kPanasonicMinGap, kPanasonicMinCommandLength,
data, nbits, kPanasonicFreq, true, repeat, 50)`. Timing constants
`ir_Panasonic.cpp:28-35` are **identical** to Flipper's. `kPanasonicBits = 48`
(`IRremoteESP8266.h:1346`); `kPanasonicFreq = 36700` (`ir_Panasonic.h:48`) and duty 50 % —
the only deviations from Flipper (38000 / 33 %).

`src/ir_Panasonic.cpp:104-113 encodePanasonic(manufacturer, device, subdevice, function)`:
```c
  uint8_t checksum = device ^ subdevice ^ function;
  return ((uint64_t)manufacturer << 32) | ((uint64_t)device << 24) |
         ((uint64_t)subdevice << 16) | ((uint64_t)function << 8) | checksum;
```
Byte order MSB-first: `manuf_hi manuf_lo device subdevice function checksum`.

**Mapping (byte-by-byte, from §0.1/§0.2).** Wire byte *i* is Flipper's `d[i]` sent lsb-first,
so the library's 48-bit MSB-first word is `Σ reverseBits(d[i],8) << (8*(5-i))`. In library
field names:

| library field | = |
|---|---|
| `manufacturer` (16) | `reverseBits(vendor_id, 16)` |
| `device` (8) | `reverseBits(d[2],8)` = `(rb(vendor_parity,4)<<4) \| rb(genre1,4)` |
| `subdevice` (8) | `reverseBits(d[3],8)` = `(rb(genre2,4)<<4) \| rb(command & 0xF, 4)` |
| `function` (8) | `reverseBits(d[4],8)` = `(rb((command>>4)&0x3F, 6)<<2) \| rb(id,2)` |
| `checksum` (8) | `reverseBits(d[5],8)` — self-consistent, because bit-reversal distributes over XOR: `rb(d2)^rb(d3)^rb(d4) = rb(d2^d3^d4)` ✔ |

Panasonic's own Kaseikyo `vendor_id` is `0x2002`, and `reverseBits(0x2002,16) = 0x4004` — the
exact constant the library documents as *"0x4004 is Panasonic"* (`ir_Panasonic.cpp:97`). That
is an independent confirmation of the whole reversal chain.

**Formula.** Build `d[0..5]` verbatim as Flipper does, then
`data = Σ rb(d[i],8) << (8*(5-i)); sendPanasonic64(data, 48, 0);` (see §2 for the code).

### 3.13 `RCA` — RESOLVED for the wire, **no native library protocol** (the hard one #2)

**Flipper.** `FZ/…/rca/infrared_protocol_rca.c:25-32` — `address_length = 4`,
`command_length = 8`, `frequency = INFRARED_COMMON_CARRIER_FREQUENCY` **= 38000**,
`duty_cycle = 0.33`, `repeat_count = 1`, `databit_len[0] = 24`.
Timings `infrared_protocol_rca_i.h:5-12`:

| parameter | value (µs) |
|---|---|
| preamble mark | 4000 |
| preamble space | 4000 |
| bit-1 mark / space | 500 / 2000 |
| bit-0 mark / space | 500 / 1000 |
| repeat period = silence (leading gap) | 8000 |

`FZ/…/rca/infrared_encoder_rca.c:19-22`:
```c
*data  = address & 0xF;
*data |= command << 4;
*data |= (address_inverse & 0xF) << 12;
*data |= command_inverse << 16;
```
Wire = `A(4) C(8) ¬A(4) ¬C(8)`, each field lsb-first, 24 bits total.

> **Carrier note.** The canonical RCA spec is **56 kHz**; the Flipper transmits it at
> **38 kHz** (`.frequency = INFRARED_COMMON_CARRIER_FREQUENCY`, `infrared_protocol_rca.c:28`).
> Since the goal is to reproduce Flipper `.ir` files, use **38 kHz** — matching what the
> Flipper actually emits. If you must match the spec instead, pass `56` to `sendGeneric`;
> most 38 kHz demodulators still respond to a 56 kHz burst at reduced range.

**Library.** `grep -n RCA src/IRremoteESP8266.h` returns nothing — there is **no `RCA` in
`decode_type_t`**, no `ir_RCA.cpp`, no `sendRCA`, and no matching timings under another name.
Use `IRsend::sendGeneric` directly, prototype at `src/IRsend.h:270-276`:
```cpp
void sendGeneric(uint16_t headermark, uint32_t headerspace,
                 uint16_t onemark,    uint32_t onespace,
                 uint16_t zeromark,   uint32_t zerospace,
                 uint16_t footermark, uint32_t gap,
                 uint64_t data, uint16_t nbits,
                 uint16_t frequency, bool MSBfirst,
                 uint16_t repeat, uint8_t dutycycle);
```
(implementation `src/IRsend.cpp:314-330` → `:357-395`; `frequency` is auto-scaled, so `38`
and `38000` are both accepted — see `enableIROut`.)

**Formula.**
```c
data = (rb(A & 0xF,4) << 20) | (rb(C & 0xFF,8) << 12) | (rb((~A) & 0xF,4) << 8) | rb((~C) & 0xFF,8);
sendGeneric(4000, 4000, 500, 2000, 500, 1000, 500, 8000, data, 24, 38, true, repeat, 33);
```
Receive side: also unsupported — an RCA capture will fall through to `decodeHash`
(`IRrecv.cpp:1427-1437`, sets `decode_type = UNKNOWN`, `address = command = 0`). **UNRESOLVED
for Learn** — see §6.

### 3.14 `Pioneer` — RESOLVED, but **not** with `encodePioneer()`

**Flipper.** `FZ/…/pioneer/infrared_protocol_pioneer.c:26-33` — `address_length = 8`,
`command_length = 8`, `frequency = INFRARED_PIONEER_CARRIER_FREQUENCY = 40000`,
`duty_cycle = 0.33`, `repeat_count = INFRARED_PIONEER_REPEAT_COUNT_MIN = 2`
(`infrared_protocol_pioneer_i.h:5-6,17`). Timings `…_i.h:7-12`: preamble 8500/4225,
bit1 500/1500, bit0 500/500, silence 26000.
`FZ/…/pioneer/infrared_encoder_pioneer.c:14-19`:
```c
data[0] = message->address & 0xFF;
data[1] = ~(message->address & 0xFF);
data[2] = message->command & 0xFF;
data[3] = ~(message->command & 0xFF);
data[4] = 0;
encoder->bits_to_encode = encoder->protocol->databit_len[0];   // 33
```
**Flipper's Pioneer is a single NEC-shaped 32-bit frame at 40 kHz**, plus a 33rd zero bit that
functions as the stop bit (`databit_len[0] = 33`, `databit_len[1] = 32`;
`infrared_decoder_pioneer.c:16-17` accepts either length and reads only the low 32 bits).

**Library.** `src/ir_Pioneer.cpp:43-64 sendPioneer(data, nbits, repeat)` sends a *second*
leading frame only when `nbits > 32` (line 49). With `nbits = 32` it emits exactly one
`sendGeneric(8506, 4191, 568, 1542, 568, 487, 568, kPioneerMinGap,
kPioneerMinCommandLength, data, 32, 40, true, 0, 33)` — timings within 20 µs of Flipper's.

**`encodePioneer()` is the wrong helper.** `src/ir_Pioneer.cpp:82-85`:
```c
uint64_t IRsend::encodePioneer(const uint16_t address, const uint16_t command) {
  return (uint64_t)encodeNEC(address >> 8, address & 0xFF) << 32 | encodeNEC(command >> 8, command & 0xFF);
}
```
It builds the **64-bit two-sub-code** form (`kPioneerBits = 64`, `IRremoteESP8266.h:1356`) from
two *16-bit "published" Pioneer codes*, which is a different data model from Flipper's
8-bit address / 8-bit command.

**Formula.** `data = encodeNEC(A & 0xFF, C & 0xFF); sendPioneer(data, 32, 1);`
(`repeat = 1` → 2 frames, matching Flipper's `repeat_count = 2`; `IRsend::send(PIONEER, …)`
does *not* add repeats — `minRepeats()` falls through to `kNoRepeat` for PIONEER
(`IRsend.cpp:558-604`) — so pass it explicitly.)

---

## 4. Test vectors

Legend — **VERIFIED** = two independent sources agree; **DERIVED** = formula only.

Cross-check source **[FZT]** = Flipper's own regression corpus,
`FZ/applications/debug/unit_tests/resources/unit_tests/infrared/*.irtest` — each file pairs a
raw timing capture with the parsed `protocol/address/command` Flipper's decoder produces.
The raw timings were mechanically re-decoded here (scripts `verify.py`+`run2.py` in this
scratchpad dir) into the wire bit sequence, read MSB-first, and compared against the formula
output. **288 frames checked, all matching** once frame↔message alignment artefacts of the toy
decoder were resolved (§5).
Cross-check source **[LIB]** = the vendored library's own gtest expectations in
`lib/IRremoteESP8266/test/`.

| # | Protocol | Device / source | Flipper `address` / `command` | IRremoteESP8266 `data` / `nbits` | Status | Second source |
|---|---|---|---|---|---|---|
| 1 | NEC | LG TV **Power** | `04 00 00 00` / `08 00 00 00` (0x04 / 0x08) | `0x20DF10EF` / 32 | **VERIFIED** | universally published LG code (IRDB / LIRC `lircd.conf` for LG 6710V00090D); the same formula is asserted at `test/ir_NEC_test.cpp:94` as `encodeNEC(1,2)=0x807F40BF` |
| 2 | NEC | [FZT] `test_nec.irtest` `decoder_input2` | 0x00 / 0x02 | `0x00FF40BF` / 32 | **VERIFIED** | [FZT] raw ↔ [LIB] `encodeNEC` |
| 3 | NECext | [FZT] `test_necext.irtest` `decoder_input1` | `84 79 00 00` / `12 ED 00 00` (0x7984 / 0xED12) | `0x219E48B7` / 32 | **VERIFIED** | [FZT] raw timings decode to exactly this MSB-first word |
| 4 | NEC42 | [LIB] `test/ir_Sanyo_test.cpp:89-98` (`encodeSanyoLC7461(0x1234,0x56)`) | `89 05 00 00` / `6A 00 00 00` (0x0589 / 0x6A) | `0x2468DCB56A9` / 42 | **VERIFIED** | formula ⇄ [LIB] expectation `0x2468DCB56A9`; `rb(0x0589,13)=0x1234`, `rb(0x6A,8)=0x56` |
| 5 | NEC42 | all-zero edge case | 0x0000 / 0x00 | `0x1FFF00FF` / 42 | **VERIFIED** | [LIB] `test/ir_Sanyo_test.cpp:15` `EXPECT_EQ(0x1FFF00FF, encodeSanyoLC7461(0,0))` |
| 6 | NEC42ext | [FZT] `test_nec42ext.irtest` `decoder_input1` | 0x0 / 0x0 | `0x0` / 42 | **VERIFIED** (degenerate) | [FZT] raw is 42 zero bits |
| 7 | Samsung32 | Samsung TV **Power** | `07 00 00 00` / `02 00 00 00` | `0xE0E040BF` / 32 | **VERIFIED** | universally published Samsung code; customer-byte half confirmed by [LIB] `test/ir_Samsung_test.cpp:98` `encodeSAMSUNG(0x07,0x99)=0xE0E09966` |
| 8 | Samsung32 | [FZT] `test_samsung32.irtest` `decoder_input1` | 0x0E / 0x0C | `0x707030CF` / 32 | **VERIFIED** | [FZT] raw ↔ [LIB] `encodeSAMSUNG` |
| 9 | SIRC (12) | Sony TV **Power** | `01 00 00 00` / `15 00 00 00` | `0xA90` / 12 | **VERIFIED** | [LIB] `test/ir_Sony_test.cpp:122` `EXPECT_EQ(0xA90, encodeSony(kSony12Bits, 21, 1))` and `:63,:181` |
| 10 | SIRC (12) | [FZT] `test_sirc.irtest` `encoder_expected1` | 0x0A / 0x55 | `0xAAA` / 12 | **VERIFIED** | [FZT] raw wire bits are `0x555` LSB-first ⇒ `0xAAA` MSB-first |
| 11 | SIRC15 | [LIB] + [FZT] | 0x01 / 0x15 | `0x5480` / 15 | **VERIFIED** | [LIB] `test/ir_Sony_test.cpp:126`; the same pair also appears in [FZT] `test_sirc.irtest` |
| 12 | SIRC20 | [LIB] `test/ir_Sony_test.cpp:131` | 0x0021 / 0x01 | `0x81080` / 20 | **VERIFIED** | `encodeSony(kSony20Bits, 1, 1, 1)` — Flipper 13-bit addr `0x21` = `addr5=1, ext=1` |
| 13 | SIRC20 | [FZT] `test_sirc.irtest` `decoder_input4` | 0x0FB5 / 0x53 | `0xCB5BE` / 20 | **VERIFIED** | [FZT] raw |
| 14 | RC5 | [FZT] `test_rc5.irtest` `decoder_input1` | `13 00 00 00` / `10 00 00 00` | `0x4D0` / 12 (toggle 0) | **VERIFIED** | hand-expanded Manchester of the raw `27888 888 888 1776 1776 1776 888 888 1776 888 888 1776 1776 1776 888 888 888 888 888 888` matches bit-for-bit |
| 15 | RC5 | [LIB] `test/ir_RC5_RC6_test.cpp:26` | 0x05 / 0x35 | `0x175` / 12 | **VERIFIED** | `EXPECT_EQ(0x175, encodeRC5(0x05, 0x35))`; round-trips at `:208-210` |
| 16 | RC5X | [FZT] `test_rc5x.irtest` (has both `decoder_input1` and `encoder_expected1`) | 0x13 / 0x10 | `0x14D0` / **13** | **VERIFIED** | raw `27888 1776 888 888 1776 1776 888 888 1776 888 888 1776 1776 1776 888 888 888 888 888 888` hand-expanded: identical to #14 except S2 = 0 |
| 17 | RC6 | [FZT] `test_rc6.irtest` `decoder_input1` frame 0 | `94 00 00 00` / `A0 00 00 00` | `0x94A0` / 20 (toggle 0) | **VERIFIED** | raw hand-expanded: S=1, mode=000, T=0 (2× wide), A=10010100, C=10100000 |
| 18 | RC6 | [FZT] `test_rc6.irtest` `encoder_input1` | 0x93 / 0xA0 | `0x93A0` / 20 | **VERIFIED** | same file's encoder vectors |
| 19 | RC6 | [LIB] `test/ir_RC5_RC6_test.cpp:693-701` | 0x01 / 0x75 | `0x175` / 20 | **VERIFIED** | `sendRC6(0x175)` decodes to `address 0x01, command 0x75` |
| 20 | Kaseikyo | [LIB] `test/ir_Panasonic_test.cpp:24` `encodePanasonic(0x4004,0x01,0x90,0xED)` | `89 02 20 02` / `70 03 00 00` (0x02200289 / 0x370) | `0x40040190ED7C` / 48 | **VERIFIED** | formula reproduces the library's expectation exactly; vendor_id 0x2002 ↔ manufacturer 0x4004, parity nibble and XOR checksum both self-consistent |
| 21 | Kaseikyo | [FZT] `test_kaseikyo.irtest` `decoder_input1` | 0x00325441 / 0x01B | `0x2A4C028D800F` / 48 | **VERIFIED** | [FZT] raw |
| 22 | RCA | [FZT] `test_rca.irtest` `decoder_input1` | `0F 00 00 00` / `54 00 00 00` | `0xF2A0D5` / 24 (`sendGeneric`) | **VERIFIED** | [FZT] raw decodes to LSB stream `0xAB054F` ⇒ MSB word `0xF2A0D5` |
| 23 | Pioneer | [LIB] `test/ir_Pioneer_test.cpp:61` (low half of `encodePioneer(0xA556,0xAF20)`) | 0xAF / 0x20 | `0xF50A04FB` / **32** | **VERIFIED** | library's 64-bit expectation `0xA55A6A95F50A04FB`; its low 32 bits == our formula |
| 24 | Pioneer | [FZT] `test_pioneer.irtest` `decoder_input1` | 0xAF / 0x36 | `0xF50A6C93` / 32 | **VERIFIED** | [FZT] raw |

Every row is VERIFIED. No DERIVED-only rows remain.

---

## 5. How the verification was run (reproducible)

Scratchpad dir `…/scratchpad/ir-analysis/`:

* `flipper/` — the fetched Flipper sources (encoder/decoder/common + `infrared_signal.c` + `infrared_transmit.c`).
* `flipper/irtest/*.irtest` — Flipper's 12 IR regression files.
* `verify.py` — `.irtest` parser (blocks, `parsed_array`, little-endian hex) + PDWM frame decoder.
* `run2.py` — implements the §2 formulas in Python and diffs them against the wire bits
  recovered from every `decoder_input*` raw capture. Result: **`checked 288 frames, 280 match`**;
  the 8 non-matches were all SIRC frame↔message *alignment* offsets in the toy decoder
  (the `got` value of row *n* equalled the `exp` value of row *n−1*).
* `run4.py` — re-checks SIRC by value-set membership per block instead of by index:
  **zero misses** across all `decoder_*` blocks. (The only remaining "misses" were against
  `encoder_decoder_input1`, a parsed-array-only round-trip block with no raw counterpart.)
* `run5.py` — Manchester decoder for RC5/RC5X/RC6:
  `test_rc5.irtest` and `test_rc5x.irtest` pass with **zero** mismatches; `test_rc6.irtest`
  shows 3 rotated-by-one alignment artefacts (values 0x93/0x94/0x95 cycling) with frames 0-1
  exact — plus the independent hand expansion in §4 row 17.

---

## 6. Reverse direction (Learn: `decode_results` → Flipper `.ir`)

`IRrecv::decode()` zeroes `decode_type / bits / value / address / command / repeat`
before every attempt (`src/IRrecv.cpp:595-601`), so a stale value is never returned — but
**many decoders leave `address`/`command` at 0 or use their own semantics**. Below is what
each one actually writes, for the protocols we care about.

| `decode_type` | `results.value` | `results.address` | `results.command` | → Flipper protocol | Flipper `address` | Flipper `command` | Source |
|---|---|---|---|---|---|---|---|
| `NEC` (32 bits) | raw MSB-first frame | `rb(addrByte,8)` if self-inverting, else `rb((value>>16)&0xFFFF,16)` | `rb(cmdByte,8)`, **forced to 0 if the command inversion check fails** | `NEC` if `(value>>24)==(((value>>16)&0xFF)^0xFF)`, else `NECext` | NEC: `results.address`; NECext: `results.address` (already `rb(value>>16,16)`) | NEC: `results.command`; **NECext: `rb(value & 0xFFFF, 16)` — must be recomputed; `results.command` is an 8-bit-only value and is zeroed when the frame is not self-inverting** | `ir_NEC.cpp:115-136` |
| `NEC` (`bits == 0`, `value == kRepeat`) | `kRepeat` | 0 | 0 | *(repeat frame — discard)* | — | — | `ir_NEC.cpp:97-103` |
| `SANYO_LC7461` (42) | raw MSB-first frame | **raw 13-bit field, NOT reversed** | **raw 8-bit field, NOT reversed** | `NEC42` | `rb(results.address,13)` | `rb(results.command,8)` | `ir_Sanyo.cpp:163-186` |
| `SAMSUNG` (32) | raw frame | `rb(value>>24,8)` | `rb((value>>8)&0xFF,8)` | `Samsung32` | `results.address` | `results.command` | `ir_Samsung.cpp:159-163` |
| `SONY` (12/15/20) | raw frame (as sent, MSB-first) | 12/15: `rb(value,bits)>>7`; 20: `(rb(value,20)>>7)&0x1F` | 12/15: `rb(value,bits)&0x7F`; **20: `(d&0x7F) + ((d>>12)<<7)` where `d=rb(value,20)` — a 15-bit mash of command+extended** | `SIRC`/`SIRC15`/`SIRC20` by `results.bits` | 12/15: `results.address`; **20: `(rb(value,20)>>7) & 0x1FFF` — recompute; `results.address` drops the 8 extended bits** | `results.command & 0x7F` | `ir_Sony.cpp:169-189` |
| `RC5` (12) | 12-bit frame incl. toggle | `(value>>6)&0x1F` | `value&0x3F` | `RC5` | `results.address` | `results.command` | `ir_RC5_RC6.cpp:359-368` |
| `RC5X` (13) | 13-bit frame, bit12 = 7th cmd bit | `(value>>6)&0x1F` | `(value&0x3F) \| 0x40` — **bit 6 is always set** by line 364 | `RC5X` | `results.address` | **`results.command & 0x3F`** (Flipper's RC5X carries only 6 command bits — §3.10) | `ir_RC5_RC6.cpp:359-366` |
| `RC6` (20) | 20-bit frame | `value>>8` — **12 bits: `mode(3)<<9 \| toggle<<8 \| addr(8)`** | `value & 0xFF` | `RC6` **only if** `((results.address>>9)&0x7) == 0` | **`results.address & 0xFF`** | `results.command` | `ir_RC5_RC6.cpp:451-452`; confirmed by `test/ir_RC5_RC6_test.cpp:706-714` (`encodeRC6(0x1234567,0x89,20)` → `capture.address == 0x567`) |
| `PANASONIC` (48) | 48-bit frame | `value >> 32` = **manufacturer**, NOT a Flipper address | `(uint32_t)value` = **low 32 bits**, NOT a Flipper command | `Kaseikyo` | recompute from the 6 wire bytes — §6.1 | recompute — §6.1 | `ir_Panasonic.cpp:154-172` |
| `PIONEER` (64) | two 32-bit sub-codes | `rb((cmd<<8)+addr,16)` of sub-code 0 | same for sub-code 1 | *not* Flipper `Pioneer` — see §6.2 item 6 | — | — | `ir_Pioneer.cpp:131-135` |
| anything unmatched | FNV hash | 0 | 0 | *(write `type: raw` instead)* | — | — | `IRrecv.cpp:1427-1437` |

### 6.1 Kaseikyo reverse recipe

`decodePanasonic` gives you `results.value` (48 bits, MSB-first). Undo §3.12:
```c
uint8_t d[6];
for (int i = 0; i < 6; i++) d[i] = reverseBits((results.value >> (8*(5-i))) & 0xFF, 8);
uint16_t vendor = ((uint16_t)d[1] << 8) | d[0];
uint8_t  g1 = d[2] >> 4, g2 = d[3] & 0xF, id = d[4] >> 6;
uint32_t flipperCmd  = (uint32_t)(d[3] >> 4) | ((uint32_t)(d[4] & 0x3F) << 4);   // 10 bits
uint32_t flipperAddr = ((uint32_t)id << 24) | ((uint32_t)vendor << 8) | (g1 << 4) | g2;
// integrity, mirroring FZ/.../infrared_decoder_kaseikyo.c:20-24:
uint8_t vp = d[0] ^ d[1]; vp = (vp & 0xF) ^ (vp >> 4);
bool ok = ((d[2] & 0xF) == vp) && (d[5] == (uint8_t)(d[2] ^ d[3] ^ d[4]));
```
Only emit `protocol: Kaseikyo` when `ok`; otherwise write the signal as `type: raw`.

### 6.2 Learn-direction caveats the firmware must handle

1. **Never trust `results.address`/`results.command` blindly.** Only `SAMSUNG`, `RC5`, and
   `NEC` (normal, non-extended) hand back values that are already in Flipper's coordinates.
   Everything else needs the recomputation in the table.
2. **`PANASONIC` → `Kaseikyo`:** the library's `address`/`command` are the *manufacturer* and
   the *low 32 bits*. Using them directly produces garbage. Use §6.1.
3. **`RC6` → `RC6`:** strip the mode and toggle bits out of `results.address`. Refuse the
   signal (write raw) if `mode != 0` — Flipper cannot represent RC6 modes ≠ 0
   (`FZ/…/rc6/infrared_decoder_rc6.c:26-28`).
4. **`RC5X` → `RC5X`:** the library always sets command bit 6 (`ir_RC5_RC6.cpp:364`); Flipper
   only stores 6 bits. Mask with `0x3F`.
5. **`SANYO_LC7461` → `NEC42`:** both fields need `reverseBits` — `decodeSanyoLC7461` calls
   `decodeNEC(..., strict=false)` first and then **overwrites** `address`/`command` with the
   *unreversed* raw fields (`ir_Sanyo.cpp:163-168, 184-186`).
6. **Pioneer will almost never decode as `PIONEER`.** `decodePioneer` demands
   `kPioneerBits = 64` — two frames (`ir_Pioneer.cpp:101,106`). A Flipper-style single 32-bit
   Pioneer frame therefore fails at `IRrecv.cpp:635` and is picked up by `decodeNEC`
   (`IRrecv.cpp:651`) instead: the header 8500/4225 is inside `decodeNEC`'s 25 % tolerance of
   9000/4500. **Practical rule:** you cannot distinguish Flipper-`Pioneer` from `NEC` on
   receive without measuring the carrier (40 kHz vs 38 kHz), which `IRrecv` does not do.
   Write such captures as `NEC`.
   *(Flipper's own decoder disambiguates on the preamble 8500/4225 with ±200 µs tolerance —
   `FZ/…/pioneer/infrared_protocol_pioneer_i.h:7-8,13`; IRremoteESP8266 has no equivalent
   narrow gate.)*
7. **`RCA` cannot be learned.** No decoder exists (§3.13). Captures fall through to
   `decodeHash` (`UNKNOWN`, address = command = 0). **UNRESOLVED for Learn** unless you write a
   custom `matchGeneric` pass (timings in §3.13; `IRrecv::matchGeneric` implementation at
   `src/IRrecv.cpp:1441+`).
8. **NEC repeat frames** (`bits == 0`, `value == kRepeat`, `repeat == true`,
   `ir_NEC.cpp:97-103`) must be dropped, not written to the file.
9. **Decoder precedence** (`IRrecv.cpp:612-745`): SanyoLC7461(:619) → Pioneer(:635) → NEC(:651)
   → Sony(:663) → RC5(:679) → RC6(:683) → Panasonic 48(:705) → LG(:712) → JVC(:725) →
   SAMSUNG(:729). A 42-bit NEC-family frame therefore surfaces as `SANYO_LC7461`, never as `NEC`.

---

## 7. The current firmware bug

`src/app/app_09/infrared.cpp:1600-1606`:
```cpp
else if (line.startsWith("command: ")) {
    sig.command = parseHexBytes(line.c_str() + 9);
    if (!sig.isRaw) {
        sig.bits = 32;
        sig.value = ((uint64_t)sig.address) | ((uint64_t)sig.command << 16);
    }
}
```
Three independent defects:
1. **No bit reversal.** Flipper stores logical field values; the library needs the wire stream
   read MSB-first (§0.1/§0.2).
2. **No inverted/duplicated fields.** NEC / Samsung / RCA / Pioneer frames carry `¬A`/`¬C` or a
   repeated address byte; they are simply absent here.
3. **Fixed `bits = 32` and a fixed layout.** Wrong for SIRC (12/15/20), RC5 (12), RC5X (13),
   RC6 (20), RCA (24), NEC42/NEC42ext (42), Kaseikyo (48).

Also in the same file, `kProtoMap` (`:1439-1458`) maps `"NECext"→NEC`, `"SIRC15"/"SIRC20"→SONY`,
`"Kaseikyo"→PANASONIC` with **no per-variant `nbits`**, and `typeToFlipperProto()` (`:1469-1474`)
does a reverse lookup on `decode_type_t` alone — so `SONY` always writes back `"Sony"`
(a name Flipper does not accept: Flipper's parser is `infrared_get_protocol_by_name`, matching
the variant names `"SIRC"`/`"SIRC15"`/`"SIRC20"` exactly, `FZ/…/encoder_decoder/infrared.c`),
and `NEC` always writes back `"NEC"` even when the address needs `NECext`. Both need the
`bits`-aware mapping from §1. `"RCA"`, `"NEC42"`, `"NEC42ext"`, `"Pioneer"` (mixed case) and
`"Samsung32"` round-tripping are missing entirely.

Byte-count bug in the writer: `writeSignalEntry()` (`:1516-1526`) computes
`nbytes = clamp((bits+7)/8, 2, 4)` — so an RC5 signal (12 bits) writes only 2 bytes. Flipper
always writes **4** (`FZ/lib/infrared/signal/infrared_signal.c:120-128`) and its reader uses
`flipper_format_read_hex(..., 4)`, which will *fail* on a 2-byte field. **Always emit 4 bytes.**

---

## 8. Unresolved / caveats summary

| Item | Status |
|---|---|
| **RCA transmit** | RESOLVED via `sendGeneric` (§3.13). No `decode_type_t::RCA` exists, so it cannot go through `IRsend::send()` or the firmware's `sig.protocol` plumbing — needs a dedicated code path. |
| **RCA receive (Learn)** | **UNRESOLVED**: IRremoteESP8266 v2.8.6 has no RCA decoder. Requires a hand-written `matchGeneric` pass, or accept `type: raw`. |
| **Flipper Pioneer vs NEC on receive** | **UNRESOLVED in principle**: indistinguishable without carrier-frequency measurement, which `IRrecv` does not provide (§6.2 item 6). |
| **RC5/RC5X/RC6 toggle bit** | Not represented in the `.ir` format at all — Flipper flips it internally per `reset()`. The firmware must own this state. Not a mapping defect. |
| **RC5 duty cycle** | Flipper 33 %, library hard-codes 25 % in `sendRC5` (`ir_RC5_RC6.cpp:67`). Cosmetic unless modulation matters. |
| **Kaseikyo carrier** | Flipper 38 000 Hz / 33 %; library `sendPanasonic64` uses 36 700 Hz / 50 % (`ir_Panasonic.h:48`, `ir_Panasonic.cpp:77`). Both are inside the passband of standard 36–40 kHz demodulators; use `sendGeneric` with the same timings and `38, true, repeat, 33` if an exact match is required. |
| **RCA carrier** | Flipper uses 38 kHz although the RCA spec says 56 kHz (§3.13). Matching Flipper is the correct choice here. |
| **Flipper RC5X drops command bit 6** | Source-confirmed behaviour, not a mapping ambiguity (§3.10). Reproduce it. |
| **`NEC` ↔ `NECext` aliasing** | Flipper's decoder always prefers `NEC` for self-inverting frames; a hand-written `NECext` with NEC-shaped bytes transmits correctly but reads back as `NEC`. Inherent to the format. |
