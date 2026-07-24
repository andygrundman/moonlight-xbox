#!/usr/bin/env python3
"""Render captured PyroWave DUs (complete or partial) to PNG previews.

Made for the client's automatic partial-frame captures — the
pyrowave_*_lostNN_*.bin files it drops in LocalState (pull them with Xbox
Device Portal); the NN in the name is the percentage of transmitted blocks
that never arrived. Each capture is decoded with the MoltenVK reference
decoder, zero-filling whatever was lost, and written as <capture>.png next to
the input, so a session's worth of salvaged frames can be judged by eye.

  python3 tools/pyrowave_preview.py capture.bin [capture2.bin ...]

  --third : third-resolution previews (quicker to flip through)
  --hdr / --sdr : force the transfer interpretation; default is from the
      capture's filename (`_hdr_` / `_sdr_`, part of the client's naming)

HDR captures are BT.2020 NCL full-range PQ (the Sunshine encoder's contract)
and are tone-mapped to SDR: PQ -> linear nits, 203-nit paper white, extended
Reinhard rolloff for highlights, sRGB out. It's a review aid, not a mastering
pipeline - judge loss artifacts with it, not color.

Needs MoltenVK and build_tools/pyrowave_dump_golden, like pyrowave_fec_sim.py.
"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pyrowave_fec_sim import ENV, TOOL  # noqa: E402

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402


def read_pwtv(path):
    """Returns (width, height, [Y, Cb, Cr]) with chroma upsampled to full size."""
    d = open(path, "rb").read()
    magic, _, w, h, chroma444, bs = struct.unpack_from("<6I", d, 0)
    if magic != 0x56545750:  # 'PWTV'
        raise ValueError(f"{path}: not a PWTV file")
    off = 24 + bs
    planes = []
    for c in range(3):
        pw, ph = (w, h) if c == 0 or chroma444 else (w // 2, h // 2)
        p = np.frombuffer(d, np.uint8, pw * ph, off).reshape(ph, pw)
        off += pw * ph
        if (pw, ph) != (w, h):
            p = np.asarray(Image.fromarray(p).resize((w, h), Image.BILINEAR))
        planes.append(p.astype(np.float32))
    return w, h, planes


def to_rgb(planes):
    """BT.709 limited-range YCbCr -> RGB, same math as pyrowave_fec_sim's
    previews. Colorimetry fidelity is beside the point here - the question a
    preview answers is where the truncation blur sits and how bad it is."""
    y = (planes[0] - 16) * 255 / 219
    cb = (planes[1] - 128) * 255 / 224
    cr = (planes[2] - 128) * 255 / 224
    rgb = np.stack([y + 1.5748 * cr, y - 0.1873 * cb - 0.4681 * cr, y + 1.8556 * cb], -1)
    return np.clip(rgb, 0, 255).astype(np.uint8)


def pq_to_nits(e):
    """SMPTE ST 2084 EOTF: normalized PQ code -> display luminance in nits."""
    m1, m2 = 2610 / 16384, 2523 / 4096 * 128
    c1, c2, c3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
    ep = np.power(np.clip(e, 0, 1), 1 / m2)
    return 10000 * np.power(np.maximum(ep - c1, 0) / (c2 - c3 * ep), 1 / m1)


def hdr_to_rgb(planes, paper_white=203.0, max_white=1000.0):
    """BT.2020 NCL full-range PQ YCbCr -> tone-mapped sRGB.

    Extended-Reinhard rolloff per channel: linear at paper white, saturating at
    max_white. Per-channel (not luminance-based) mapping desaturates strong
    highlights a little, which is fine for a review aid."""
    y = planes[0] / 255
    cb = (planes[1] - 128) / 255
    cr = (planes[2] - 128) / 255
    # BT.2020 NCL: Kr = 0.2627, Kb = 0.0593
    r = y + 1.4746 * cr
    b = y + 1.8814 * cb
    g = (y - 0.2627 * r - 0.0593 * b) / 0.6780
    rgb = np.stack([r, g, b], -1)

    lin = pq_to_nits(rgb) / paper_white
    w = max_white / paper_white
    lin = lin * (1 + lin / (w * w)) / (1 + lin)

    srgb = np.where(lin <= 0.0031308,
                    12.92 * lin,
                    1.055 * np.power(np.clip(lin, 0, None), 1 / 2.4) - 0.055)
    return (np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8)


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith("-")]
    third = "--third" in sys.argv
    force_hdr = "--hdr" in sys.argv
    force_sdr = "--sdr" in sys.argv
    if not paths:
        print(__doc__.strip())
        return 1

    failed = 0
    for path in paths:
        out_png = os.path.splitext(path)[0] + ".png"
        hdr = force_hdr or (not force_sdr and "_hdr_" in os.path.basename(path))
        with tempfile.NamedTemporaryFile(suffix=".pwtv", delete=False) as tmp:
            pwtv = tmp.name
        try:
            r = subprocess.run([TOOL, "--force-partial", path, pwtv], env=ENV,
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(f"{os.path.basename(path)}: decode failed")
                sys.stderr.write(r.stderr)
                failed += 1
                continue
            w, h, planes = read_pwtv(pwtv)
            img = Image.fromarray(hdr_to_rgb(planes) if hdr else to_rgb(planes))
            if third:
                img = img.resize((w // 3, h // 3), Image.LANCZOS)
            img.save(out_png)
            print(f"{os.path.basename(path)} -> {out_png}{' (PQ tone-mapped)' if hdr else ''}")
        finally:
            os.unlink(pwtv)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
