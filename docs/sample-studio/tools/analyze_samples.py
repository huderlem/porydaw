#!/usr/bin/env python3
"""Measure level statistics of a DirectSound sample corpus (.wav files).

Purpose: derive/verify the Sample Studio normalization constants
(kTargetLoopRms, kPeakCeiling) from a reference sample set -- canonically
the sc88pro instrument samples shipped in pokeemerald.

Usage:
    python3 analyze_samples.py <dir-or-glob> [more...]
    python3 analyze_samples.py ~/pokeemerald/sound/direct_sound_samples/sc88pro_*.wav

All measurements are reported in the GBA sample domain: signed 8-bit,
full scale = 128 (so a value V corresponds to V/128 of full scale, and
dBFS = 20*log10(V/128)). Hi-res inputs (16/24/32-bit, float) are scaled
into that domain without quantizing, so the script also works on
pre-quantization masters.

Stdlib only. No external dependencies.
"""

import glob
import math
import os
import struct
import sys


def parse_wav(path):
    """Minimal RIFF/WAVE parser. Returns dict with samples (float, FS=128
    scale, mono-downmixed), rate, loop info, agbp/agbl if present."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 12 or blob[0:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    fmt = None
    data = None
    loop = None          # (start, end_inclusive)
    agbp = None
    agbl = None

    pos = 12
    while pos + 8 <= len(blob):
        cid = blob[pos:pos + 4]
        (clen,) = struct.unpack_from("<I", blob, pos + 4)
        body = blob[pos + 8:pos + 8 + clen]
        if cid == b"fmt ":
            tag, ch, rate, _, block, bits = struct.unpack_from("<HHIIHH", body, 0)
            fmt = dict(tag=tag, channels=ch, rate=rate, block=block, bits=bits)
        elif cid == b"data":
            data = body
        elif cid == b"smpl":
            (nloops,) = struct.unpack_from("<I", body, 28)
            if nloops >= 1:
                # first loop record at offset 36: id, type, start, end, frac, count
                _, ltype, lstart, lend = struct.unpack_from("<IIII", body, 36)
                if ltype == 0:
                    loop = (lstart, lend)  # end is INCLUSIVE per smpl spec
        elif cid == b"agbp" and clen >= 4:
            (agbp,) = struct.unpack_from("<I", body, 0)
        elif cid == b"agbl" and clen >= 4:
            (agbl,) = struct.unpack_from("<I", body, 0)
        pos += 8 + clen + (clen & 1)  # RIFF pad byte on odd sizes

    if fmt is None or data is None:
        raise ValueError("missing fmt or data chunk")

    ch = fmt["channels"]
    bits = fmt["bits"]
    tag = fmt["tag"]

    # Decode to per-frame mono float in FS=128 scale.
    if tag == 1 and bits == 8:
        raw = [b - 128 for b in data]
        scale = 1.0
    elif tag == 1 and bits == 16:
        raw = struct.unpack("<%dh" % (len(data) // 2), data)
        scale = 128.0 / 32768.0
    elif tag == 1 and bits == 24:
        n = len(data) // 3
        raw = []
        for i in range(n):
            b0, b1, b2 = data[3 * i], data[3 * i + 1], data[3 * i + 2]
            v = b0 | (b1 << 8) | (b2 << 16)
            if v & 0x800000:
                v -= 1 << 24
            raw.append(v)
        scale = 128.0 / 8388608.0
    elif tag == 1 and bits == 32:
        raw = struct.unpack("<%di" % (len(data) // 4), data)
        scale = 128.0 / 2147483648.0
    elif tag == 3 and bits == 32:
        raw = struct.unpack("<%df" % (len(data) // 4), data)
        scale = 128.0
    elif tag == 3 and bits == 64:
        raw = struct.unpack("<%dd" % (len(data) // 8), data)
        scale = 128.0
    else:
        raise ValueError("unsupported format tag=%d bits=%d" % (tag, bits))

    if ch > 1:
        frames = len(raw) // ch
        mono = [sum(raw[i * ch:(i + 1) * ch]) / ch for i in range(frames)]
    else:
        mono = list(raw)
    samples = [v * scale for v in mono]

    return dict(samples=samples, rate=fmt["rate"], loop=loop, agbp=agbp, agbl=agbl)


def rms(xs):
    if not xs:
        return 0.0
    return math.sqrt(sum(v * v for v in xs) / len(xs))


def db(v):
    if v <= 0:
        return float("-inf")
    return 20.0 * math.log10(v / 128.0)


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * p
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return sorted_vals[lo]
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def analyze(path):
    w = parse_wav(path)
    s = w["samples"]
    n = len(s)
    if n == 0:
        raise ValueError("empty sample")
    peak = max(abs(v) for v in s)
    full = rms(s)
    looped = w["loop"] is not None
    loop_r = None
    if looped:
        a, b = w["loop"]
        b = min(b + 1, n)  # smpl end is inclusive
        if b > a:
            loop_r = rms(s[a:b])
    sustain = rms(s[n // 4:]) if not looped else loop_r  # attack-excluded
    crest = peak / full if full > 0 else float("inf")
    return dict(name=os.path.basename(path), n=n, rate=w["rate"], looped=looped,
                peak=peak, rms_full=full, rms_loop=loop_r, rms_sustain=sustain,
                crest=crest, agbp=w["agbp"], agbl=w["agbl"])


def summarize(label, vals):
    vs = sorted(v for v in vals if v is not None)
    if not vs:
        print("  %-22s (none)" % label)
        return
    med = percentile(vs, 0.5)
    q1 = percentile(vs, 0.25)
    q3 = percentile(vs, 0.75)
    print("  %-22s median %6.1f (%6.2f dBFS)   IQR [%5.1f, %5.1f]   min %5.1f  max %5.1f"
          % (label, med, db(med), q1, q3, vs[0], vs[-1]))


def main(argv):
    paths = []
    for arg in argv:
        if os.path.isdir(arg):
            paths.extend(sorted(glob.glob(os.path.join(arg, "*.wav"))))
        else:
            paths.extend(sorted(glob.glob(arg)))
    if not paths:
        print("usage: analyze_samples.py <dir-or-glob> [more...]", file=sys.stderr)
        return 2

    results = []
    for p in paths:
        try:
            results.append(analyze(p))
        except ValueError as e:
            print("skip %s: %s" % (p, e), file=sys.stderr)

    looped = [r for r in results if r["looped"]]
    oneshot = [r for r in results if not r["looped"]]

    print("%d files (%d looped, %d one-shot)   [FS = 128]" %
          (len(results), len(looped), len(oneshot)))
    print("\nAll files:")
    summarize("peak", [r["peak"] for r in results])
    summarize("full RMS", [r["rms_full"] for r in results])
    print("\nLooped (tonal):")
    summarize("peak", [r["peak"] for r in looped])
    summarize("loop-region RMS", [r["rms_loop"] for r in looped])
    summarize("crest (peak/RMS)", [r["crest"] for r in looped])
    print("\nOne-shot (percussive):")
    summarize("peak", [r["peak"] for r in oneshot])
    summarize("full RMS", [r["rms_full"] for r in oneshot])
    summarize("sustain RMS (skip 25%)", [r["rms_sustain"] for r in oneshot])
    summarize("crest (peak/RMS)", [r["crest"] for r in oneshot])

    print("\nPer-file detail:")
    print("  %-28s %6s %7s %6s %6s %8s %8s" %
          ("name", "rate", "frames", "loop", "peak", "fullRMS", "loopRMS"))
    for r in results:
        print("  %-28s %6d %7d %6s %6.1f %8.2f %8s" %
              (r["name"], r["rate"], r["n"], "yes" if r["looped"] else "no",
               r["peak"], r["rms_full"],
               ("%.2f" % r["rms_loop"]) if r["rms_loop"] is not None else "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
