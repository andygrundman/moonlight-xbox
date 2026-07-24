#!/usr/bin/env python3
# PyroWave packet-loss simulator: surgically drops packets from a captured
# frame, decodes each scenario with the reference decoder (via
# pyrowave_dump_golden --allow-partial), and reports PSNR + preview images.
import os, struct, subprocess, random, sys
from array import array

REPO = "/Users/andy/dev/moonlight-xbox"
SC = os.path.dirname(os.path.abspath(__file__))
CAPTURE = f"{REPO}/tools/testdata/pyrowave_frame_3840x2160_444_sdr.bin"
TOOL = f"{REPO}/build_tools/pyrowave_dump_golden"
ENV = dict(os.environ,
           DYLD_LIBRARY_PATH="/opt/homebrew/lib",
           VK_ICD_FILENAMES="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
           PYROWAVE_PRECISION="2")

W, H = 2560, 1440

# --- parse the framed capture into (is_seq, block_index, bytes) packets ---
data = open(CAPTURE, "rb").read()
count = struct.unpack_from("<I", data, 0)[0]
packets = []  # (is_seq, block_index, raw_bytes) in stream order
pos = 4
for _ in range(count):
    sz = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    chunk = data[pos:pos+sz]
    pos += sz
    # a chunk holds one or more self-delimiting pyrowave packets
    cp = 0
    while cp + 8 <= len(chunk):
        u16b = struct.unpack_from("<H", chunk, cp+2)[0]
        if u16b & 0x8000:
            packets.append((True, -1, chunk[cp:cp+8]))
            cp += 8
        else:
            psize = (u16b & 0x0FFF) * 4
            blk = struct.unpack_from("<I", chunk, cp+4)[0] >> 8
            packets.append((False, blk, chunk[cp:cp+psize]))
            cp += psize
print(f"capture: {len(packets)} packets ({sum(1 for p in packets if p[0])} seq)")

# --- block-index ranges per (level, comp, band), mirroring init_block_meta ---
def band_ranges(w, h, chroma444=True):
    aw = max((w + 31) & ~31, 128); ah = max((h + 31) & ~31, 128)
    ranges = {}
    blocks = 0
    for level in range(4, -1, -1):
        for comp in range(3):
            if level == 0 and comp != 0 and not chroma444:
                continue
            for band in range(0 if level == 4 else 1, 4):
                bw, bh = (aw // 2) >> level, (ah // 2) >> level
                n = -(-bw // 32) * (-(-(-(-bh // 8)) // 4))
                ranges[(level, comp, band)] = (blocks, blocks + n)
                blocks += n
    return ranges

R = band_ranges(W, H)
def in_bands(blk, keys):
    return any(R[k][0] <= blk < R[k][1] for k in keys)

LL4_Y  = [(4, 0, 0)]
LL4_ALL = [(4, c, 0) for c in range(3)]
L1_ALL = [(1, c, b) for c in range(3) for b in (1, 2, 3)]

# data-chunk index per packet (seq header excluded), for burst/random drops
data_idx = 0
pkt_chunk = []
for is_seq, blk, raw in packets:
    pkt_chunk.append(-1 if is_seq else data_idx)
    if not is_seq:
        data_idx += 1
n_data = data_idx
rng = random.Random(0xC0FFEE)
random_10 = set(rng.sample(range(n_data), n_data // 10))

scenarios = {
    "baseline":        lambda i, blk: False,
    "ll4_y_1block":    lambda i, blk: blk == 0,
    "ll4_y_all":       lambda i, blk: blk >= 0 and in_bands(blk, LL4_Y),
    "ll4_all":         lambda i, blk: blk >= 0 and in_bands(blk, LL4_ALL),
    "level1_all":      lambda i, blk: blk >= 0 and in_bands(blk, L1_ALL),
    "burst_1pkt_early": lambda i, blk: i in (0, 1),          # first 2 data packets (L4)
    "burst_20pkt_mid": lambda i, blk: 900 <= i < 920,        # ~L1 territory
    "random_10pct":    lambda i, blk: i in random_10,
}

results = {}
for name, drop in scenarios.items():
    out_stream = bytearray()
    kept = dropped = 0
    dropped_bytes = 0
    for (is_seq, blk, raw), ci in zip(packets, pkt_chunk):
        if not is_seq and drop(ci, blk):
            dropped += 1; dropped_bytes += len(raw)
            continue
        kept += 1
        out_stream += struct.pack("<I", len(raw)) + raw
    framed = struct.pack("<I", kept) + bytes(out_stream)
    cap = f"{SC}/loss_{name}.cap"
    open(cap, "wb").write(framed)
    out = f"{SC}/loss_{name}.pwtv"
    r = subprocess.run([TOOL, "--allow-partial", cap, out], env=ENV,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{name}: DECODE FAILED\n{r.stderr}")
        continue
    results[name] = (out, dropped, dropped_bytes)
    print(f"{name}: dropped {dropped} pkts / {dropped_bytes} bytes")

# --- PSNR vs baseline + previews ---
def load_planes(path):
    d = open(path, "rb").read()
    _, _, w, h, _, bs = struct.unpack_from("<6I", d, 0)
    off = 24 + bs
    return [d[off + c*w*h : off + (c+1)*w*h] for c in range(3)]

base = load_planes(results["baseline"][0])
import math
print(f"\n{'scenario':18s} {'lost pkts':>9s} {'lost bytes':>10s} {'%frame':>7s} {'Y PSNR':>8s}")
for name, (path, dropped, dbytes) in results.items():
    planes = load_planes(path)
    if name == "baseline":
        print(f"{name:18s} {0:9d} {0:10d} {0.0:6.1f}% {'ref':>8s}")
        continue
    se = 0
    g, p = base[0], planes[0]
    for i in range(0, W*H):
        d = g[i] - p[i]
        se += d*d
    psnr = 10*math.log10(255*255*W*H/se) if se else float("inf")
    print(f"{name:18s} {dropped:9d} {dbytes:10d} {100.0*dbytes/350588:6.1f}% {psnr:7.2f}dB")

# --- render half-res previews ---
for name, (path, _, _) in results.items():
    planes = load_planes(path)
    pw, ph = W//2, H//2
    out = bytearray(pw*ph*3)
    i = 0
    Y, Cb, Cr = planes
    for yy in range(0, H, 2):
        row = yy*W
        for xx in range(0, W, 2):
            y = (Y[row+xx]-16)*255/219
            cb = (Cb[row+xx]-128)*255/224; cr = (Cr[row+xx]-128)*255/224
            r_ = y + 1.5748*cr; g_ = y - 0.1873*cb - 0.4681*cr; b_ = y + 1.8556*cb
            out[i]   = max(0, min(255, int(r_)))
            out[i+1] = max(0, min(255, int(g_)))
            out[i+2] = max(0, min(255, int(b_)))
            i += 3
    ppm = f"{SC}/loss_{name}.ppm"
    with open(ppm, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (pw, ph)); f.write(out)
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", f"{SC}/loss_{name}.png"],
                   capture_output=True)
print("previews written")
