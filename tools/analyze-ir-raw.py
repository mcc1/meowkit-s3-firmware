#!/usr/bin/env python3
"""Inspect `type: raw` entries of a Flipper `.ir` file captured by MeowKit.

Prints, per raw entry: sample count, total duration, the leading header pair,
the mark/space histograms (clustered), a bit-count estimate from the space
clusters, and hints about truncation or multi-frame captures. Meant for
diagnosing "learned it, replay does nothing" reports without the hardware.

Usage:
    python tools/analyze-ir-raw.py path/to/Device.ir [--name POWER]
"""
import argparse
import statistics
import sys
from collections import Counter


def parse_ir(path):
    entries, cur = [], None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if line.startswith("name:"):
                cur = {"name": line[5:].strip(), "type": "", "frequency": None, "data": []}
                entries.append(cur)
            elif cur is None or line.startswith("#"):
                continue
            elif line.startswith("type:"):
                cur["type"] = line[5:].strip()
            elif line.startswith("protocol:"):
                cur["protocol"] = line[9:].strip()
            elif line.startswith("frequency:"):
                cur["frequency"] = int(line[10:].strip() or 0)
            elif line.startswith("data:"):
                cur["data"] = [int(x) for x in line[5:].split()]
    return entries


def cluster(values, tol=0.18):
    """Greedy 1-D clustering; returns [(center, count)] sorted by center."""
    out = []
    for v in sorted(values):
        for c in out:
            if abs(v - c[0]) <= tol * c[0]:
                c[0] = (c[0] * c[1] + v) / (c[1] + 1)
                c[1] += 1
                break
        else:
            out.append([float(v), 1])
    return [(round(c), n) for c, n in out]


def analyze(entry):
    d = entry["data"]
    print(f"== {entry['name']}  type={entry['type']}  freq={entry['frequency']}  samples={len(d)}")
    if entry["type"] != "raw":
        print(f"   parsed entry ({entry.get('protocol')}), nothing to analyse")
        return
    if len(d) < 4:
        print("   too short")
        return
    marks, spaces = d[0::2], d[1::2]
    total_ms = sum(d) / 1000.0
    print(f"   duration {total_ms:.1f} ms, header mark/space = {d[0]}/{d[1]} us, "
          f"last mark {marks[-1]} us{' (odd count: ends on a mark)' if len(d) % 2 else ''}")

    # Long spaces split frames: anything > 8x the median space is a gap.
    med_space = statistics.median(spaces[1:]) if len(spaces) > 2 else spaces[0]
    gaps = [(i * 2 + 1, s) for i, s in enumerate(spaces) if i > 0 and s > 8 * med_space]
    if gaps:
        print(f"   {len(gaps)} inter-frame gap(s) at sample index/us: "
              + ", ".join(f"{i}:{s}" for i, s in gaps[:6]))
        frames = []
        start = 0
        for i, _ in gaps:
            frames.append((start, i))
            start = i + 1
        frames.append((start, len(d) - 1))
        print("   frame sample counts: " + ", ".join(str(b - a + 1) for a, b in frames))
    else:
        print("   single frame (no gap > 8x median space)")

    body_marks = marks[1:]
    body_spaces = spaces[1:len(spaces) - (0 if len(d) % 2 else 1)] or spaces[1:]
    mc = cluster(body_marks)
    sc = cluster(body_spaces)
    print("   mark clusters  (us:count): " + ", ".join(f"{c}:{n}" for c, n in mc[:6]))
    print("   space clusters (us:count): " + ", ".join(f"{c}:{n}" for c, n in sc[:6]))
    data_bits = sum(n for _, n in sc[:2]) if len(sc) >= 2 else len(body_spaces)
    print(f"   ~{data_bits} data bits if the two largest space clusters are 0/1 "
          f"({data_bits // 8} bytes{'' if data_bits % 8 == 0 else ', not byte aligned'})")

    if len(d) in (1023, 1024, 2047, 2048):
        print("   !! sample count equals a receive-buffer size: capture was truncated")
    if max(d) >= 65535:
        print("   !! a timing hit the 16-bit clamp (65535 us)")
    if body_marks and statistics.pstdev(body_marks) > 0.35 * statistics.mean(body_marks):
        print("   note: mark widths vary a lot; check for noise samples")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--name", help="only this signal name (case-insensitive)")
    args = ap.parse_args()
    entries = parse_ir(args.path)
    if not entries:
        print("no entries found", file=sys.stderr)
        return 1
    for e in entries:
        if args.name and e["name"].lower() != args.name.lower():
            continue
        analyze(e)
    return 0


if __name__ == "__main__":
    sys.exit(main())
