#!/usr/bin/env python3
"""Complete a truncated Hitachi HITACHI_AC344 capture into full raw frames.

MeowKit's IR receiver fades out after ~200 ms of continuous carrier, so the
344-bit (43-byte) Hitachi frame of e.g. a RAS-22NK remote is captured only up
to ~25 bytes. The first 25 bytes carry the remote's identity, button and
temperature; the missing tail holds mode/fan (byte 25), power (byte 27) and
fixed bytes. This tool rebuilds the tail from the IRremoteESP8266
IRHitachiAc344 layout (lib/IRremoteESP8266/src/ir_Hitachi.{h,cpp}) and writes
one `type: raw` entry per variant into a new Flipper `.ir` device file.

Layout (Hitachi424Protocol union, 344 variant):
  0..2   fixed 01 10 00
  3..    inverted byte pairs: byte[i+1] = ~byte[i] for i = 3, 5, 7, ...
  11     Button (0x13 power/mode, 0x42 fan, 0x43 temp-, 0x44 temp+, 0x81 swingV)
  13     Temp << 2
  25     Fan << 4 | Mode   (mode: 1 fan, 3 cool, 5 dry, 6 heat; fan 1..6, 5 = auto)
  27     0xE1 | Power << 4
  33     0x80,  35 0x03 (SwingH),  37 SwingV << 5,  39 0x00
Wire: header 3300/1700, bit mark 400, one 1250, zero 500, footer 400, LSB-first,
38 kHz (IRsend::sendHitachiAC with nbytes == 43).

Usage:
  python tools/complete-hitachi344.py L:/infrared/HITACHI.ir --name ON \
      --out L:/infrared/HITACHI344.ir [--temp 26]
"""
import argparse
import sys

STATE_LEN = 43
HDR_MARK, HDR_SPACE = 3300, 1700
BIT_MARK, ONE_SPACE, ZERO_SPACE = 400, 1250, 500
MODES = {"fan": 1, "cool": 3, "dry": 5, "heat": 6}
FAN_AUTO, FAN_DRY_MAX = 5, 2


def parse_entry(path, name):
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if line.startswith("name:"):
                cur = {"name": line[5:].strip(), "type": "", "data": []}
            elif cur is None or line.startswith("#"):
                continue
            elif line.startswith("type:"):
                cur["type"] = line[5:].strip()
            elif line.startswith("data:"):
                cur["data"] = [int(x) for x in line[5:].split()]
                if cur["name"].lower() == name.lower():
                    return cur
    return None


def bits_from_raw(data):
    """Pulse-distance decode by bit period (robust against faded marks)."""
    bits = []
    for i in range(2, len(data) - 1, 2):
        period = data[i] + data[i + 1]
        if period > 5000:
            break
        bits.append(1 if period > 1300 else 0)
    return bits


def bytes_lsb_first(bits):
    out, spare = [], bits[len(bits) - len(bits) % 8:]
    for i in range(0, len(bits) - len(bits) % 8, 8):
        chunk = bits[i:i + 8]
        out.append(sum(b << k for k, b in enumerate(chunk)))
    return out, spare


def invert_pairs(state):
    for i in range(3, STATE_LEN - 1, 2):
        state[i + 1] = (~state[i]) & 0xFF
    return state


def build(captured, *, mode, fan, power, temp=None, swing_v=False):
    st = [0] * STATE_LEN
    st[:len(captured)] = captured[:STATE_LEN]
    st[0], st[1], st[2] = 0x01, 0x10, 0x00
    if temp is not None:
        st[13] = (st[13] & 0x03) | ((max(16, min(32, temp)) & 0x3F) << 2)
    st[25] = ((fan & 0x0F) << 4) | (mode & 0x0F)
    st[27] = 0xE1 | ((1 if power else 0) << 4)
    st[33] = 0x80
    st[35] = 0x03
    st[37] = (1 << 5) if swing_v else 0x00
    st[39] = 0x00
    return invert_pairs(st)


def render(state):
    out = [HDR_MARK, HDR_SPACE]
    for byte in state:
        for k in range(8):
            out.append(BIT_MARK)
            out.append(ONE_SPACE if (byte >> k) & 1 else ZERO_SPACE)
    out.append(BIT_MARK)
    return out


def entry_text(name, timings, note):
    return (f"#\n# meowkit: {note}\nname: {name}\ntype: raw\nfrequency: 38000\n"
            f"duty_cycle: 0.330000\ndata: {' '.join(map(str, timings))}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--name", default="ON")
    ap.add_argument("--out", required=True)
    ap.add_argument("--temp", type=int, help="override temperature 16..32")
    args = ap.parse_args()

    e = parse_entry(args.path, args.name)
    if not e or e["type"] != "raw":
        print("raw entry not found", file=sys.stderr)
        return 1
    bits = bits_from_raw(e["data"])
    captured, spare = bytes_lsb_first(bits)
    print(f"captured {len(e['data'])} samples -> {len(bits)} bits -> {len(captured)} bytes + {len(spare)} spare bits {spare}")
    print("bytes:", " ".join(f"{b:02X}" for b in captured))

    if captured[:3] != [0x01, 0x10, 0x00]:
        print("!! header bytes are not 01 10 00; this is not a Hitachi 344/424 frame")
        return 1
    bad = [i for i in range(3, len(captured) - 1, 2) if captured[i + 1] != (~captured[i]) & 0xFF]
    print("inverted-pair check on captured bytes:", "OK" if not bad else f"MISMATCH at {bad}")
    temp = captured[13] >> 2
    print(f"button 0x{captured[11]:02X}, temp {temp} C, unknown bytes 9/19/21/23 = "
          f"{captured[9]:02X}/{captured[19]:02X}/{captured[21]:02X}/{captured[23]:02X}")
    if len(spare) >= 2:
        low2 = spare[0] | (spare[1] << 1)
        guess = [m for m, v in MODES.items() if (v & 3) == low2]
        print(f"byte 25 low 2 bits = {low2:02b} -> mode candidates {guess}")

    t = args.temp
    variants = [
        ("ON_COOL", dict(mode=MODES["cool"], fan=FAN_AUTO, power=True, temp=t)),
        ("ON_HEAT", dict(mode=MODES["heat"], fan=FAN_AUTO, power=True, temp=t)),
        ("ON_DRY", dict(mode=MODES["dry"], fan=FAN_DRY_MAX, power=True, temp=t)),
        ("ON_FANONLY", dict(mode=MODES["fan"], fan=1, power=True, temp=27)),
        ("OFF", dict(mode=MODES["cool"], fan=FAN_AUTO, power=False, temp=t)),
        ("ON_COOL_SWING", dict(mode=MODES["cool"], fan=FAN_AUTO, power=True, temp=t, swing_v=True)),
    ]
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("Filetype: IR signals file\nVersion: 1\n")
        for name, kw in variants:
            st = build(captured, **kw)
            timings = render(st)
            # self-check: decode what we render and compare
            rb, _ = bytes_lsb_first(bits_from_raw(timings))
            assert rb == st, name
            note = (f"completed from {len(captured)} captured bytes; mode={kw['mode']} fan={kw['fan']} "
                    f"power={int(kw['power'])} temp={st[13] >> 2}")
            f.write(entry_text(name, timings, note))
            print(f"{name:14s} {len(timings)} samples  state: {' '.join(f'{b:02X}' for b in st)}")
    print("wrote", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
