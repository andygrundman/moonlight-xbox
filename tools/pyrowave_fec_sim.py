#!/usr/bin/env python3
"""Measure the head/bare-tail FEC split against Sunshine's even split, under packet loss.

Models the full delivery path on a real capture: split the frame into FEC blocks
(the implemented layout: sequence header + L4 + L3 at 50% parity, tail at 0%),
drop RTP packets, recover what Reed-Solomon can, deliver the contiguous prefix
the client would salvage, apply the client's coarse-coverage quality floor, and
decode what survives with the reference decoder.

Monte Carlo gives the statistics (how often a frame survives whole, how much of it
survives when it doesn't); a handful of representative prefixes are decoded for PSNR.

  python3 tools/pyrowave_fec_sim.py [capture.bin]

Needs MoltenVK and build_tools/pyrowave_dump_golden, same as pyrowave_loss_sim.py.
"""
import math
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pyrowave_tier_sim import (  # noqa: E402
    DATA_SHARDS_MAX,
    FEC_PROTECTED_THROUGH_LEVEL,
    MAX_FEC_BLOCKS,
    compute_head_bytes,
    compute_level_block_ends,
    level_offsets,
    parse_capture,
    split_fec_blocks_head_tail,
)

import numpy as np  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRATCH = os.path.join(REPO, "build_tools", "fecsim")
TOOL = os.path.join(REPO, "build_tools", "pyrowave_dump_golden")
ENV = dict(os.environ,
           DYLD_LIBRARY_PATH="/opt/homebrew/lib",
           VK_ICD_FILENAMES="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
           PYROWAVE_PRECISION="2")

# videoBroadcastThread() geometry
NV_VIDEO_PACKET = 16
MAX_RTP_HEADER_SIZE = 16
FRAME_HEADER = 8
VIDEO_PACKET_RAW = NV_VIDEO_PACKET + 12 + 4  # sizeof(video_packet_raw_t)
MIN_PARITY = 2

TRIALS = 4000


def split_evenly(payload_size, blocksize, fec_percentage):
    """Sunshine's existing split (stream.cpp), for comparison."""
    max_data_shards = (DATA_SHARDS_MAX * 100) // (100 + fec_percentage)
    blocks_needed = (payload_size + max_data_shards * blocksize - 1) // (max_data_shards * blocksize)
    if blocks_needed > MAX_FEC_BLOCKS:
        blocks_needed, fec_percentage = MAX_FEC_BLOCKS, 0

    total_packets = payload_size // blocksize
    aligned = ((payload_size // blocks_needed + blocksize - 1) // blocksize) * blocksize
    starts = [min(x * aligned // blocksize, total_packets) for x in range(blocks_needed)]
    starts.append(total_packets)
    return starts, [fec_percentage] * blocks_needed


def block_shards(starts, percentages):
    """(data_shards, parity_shards) per block."""
    out = []
    for i, pct in enumerate(percentages):
        data = starts[i + 1] - starts[i]
        out.append((data, (data * pct + 99) // 100))
    return out


def bernoulli(rng, n, rate):
    return {i for i in range(n) if rng.random() < rate}


def gilbert(rng, n, rate, mean_burst=5.0):
    """Two-state burst model with the requested average loss rate."""
    p_recover = 1.0 / mean_burst
    p_fail = rate * p_recover / max(1e-9, 1.0 - rate)
    lost = set()
    bad = False
    for i in range(n):
        if bad:
            lost.add(i)
            if rng.random() < p_recover:
                bad = False
        elif rng.random() < p_fail:
            bad = True
            lost.add(i)
    return lost


def delivered_packets(starts, shards, lost):
    """Data packets the client hands the decoder, following Phase 1: every fully
    recovered block from the front, then the contiguous prefix of the first block
    Reed-Solomon could not repair. Returns (packets, whole_frame)."""
    seq = 0
    for i, (data, parity) in enumerate(shards):
        block_lost = [s - seq for s in range(seq, seq + data + parity) if s in lost]
        if len(block_lost) <= parity:
            seq += data + parity
            continue
        first_lost_data = next(j for j in block_lost if j < data)
        return starts[i] + first_lost_data, False
    return starts[-1], True


def du_prefix_bytes(packets, payload_blocksize, du_size):
    return max(0, min(du_size, packets * payload_blocksize - FRAME_HEADER))


def prefix_chunks(chunks, du_bytes):
    """Chunks fully contained in the first du_bytes of the DU (the shim drops a
    chunk that the hole truncated)."""
    pos = 4
    kept = []
    for chunk in chunks:
        end = pos + 4 + len(chunk)
        if end > du_bytes:
            break
        kept.append(chunk)
        pos = end
    return kept


def count_blocks(chunks):
    """PyroWave blocks in these chunks; each chunk holds several self-delimiting
    packets and each packet is one 32x32 block."""
    n = 0
    for chunk in chunks:
        cp = 0
        while cp + 8 <= len(chunk):
            u16b = struct.unpack_from("<H", chunk, cp + 2)[0]
            if u16b & 0x8000:  # sequence header
                cp += 8
                continue
            n += 1
            cp += (u16b & 0x0FFF) * 4
    return n


def first_fine_header_end(chunks, coarse_block_end):
    """DU byte offset just past the header of the first packet whose block index
    is at or past the coarse boundary, or None if the frame has no such packet.

    Mirrors the client's partial-frame readiness rule (Decoder::DecodeIsReady):
    blocks are emitted in ascending index order and a delivered prefix is a byte
    prefix of the DU, so a prefix covering this offset has seen a packet start
    past the coarse levels — proof that every transmitted coarse block arrived.
    The header alone is enough (PushPacket peeks truncated packets)."""
    pos = 4  # [u32 packet_count]
    for chunk in chunks:
        cp = 0
        while cp + 8 <= len(chunk):
            u16b = struct.unpack_from("<H", chunk, cp + 2)[0]
            if u16b & 0x8000:  # sequence header
                cp += 8
                continue
            if struct.unpack_from("<I", chunk, cp + 4)[0] >> 8 >= coarse_block_end:
                return pos + 4 + cp + 8
            cp += (u16b & 0x0FFF) * 4
        pos += 4 + len(chunk)
    return None


def decode(chunks, tag):
    os.makedirs(SCRATCH, exist_ok=True)
    cap = os.path.join(SCRATCH, f"{tag}.cap")
    with open(cap, "wb") as f:
        f.write(struct.pack("<I", len(chunks)))
        for chunk in chunks:
            f.write(struct.pack("<I", len(chunk)) + chunk)
    out = os.path.join(SCRATCH, f"{tag}.pwtv")
    # --force-partial: upstream's own readiness rule is the ">half the blocks"
    # floor this work replaced, so prefixes the client would now show can sit
    # below it.
    r = subprocess.run([TOOL, "--force-partial", cap, out], env=ENV,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return out


def luma(path):
    d = open(path, "rb").read()
    _, _, w, h, _, bs = struct.unpack_from("<6I", d, 0)
    off = 24 + bs
    return np.frombuffer(d, dtype=np.uint8, count=w * h, offset=off).astype(np.int32)


def psnr(ref, test):
    se = np.sum((ref - test) ** 2)
    if se == 0:
        return float("inf")
    return 10 * math.log10(255 * 255 * ref.size / se)


def preview(path, out_path):
    """Third-resolution RGB preview, so the truncation can be judged by eye."""
    try:
        from PIL import Image
    except ImportError:
        return
    d = open(path, "rb").read()
    _, _, w, h, _, bs = struct.unpack_from("<6I", d, 0)
    off = 24 + bs
    p = [np.frombuffer(d, np.uint8, w * h, off + c * w * h).reshape(h, w).astype(np.float32)
         for c in range(3)]
    y = (p[0] - 16) * 255 / 219
    cb = (p[1] - 128) * 255 / 224
    cr = (p[2] - 128) * 255 / 224
    rgb = np.stack([y + 1.5748 * cr, y - 0.1873 * cb - 0.4681 * cr, y + 1.8556 * cb], -1)
    Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)) \
         .resize((w // 3, h // 3), Image.LANCZOS).save(out_path)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    write_previews = "--previews" in sys.argv
    capture = args[0] if args else \
        os.path.join(REPO, "tools", "testdata", "captures",
                     "pyrowave_3840x2160_444_10bit_hdr_f37767_rdr2.bin")
    packetsize = 1392
    fec_percentage = 20

    width, height, yuv444, total_blocks, chunks = parse_capture(capture)
    ends = compute_level_block_ends(width, height, yuv444)
    du_size, head_bytes = compute_head_bytes(chunks, ends)
    _, tier_offsets = level_offsets(chunks, ends, (4, 3, 2))

    blocksize = packetsize + MAX_RTP_HEADER_SIZE
    payload_blocksize = blocksize - VIDEO_PACKET_RAW
    total_packets = (du_size + FRAME_HEADER + payload_blocksize - 1) // payload_blocksize
    payload_size = total_packets * blocksize

    # The client's quality floor (Decoder::DecodeIsReady with the coarse-coverage
    # rule): a partial frame is shown iff the delivered prefix reaches a packet
    # starting past the coarse levels (which proves levels 4 and 3 are complete).
    coarse_block_end = ends[FEC_PROTECTED_THROUGH_LEVEL]
    floor_bytes = first_fine_header_end(chunks, coarse_block_end)
    if floor_bytes is None:
        floor_bytes = du_size + 1  # only a whole frame passes

    print(f"{os.path.basename(capture)}: {width}x{height} "
          f"{'4:4:4' if yuv444 else '4:2:0'}, {du_size} DU bytes, "
          f"{total_blocks} blocks, {total_packets} data packets @ packetsize {packetsize}")

    def head_tail():
        starts, pcts = split_fec_blocks_head_tail(
            payload_size, blocksize, payload_blocksize, head_bytes + FRAME_HEADER)
        if starts is None:
            return None
        return starts, pcts

    layouts = {
        # Sunshine's even split at a few percentages, to trace the
        # bandwidth/quality frontier the head/bare-tail layout has to beat.
        "even@20": split_evenly(payload_size, blocksize, 20),
        "even@15": split_evenly(payload_size, blocksize, 15),
        "even@10": split_evenly(payload_size, blocksize, 10),
        # The implemented layout: L4+L3 @ 50%, tail bare.
        "head50/0": head_tail(),
    }
    layouts = {k: v for k, v in layouts.items() if v is not None}

    for name, (starts, pcts) in layouts.items():
        shards = block_shards(starts, pcts)
        sent = sum(d + p for d, p in shards)
        geometry = "  ".join(f"{d}+{p}@{pct}%" for (d, p), pct in zip(shards, pcts))
        print(f"  {name:16s} {100.0 * sent / total_packets - 100:5.1f}% overhead   {geometry}")

    # --- Monte Carlo -------------------------------------------------------
    # Bucket each trial by how far into the frame the truncation landed. The
    # boundaries are the measured quality cliffs: keeping everything through level 2
    # is the "all of level 1 lost" case at 31.7 dB, while losing part of level 4 is
    # the ~10 dB case that is not worth putting on screen.
    print("\noutcome distribution, share of trials")
    print(f"  {'model':18s} {'layout':16s} {'whole':>7s} {'truncated':>10s} "
          f"{'dropped':>8s} {'median kept':>12s}")
    for model_name, mean_burst in (("independent", 0), ("burst(5)", 5.0), ("burst(20)", 20.0)):
        for rate in (0.02, 0.05, 0.10):
            for name, (starts, pcts) in layouts.items():
                shards = block_shards(starts, pcts)
                sent = sum(d + p for d, p in shards)
                rng = random.Random(0xC0FFEE)

                whole = truncated = dropped = 0
                truncated_fracs = []
                for _ in range(TRIALS):
                    lost = (bernoulli(rng, sent, rate) if mean_burst == 0
                            else gilbert(rng, sent, rate, mean_burst))
                    packets, full = delivered_packets(starts, shards, lost)
                    if full:
                        whole += 1
                        continue

                    du_bytes = du_prefix_bytes(packets, payload_blocksize, du_size)

                    # The client's quality floor: Decoder::DecodeIsReady() shows a
                    # partial frame iff the coarse levels (4 and 3) fully decoded.
                    if du_bytes < floor_bytes:
                        dropped += 1
                    else:
                        truncated += 1
                        truncated_fracs.append(du_bytes / du_size)

                truncated_fracs.sort()
                median = (f"{100.0 * truncated_fracs[len(truncated_fracs) // 2]:11.1f}%"
                          if truncated_fracs else f"{'-':>12s}")
                label = f"{model_name} {rate * 100:g}%"
                print(f"  {label:18s} {name:16s} {100.0 * whole / TRIALS:6.1f}% "
                      f"{100.0 * truncated / TRIALS:9.1f}% {100.0 * dropped / TRIALS:7.1f}% "
                      f"{median}")
            print()

    # --- What truncation actually costs -------------------------------------
    # The bucket boundaries above are byte offsets; this is what they look like.
    # Note PSNR is measured against the lossless decode of the same frame, and it
    # punishes a soft reconstruction hard - inspect the previews before reading too
    # much into a number. A synthetic test pattern is close to the worst case here,
    # since fine gratings and dither put most of the bytes in the finest levels.
    ref_path = decode(chunks, "baseline")
    if ref_path is None:
        print("baseline decode failed; skipping the truncation curve")
        return 1
    ref = luma(ref_path)

    print("cost of truncating the frame at a given point")
    print(f"  {'cut at':16s} {'kept':>7s} {'blocks':>14s} {'Y PSNR':>9s}")
    cuts = [("end of L4", tier_offsets[0]), ("end of L3", tier_offsets[1]),
            ("end of L2", tier_offsets[2])]
    cuts += [(f"{pct}% of frame", du_size * pct // 100) for pct in (50, 60, 75, 90, 95)]
    for name, cut in cuts:
        kept = prefix_chunks(chunks, cut)
        blocks = count_blocks(kept)
        if cut < floor_bytes:
            print(f"  {name:16s} {100.0 * cut / du_size:6.1f}% {blocks:6d}/{total_blocks:6d} "
                  f"{'dropped':>9s}  (coarse levels incomplete)")
            continue
        out = decode(kept, f"cut{cut}")
        if out is None:
            print(f"  {name:16s} decode failed")
            continue
        if write_previews:
            preview(out, os.path.join(SCRATCH, f"cut{cut}.png"))
        print(f"  {name:16s} {100.0 * cut / du_size:6.1f}% {blocks:6d}/{total_blocks:6d} "
              f"{psnr(ref, luma(out)):8.2f}dB")
    if write_previews:
        preview(ref_path, os.path.join(SCRATCH, "whole.png"))
        print(f"\n  previews written to {SCRATCH}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
