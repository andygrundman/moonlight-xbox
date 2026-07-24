// Turns a captured PyroWave decode-unit byte stream (one frame's packets,
// e.g. dumped from a working Moonlight client fed by Sunshine) into a PWTV
// golden test vector by decoding it with the reference decoder.
//
// Width/height/chroma are auto-detected from the START_OF_FRAME sequence
// header in the stream; no encoder is needed (the reference encoder does not
// initialize under MoltenVK, but the decoder does).
//
//   PWTV file: magic 'PWTV', version, width, height, chroma444,
//              bitstream size, bitstream bytes, 3x golden u8 planes (W*H).
//
// Build (macOS, against Andy's pyrowave build):
//   clang++ -std=c++17 -O2 -I third_party/pyrowave -I /opt/homebrew/include \
//     tools/pyrowave_dump_golden.cpp third_party/pyrowave/build/libpyrowave-shared.dylib \
//     -Wl,-rpath,$PWD/third_party/pyrowave/build -o build_tools/pyrowave_dump_golden
// Run from repo root (the committed capture lives in tools/testdata/;
// Assets/pyrowave_testvec.bin is generated and gitignored):
//   DYLD_LIBRARY_PATH=/opt/homebrew/lib \
//   VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
//   PYROWAVE_PRECISION=2 \
//   build_tools/pyrowave_dump_golden tools/testdata/pyrowave_frame_2560x1440_444.bin Assets/pyrowave_testvec.bin
//
// PYROWAVE_PRECISION=2 (all-R32F storage) is deliberate even though the D3D11
// port stores bands as R16F: on devices with shaderFloat16 (Apple M1) the
// reference's precision-0/1 idwt programs compute in explicit float16_t
// arithmetic, which carries a systematic ~-0.4 8-bit-code luma bias vs fp32.
// SM 5.0 has no fp16, so the D3D11 port computes in fp32 and matches the
// precision-2 (fp32-arithmetic) decode within <0.9 codes; against a
// precision-0 golden it would spuriously fail the ±1 criterion (measured on
// Series X: luma mean |d| 0.48, max 2.6 vs P0; max 0.87 vs P2).

#include <vulkan/vulkan.h>
#include "pyrowave.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#define CHECKED(x) do { \
	pyrowave_result res_ = (x); \
	if (res_ != PYROWAVE_SUCCESS) { fprintf(stderr, "FATAL: %s -> %d (line %d)\n", #x, res_, __LINE__); return 1; } \
} while (0)

// Mirrors pyrowave_common.hpp (which drags in Granite, so redefine locally).
struct BitstreamHeader {
	uint16_t ballot;
	uint16_t payload_words : 12;
	uint16_t sequence : 3;
	uint16_t extended : 1;
	uint32_t quant_code : 8;
	uint32_t block_index : 24;
};
struct BitstreamSequenceHeader {
	uint32_t width_minus_1 : 14;
	uint32_t height_minus_1 : 14;
	uint32_t sequence : 3;
	uint32_t extended : 1;
	uint32_t total_blocks : 24;
	uint32_t code : 2;
	uint32_t chroma_resolution : 1;
	uint32_t color_primaries : 1;
	uint32_t transfer_function : 1;
	uint32_t ycbcr_transform : 1;
	uint32_t ycbcr_range : 1;
	uint32_t chroma_siting : 1;
};
static_assert(sizeof(BitstreamHeader) == 8 && sizeof(BitstreamSequenceHeader) == 8, "header size");

// --verbose: walk the packet stream again and break the payload bytes down
// by wavelet band. Block indices are assigned by the encoder in a fixed
// iteration order (level coarse->fine, component, band; LL exists only at
// the coarsest level; level-0 chroma is skipped in 4:2:0) — this mirrors
// WaveletBuffers::init_block_meta / D3D11Decoder::InitBlockMeta exactly, so
// each packet's block_index maps to one (level, component, band).
static void print_verbose_structure(const std::vector<uint8_t> &stream, size_t size,
                                    int width, int height, int chroma444) {
	constexpr int kLevels = 5;
	const char *bandNames[4] = { "LL", "HL", "LH", "HH" };
	const char *compNames[3] = { "Y ", "Cb", "Cr" };

	int alignedW = (width + 31) & ~31, alignedH = (height + 31) & ~31;
	if (alignedW < 128) alignedW = 128;
	if (alignedH < 128) alignedH = 128;

	struct BandInfo {
		int level, comp, band;
		int w, h;            // band dimensions in coefficients
		int firstBlock, numBlocks;
		size_t bytes = 0;
		int packets = 0, blocksPresent = 0;
	};
	std::vector<BandInfo> bands;
	int blockCount = 0;
	for (int level = kLevels - 1; level >= 0; level--) {
		for (int comp = 0; comp < 3; comp++) {
			if (level == 0 && comp != 0 && !chroma444)
				continue;
			for (int band = (level == kLevels - 1 ? 0 : 1); band < 4; band++) {
				BandInfo bi;
				bi.level = level;
				bi.comp = comp;
				bi.band = band;
				bi.w = (alignedW / 2) >> level;
				bi.h = (alignedH / 2) >> level;
				int blocksX32 = (bi.w + 31) / 32;
				int blocksY8 = (bi.h + 7) / 8;
				int blocksY32 = (blocksY8 + 3) / 4;
				bi.firstBlock = blockCount;
				bi.numBlocks = blocksX32 * blocksY32;
				blockCount += bi.numBlocks;
				bands.push_back(bi);
			}
		}
	}

	std::vector<bool> blockSeen(blockCount, false);
	size_t seqBytes = 0, unattributed = 0;
	size_t pos = 0;
	while (pos + sizeof(BitstreamHeader) <= size) {
		BitstreamHeader hdr;
		memcpy(&hdr, stream.data() + pos, sizeof(hdr));
		if (hdr.extended) {
			seqBytes += sizeof(BitstreamSequenceHeader);
			pos += sizeof(BitstreamSequenceHeader);
			continue;
		}
		size_t packetSize = size_t(hdr.payload_words) * sizeof(uint32_t);
		int blockIndex = int(hdr.block_index);
		bool attributed = false;
		for (auto &bi : bands) {
			if (blockIndex >= bi.firstBlock && blockIndex < bi.firstBlock + bi.numBlocks) {
				bi.bytes += packetSize;
				bi.packets++;
				if (!blockSeen[blockIndex]) {
					blockSeen[blockIndex] = true;
					bi.blocksPresent++;
				}
				attributed = true;
				break;
			}
		}
		if (!attributed)
			unattributed += packetSize;
		pos += packetSize;
	}

	printf("\n=== Bitstream structure (%dx%d aligned %dx%d, %s, %d coded 32x32 blocks) ===\n",
	       width, height, alignedW, alignedH, chroma444 ? "4:4:4" : "4:2:0", blockCount);
	printf("Coefficient pyramid: 5 decomposition levels; each band holds one quadrant of\n");
	printf("its level. LL exists only at level 4 (coarser levels' LL are reconstructed by\n");
	printf("the iDWT). Bytes include each packet's 8-byte header; blocks are 32x32\n");
	printf("coefficient groups (one packet per block unless dropped).\n\n");
	printf("level comp band     dims        blocks       bytes    share  avg B/block\n");

	size_t total = 0;
	for (auto &bi : bands)
		total += bi.bytes;

	int lastLevel = -1;
	size_t levelBytes = 0;
	auto flushLevel = [&](int level) {
		if (lastLevel >= 0)
			printf("  -- level %d subtotal: %zu bytes (%.1f%%)\n\n",
			       lastLevel, levelBytes, total ? 100.0 * levelBytes / total : 0.0);
		levelBytes = 0;
		lastLevel = level;
	};
	for (auto &bi : bands) {
		if (bi.level != lastLevel)
			flushLevel(bi.level);
		levelBytes += bi.bytes;
		printf("  L%d   %s   %s   %5dx%-5d  %4d/%-4d  %9zu   %5.1f%%   %8.0f\n",
		       bi.level, compNames[bi.comp], bandNames[bi.band], bi.w, bi.h,
		       bi.blocksPresent, bi.numBlocks, bi.bytes,
		       total ? 100.0 * bi.bytes / total : 0.0,
		       bi.blocksPresent ? double(bi.bytes) / bi.blocksPresent : 0.0);
	}
	flushLevel(-1);

	size_t compBytes[3] = {};
	for (auto &bi : bands)
		compBytes[bi.comp] += bi.bytes;
	printf("  Per component: Y %zu (%.1f%%), Cb %zu (%.1f%%), Cr %zu (%.1f%%)\n",
	       compBytes[0], total ? 100.0 * compBytes[0] / total : 0.0,
	       compBytes[1], total ? 100.0 * compBytes[1] / total : 0.0,
	       compBytes[2], total ? 100.0 * compBytes[2] / total : 0.0);
	printf("  Band payload %zu + sequence headers %zu = %zu bytes total\n",
	       total, seqBytes, total + seqBytes);
	if (unattributed)
		printf("  WARNING: %zu bytes in packets with out-of-range block indices\n", unattributed);
	printf("\n");
}

int main(int argc, char **argv) {
	bool verbose = false;
	bool allow_partial = false;
	bool force_partial = false;
	std::vector<const char *> paths;
	for (int i = 1; i < argc; i++) {
		if (std::string(argv[i]) == "--verbose" || std::string(argv[i]) == "-v")
			verbose = true;
		else if (std::string(argv[i]) == "--allow-partial")
			allow_partial = true; // packet-loss experiments; never for goldens
		else if (std::string(argv[i]) == "--force-partial")
			// Decode below decode_is_ready()'s "more than half the blocks" floor.
			// That floor is a guess at where a partial frame stops being worth
			// showing; measuring what lies under it is how you check the guess.
			force_partial = allow_partial = true;
		else
			paths.push_back(argv[i]);
	}
	if (paths.size() < 2) {
		fprintf(stderr, "usage: %s [--verbose] [--allow-partial] [--force-partial] <captured_du_stream.bin> <out_testvec.bin>\n", argv[0]);
		return 1;
	}
	const char *in_path = paths[0];
	const char *out_path = paths[1];

	FILE *in = fopen(in_path, "rb");
	if (!in) { fprintf(stderr, "FATAL: cannot read %s\n", in_path); return 1; }
	fseek(in, 0, SEEK_END);
	long in_size = ftell(in);
	fseek(in, 0, SEEK_SET);
	std::vector<uint8_t> stream(in_size);
	if (fread(stream.data(), 1, stream.size(), in) != stream.size()) {
		fprintf(stderr, "FATAL: short read on %s\n", in_path);
		return 1;
	}
	fclose(in);

	// The host frames each decode unit as [u32 count]{[u32 size][bytes]}*.
	// Strip that transport framing to recover the raw self-delimiting packet
	// stream, which is what D3D11Decoder::PushPacket (and this reference
	// decoder) consume.
	if (stream.size() < 4) { fprintf(stderr, "FATAL: capture too small\n"); return 1; }
	uint32_t chunk_count;
	memcpy(&chunk_count, stream.data(), 4);
	std::vector<uint8_t> raw;
	raw.reserve(stream.size());
	{
		size_t fpos = 4;
		uint32_t chunks = 0;
		for (; chunks < chunk_count && fpos + 4 <= stream.size(); chunks++) {
			uint32_t sz;
			memcpy(&sz, stream.data() + fpos, 4);
			fpos += 4;
			if (fpos + sz > stream.size()) {
				if (allow_partial) {
					// A partial capture (the client's automatic partial-frame
					// saves) stops at an arbitrary byte. Whole packets inside
					// the truncated chunk still decode; the packet walk below
					// stops cleanly at the cut.
					fprintf(stderr, "NOTE: chunk %u (size %u) truncated at offset %zu — partial capture, keeping %zu bytes of it\n",
					        chunks, sz, fpos, stream.size() - fpos);
					raw.insert(raw.end(), stream.begin() + fpos, stream.end());
					fpos = stream.size();
					chunks++;
					break;
				}
				fprintf(stderr, "FATAL: chunk %u (size %u) overruns file at offset %zu — truncated capture (pass --allow-partial if intended)\n",
				        chunks, sz, fpos);
				return 1;
			}
			raw.insert(raw.end(), stream.begin() + fpos, stream.begin() + fpos + sz);
			fpos += sz;
		}
		if (chunks != chunk_count || fpos != stream.size()) {
			// The [u32 count] prefix over-counts for a partial DU (it is the
			// host's count for the whole frame).
			if (!allow_partial) {
				fprintf(stderr, "FATAL: framing walk ended at chunk %u/%u, offset %zu/%zu\n",
				        chunks, chunk_count, fpos, stream.size());
				return 1;
			}
			fprintf(stderr, "NOTE: partial capture holds %u of %u framed chunks\n", chunks, chunk_count);
		}
		printf("Stripped DU framing: %u chunks, %zu raw packet-stream bytes\n", chunks, raw.size());
	}
	stream = std::move(raw);

	// Walk the self-delimiting packet stream: find dimensions/chroma and
	// sanity-check structure before handing it to the decoder.
	int width = 0, height = 0, chroma444 = -1;
	size_t pos = 0, data_packets = 0, seq_packets = 0;
	while (pos + sizeof(BitstreamHeader) <= stream.size()) {
		BitstreamHeader hdr;
		memcpy(&hdr, stream.data() + pos, sizeof(hdr));
		if (hdr.extended) {
			BitstreamSequenceHeader seq;
			memcpy(&seq, stream.data() + pos, sizeof(seq));
			if (seq.code == 0) { // BITSTREAM_EXTENDED_CODE_START_OF_FRAME
				width = seq.width_minus_1 + 1;
				height = seq.height_minus_1 + 1;
				chroma444 = int(seq.chroma_resolution); // 1 == 4:4:4
				printf("Sequence header: %dx%d chroma=%s seq=%u total_blocks=%u\n",
				       width, height, chroma444 ? "444" : "420",
				       unsigned(seq.sequence), unsigned(seq.total_blocks));
			}
			seq_packets++;
			pos += sizeof(BitstreamSequenceHeader);
		} else {
			size_t packet_size = size_t(hdr.payload_words) * sizeof(uint32_t);
			if (packet_size < sizeof(BitstreamHeader) || pos + packet_size > stream.size()) {
				if (allow_partial && packet_size >= sizeof(BitstreamHeader)) {
					// The cut fell mid-packet; everything before it is whole
					// (only the first `pos` bytes are pushed below).
					fprintf(stderr, "NOTE: final packet truncated at offset %zu (%zu of %zu bytes) — decoding the whole packets before it\n",
					        pos, stream.size() - pos, packet_size);
					break;
				}
				fprintf(stderr, "FATAL: malformed packet at offset %zu (payload_words=%u, %zu bytes left) — capture is truncated or not a PyroWave stream\n",
				        pos, unsigned(hdr.payload_words), stream.size() - pos);
				return 1;
			}
			data_packets++;
			pos += packet_size;
		}
	}
	if (pos != stream.size())
		fprintf(stderr, "WARNING: %zu trailing bytes after last whole packet\n", stream.size() - pos);
	printf("Parsed %zu bytes: %zu sequence + %zu data packets\n", pos, seq_packets, data_packets);

	if (width == 0 || chroma444 < 0) {
		fprintf(stderr, "FATAL: no START_OF_FRAME sequence header found — capture must include the whole decode unit from its first byte\n");
		return 1;
	}

	if (verbose)
		print_verbose_structure(stream, pos, width, height, chroma444);

	pyrowave_device device;
	CHECKED(pyrowave_create_default_device(&device));

	pyrowave_decoder_create_info dec_info = {};
	dec_info.device = device;
	dec_info.width = width;
	dec_info.height = height;
	dec_info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
	pyrowave_decoder decoder;
	CHECKED(pyrowave_decoder_create(&dec_info, &decoder));

	// push_packet parses the whole self-delimiting buffer internally.
	CHECKED(pyrowave_decoder_push_packet(decoder, stream.data(), pos));

	if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
		if (allow_partial && pyrowave_decoder_decode_is_ready(decoder, true)) {
			fprintf(stderr, "WARNING: PARTIAL frame decoded (--allow-partial); missing blocks are zero. Do NOT use as a golden vector.\n");
		} else if (pyrowave_decoder_decode_is_ready(decoder, true)) {
			fprintf(stderr, "FATAL: capture is a PARTIAL frame (over half arrived, but not all). Recapture — a golden vector must be complete (or pass --allow-partial for loss experiments).\n");
			return 1;
		} else if (force_partial) {
			fprintf(stderr, "WARNING: decoding below the half-block floor (--force-partial). Do NOT use as a golden vector.\n");
		} else {
			fprintf(stderr, "FATAL: decoder not ready; capture does not contain a usable frame (less than half its blocks)\n");
			return 1;
		}
	}

	// 4:2:0 chroma planes are quarter size (w/2 x h/2)
	std::vector<uint8_t> golden[3];
	pyrowave_cpu_buffer output = {};
	for (int c = 0; c < 3; c++) {
		int pw = (c != 0 && !chroma444) ? width / 2 : width;
		int ph = (c != 0 && !chroma444) ? height / 2 : height;
		golden[c].resize(size_t(pw) * ph);
		output.data[c] = golden[c].data();
		output.row_stride_in_bytes[c] = pw;
		output.plane_size_in_bytes[c] = golden[c].size();
	}
	output.width = width;
	output.height = height;
	output.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
	CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &output));

	// Plane checksums for quick eyeball comparison across runs.
	for (int c = 0; c < 3; c++) {
		uint64_t sum = 0;
		for (uint8_t v : golden[c]) sum = sum * 131 + v;
		printf("Plane %d checksum: %016llx\n", c, (unsigned long long)sum);
	}

	FILE *f = fopen(out_path, "wb");
	if (!f) { fprintf(stderr, "FATAL: cannot write %s\n", out_path); return 1; }
	uint32_t magic = 0x56545750; // 'PWTV' little-endian
	uint32_t version = 1, chroma_flag = chroma444 ? 1 : 0;
	uint32_t w = width, h = height, bs_size = (uint32_t)pos;
	fwrite(&magic, 4, 1, f);
	fwrite(&version, 4, 1, f);
	fwrite(&w, 4, 1, f);
	fwrite(&h, 4, 1, f);
	fwrite(&chroma_flag, 4, 1, f);
	fwrite(&bs_size, 4, 1, f);
	fwrite(stream.data(), 1, pos, f);
	for (int c = 0; c < 3; c++)
		fwrite(golden[c].data(), 1, golden[c].size(), f);
	fclose(f);

	printf("Wrote %s (%u-byte bitstream + %s golden planes, %dx%d)\n",
	       out_path, bs_size, chroma444 ? "3x full-res" : "Y + 2x quarter-res", width, height);

	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(device);
	return 0;
}
