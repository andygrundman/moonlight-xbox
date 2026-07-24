#!/usr/bin/env python3
"""Validate the head/bare-tail FEC split (host side) against real captures.

Mirrors the implemented C++ exactly:

  * compute_level_block_ends() / compute_head_bytes() mirror
    Sunshine-work/src/platform/windows/pyrowave_encode.cpp - the level->block-index
    map (cross-checked below against the independently written band_ranges() from
    pyrowave_loss_sim.py) and the boundary walk in encoder_t::encode().
  * split_fec_blocks_head_tail() mirrors stream.cpp - FEC block 0 covers the
    sequence header + wavelet levels 4 and 3 at FEC_HEAD_PERCENTAGE parity, the
    tail rides bare at 0%.

The rescale-to-configured-percentage design this replaced (split_fec_blocks_by_tier)
was measured as a regression on real captures and lives in commit ff9bbec; the
measurements are recorded in docs/pyrowave-partial-du-design.md.

  python3 tools/pyrowave_tier_sim.py
"""
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTDATA = os.path.join(REPO, "tools", "testdata")

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print(f"  FAIL: {what}")
    return cond


# ---------------------------------------------------------------------------
# Mirror of pyrowave_encode.cpp
# ---------------------------------------------------------------------------

DECOMPOSITION_LEVELS = 5
NUM_COMPONENTS = 3
BANDS_PER_LEVEL = 4
WAVELET_ALIGNMENT = 1 << DECOMPOSITION_LEVELS
MIN_IMAGE_SIZE = 4 << DECOMPOSITION_LEVELS

FEC_PROTECTED_THROUGH_LEVEL = 3


def compute_level_block_ends(width, height, yuv444):
    def align_up(v):
        v = ((v + WAVELET_ALIGNMENT - 1) // WAVELET_ALIGNMENT) * WAVELET_ALIGNMENT
        return max(v, MIN_IMAGE_SIZE)

    aligned_width = align_up(width)
    aligned_height = align_up(height)

    ends = [0] * DECOMPOSITION_LEVELS
    blocks = 0
    for level in range(DECOMPOSITION_LEVELS - 1, -1, -1):
        level_width = (aligned_width // 2) >> level
        level_height = (aligned_height // 2) >> level
        blocks_x_32 = (level_width + 31) // 32
        blocks_y_32 = (((level_height + 7) // 8) + 3) // 4
        for component in range(NUM_COMPONENTS):
            if level == 0 and component != 0 and not yuv444:
                continue
            first_band = 0 if level == DECOMPOSITION_LEVELS - 1 else 1
            blocks += (BANDS_PER_LEVEL - first_band) * blocks_x_32 * blocks_y_32
        ends[level] = blocks
    return ends


def first_block_index(chunk):
    """Trailing u32 of the 8-byte BitstreamHeader is {quant_code:8, block_index:24}."""
    return struct.unpack_from("<I", chunk, 4)[0] >> 8


def compute_head_bytes(chunks, level_block_ends):
    """Mirror of the head-boundary walk in encoder_t::encode(): the DU byte offset
    at which the first packet *starts* past the last protected level. Also returns
    the DU size. 0 head_bytes means the frame never left the head (no split)."""
    head_block_end = level_block_ends[FEC_PROTECTED_THROUGH_LEVEL]
    size = 4  # [u32 packet_count]
    head_bytes = 0
    for i, chunk in enumerate(chunks):
        if head_bytes == 0 and i > 0 and first_block_index(chunk) >= head_block_end:
            head_bytes = size
        size += 4 + len(chunk)
    return size, head_bytes


# ---------------------------------------------------------------------------
# Mirror of stream.cpp
# ---------------------------------------------------------------------------

MAX_FEC_BLOCKS = 4
DATA_SHARDS_MAX = 255
FEC_HEAD_PERCENTAGE = 50


def split_fec_blocks_head_tail(payload_size, blocksize, payload_blocksize,
                               head_stream_bytes):
    """Returns (starts, percentages) where starts are packet indices with a final
    total-packets sentinel, or (None, None) to fall back to the even split.
    head_stream_bytes counts the frame header + protected DU bytes, matching the
    `packet->fec_head_bytes + sizeof(frame_header)` at the call site."""
    total_packets = (payload_size + blocksize - 1) // blocksize
    head_packets = (head_stream_bytes + payload_blocksize - 1) // payload_blocksize
    if head_packets == 0 or head_packets >= total_packets:
        return None, None

    if head_packets > (DATA_SHARDS_MAX * 100) // (100 + FEC_HEAD_PERCENTAGE):
        return None, None

    tail_packets = total_packets - head_packets
    tail_blocks = (tail_packets + DATA_SHARDS_MAX - 1) // DATA_SHARDS_MAX
    if 1 + tail_blocks > MAX_FEC_BLOCKS:
        return None, None

    starts = [0, head_packets]
    percentages = [FEC_HEAD_PERCENTAGE]
    tail_base, tail_extra = divmod(tail_packets, tail_blocks)
    for x in range(tail_blocks):
        block_packets = tail_base + (1 if x < tail_extra else 0)
        starts.append(starts[-1] + block_packets)
        percentages.append(0)
    return starts, percentages


# ---------------------------------------------------------------------------
# Reference: band_ranges() copied from pyrowave_loss_sim.py
# ---------------------------------------------------------------------------

def band_ranges(w, h, chroma444=True):
    aw = max((w + 31) & ~31, 128)
    ah = max((h + 31) & ~31, 128)
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


def test_level_block_ends():
    print("level->block map vs pyrowave_loss_sim.band_ranges()")
    cases = [(1920, 1080), (2560, 1440), (3840, 2160), (1280, 720),
             (1366, 768), (100, 100), (720, 480)]
    for w, h in cases:
        for yuv444 in (True, False):
            ends = compute_level_block_ends(w, h, yuv444)
            ranges = band_ranges(w, h, yuv444)
            # The end of a level is the highest band end seen at that level.
            for level in range(DECOMPOSITION_LEVELS):
                want = max(e for (lv, _, _), (_, e) in ranges.items() if lv >= level)
                check(ends[level] == want,
                      f"{w}x{h} {'444' if yuv444 else '420'} level {level}: "
                      f"got {ends[level]}, want {want}")
            check(ends[0] == max(e for _, e in ranges.values()),
                  f"{w}x{h}: total block count")
    print(f"  checked {len(cases) * 2} configurations")


# ---------------------------------------------------------------------------
# Real captures
# ---------------------------------------------------------------------------

def parse_capture(path):
    """Returns (width, height, yuv444, total_blocks, chunks) where chunks are raw
    byte strings of the [u32 size][bytes] framing produced by pyrowave_encode.cpp."""
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 0)[0]
    chunks = []
    pos = 4
    for _ in range(count):
        sz = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        chunks.append(data[pos:pos + sz])
        pos += sz

    # BitstreamSequenceHeader: width_minus_1:14, height_minus_1:14, sequence:3, extended:1 /
    # total_blocks:24, code:2, chroma_resolution:1, ...
    hdr0, hdr1 = struct.unpack_from("<II", chunks[0], 0)
    width = (hdr0 & 0x3FFF) + 1
    height = ((hdr0 >> 14) & 0x3FFF) + 1
    total_blocks = hdr1 & 0xFFFFFF
    yuv444 = bool((hdr1 >> 26) & 1)
    return width, height, yuv444, total_blocks, chunks


def level_offsets(chunks, level_block_ends, levels=(4, 3, 2)):
    """DU byte offsets where each of the given levels ends (packet-start rule),
    for sizing reports and pyrowave_fec_sim.py's truncation-cost table."""
    offsets = {}
    size = 4
    for i, chunk in enumerate(chunks):
        if i > 0:
            for level in levels:
                if level not in offsets and first_block_index(chunk) >= level_block_ends[level]:
                    offsets[level] = size
        size += 4 + len(chunk)
    return size, [offsets[lv] for lv in levels if lv in offsets]


def report(path, packetsize):
    width, height, yuv444, _, chunks = parse_capture(path)
    ends = compute_level_block_ends(width, height, yuv444)
    du_size, head_bytes = compute_head_bytes(chunks, ends)

    print(f"\n{os.path.basename(path)}")
    print(f"  {width}x{height} {'4:4:4' if yuv444 else '4:2:0'}, "
          f"{len(chunks)} chunks, {du_size} DU bytes")
    print(f"  level block ends L4..L0: {ends[4]} {ends[3]} {ends[2]} {ends[1]} {ends[0]}")

    check(0 < head_bytes < du_size, "head boundary inside the frame")
    # The head walk is the level_offsets() walk restricted to one boundary.
    du_size2, offs = level_offsets(chunks, ends, (FEC_PROTECTED_THROUGH_LEVEL,))
    check(du_size2 == du_size and offs == [head_bytes],
          "compute_head_bytes() agrees with level_offsets()")
    print(f"  head (seq header + L4 + L3): {head_bytes} bytes "
          f"({100.0 * head_bytes / du_size:.1f}% of frame)")

    # Match videoBroadcastThread(): 8-byte frame header, one packet header per
    # payload_blocksize bytes of frame.
    NV_VIDEO_PACKET = 16
    MAX_RTP_HEADER_SIZE = 16
    FRAME_HEADER = 8
    blocksize = packetsize + MAX_RTP_HEADER_SIZE
    payload_blocksize = blocksize - (NV_VIDEO_PACKET + 12 + 4)  # sizeof(video_packet_raw_t)
    strided = du_size + FRAME_HEADER
    total_packets = (strided + payload_blocksize - 1) // payload_blocksize
    payload_size = total_packets * blocksize

    starts, pcts = split_fec_blocks_head_tail(payload_size, blocksize,
                                              payload_blocksize,
                                              head_bytes + FRAME_HEADER)
    print(f"  packetsize {packetsize} -> {total_packets} data packets")

    if starts is None:
        # The only legitimate reasons to decline at these capture sizes.
        head_packets = (head_bytes + FRAME_HEADER + payload_blocksize - 1) // payload_blocksize
        check(head_packets >= total_packets or
              head_packets > (DATA_SHARDS_MAX * 100) // (100 + FEC_HEAD_PERCENTAGE),
              "declined for a reason the fallback handles")
        print("  declined (falls back to the even split)")
        return

    # MIN_PARITY: x-nv-vqos[0].fec.minRequiredFecPackets, from SdpGenerator.c.
    # fec::encode() must not rewrite our percentage, or the parity count the
    # client derives from the header stops matching the shards actually sent.
    MIN_PARITY = 2
    check(starts[0] == 0 and starts[-1] == total_packets, "blocks cover the payload")
    check(len(pcts) <= MAX_FEC_BLOCKS, "block count within protocol limit")
    check(pcts[0] == FEC_HEAD_PERCENTAGE and all(p == 0 for p in pcts[1:]),
          "head protected, tail bare")

    head_end_packet = (head_bytes + FRAME_HEADER + payload_blocksize - 1) // payload_blocksize
    check(starts[1] == head_end_packet, "block 0 covers exactly the protected prefix")

    parity_total = 0
    for i, pct in enumerate(pcts):
        data_shards = starts[i + 1] - starts[i]
        parity = (data_shards * pct + 99) // 100
        parity_total += parity
        check(data_shards >= 1, f"block {i} has at least one data packet")
        check(data_shards + parity <= DATA_SHARDS_MAX, f"block {i} fits Reed-Solomon")
        check(pct == 0 or parity >= MIN_PARITY,
              f"block {i} meets the parity minimum without being rewritten")
        print(f"  block {i}: {data_shards:4d} data + {parity:3d} parity @ {pct:3d}%")

    overhead = 100.0 * parity_total / total_packets
    print(f"  parity overhead {overhead:.1f}% (uniform 20% was the old default)")


def main():
    test_level_block_ends()

    paths = [os.path.join(TESTDATA, f) for f in sorted(os.listdir(TESTDATA))
             if f.endswith(".bin")]
    cap_dir = os.path.join(TESTDATA, "captures")
    if os.path.isdir(cap_dir):
        paths += [os.path.join(cap_dir, f) for f in sorted(os.listdir(cap_dir))
                  if f.endswith(".bin")]
    for path in paths:
        for packetsize in (1024, 1392):
            report(path, packetsize)

    print()
    if failures:
        print(f"FAILED ({failures} checks)")
        return 1
    print("PASSED (0 failures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
