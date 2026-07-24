#include "pch.h"
#include "D3D11Decoder.h"
#include "..\..\Common\DirectXHelper.h"
#include <Utils.hpp>

using Microsoft::WRL::ComPtr;

namespace moonlight_xbox_dx {
namespace PyroWaveD3D11 {

namespace {

	// Matches cbuffer Registers in dequant_sm50.hlsl (b0, 32 bytes)
	struct DequantConstants {
		int32_t resolution[2];
		int32_t outputLayer;
		int32_t blockOffset32x32;
		int32_t blockStride32x32;
		int32_t padding[3];
	};

	// Matches cbuffer Registers in idwt_sm50.hlsl (b0, 16 bytes)
	struct IdwtConstants {
		int32_t resolution[2];
		float invResolution[2];
	};

	int AlignUp(int value, int align) {
		return (value + align - 1) & ~(align - 1);
	}

	enum { kStartOfFrame = 0 }; // BITSTREAM_EXTENDED_CODE_START_OF_FRAME

} // namespace

bool Decoder::Init(ID3D11Device *device, int width, int height, bool chroma444) {
	m_device = device;
	m_width = width;
	m_height = height;
	m_chroma444 = chroma444;

	m_alignedWidth = AlignUp(width, kAlignment);
	m_alignedHeight = AlignUp(height, kAlignment);
	if (m_alignedWidth < kMinimumImageSize)
		m_alignedWidth = kMinimumImageSize;
	if (m_alignedHeight < kMinimumImageSize)
		m_alignedHeight = kMinimumImageSize;

	try {
		auto loadShader = [&](const wchar_t *path, ComPtr<ID3D11ComputeShader> &shader) {
			auto bytecode = DX::ReadData(path);
			DX::ThrowIfFailed(
			    device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader),
			    "PyroWave CS creation");
		};
		loadShader(L"Assets\\Shader\\dequant_sm50.fxc", m_dequantShader);
		loadShader(L"Assets\\Shader\\idwt_sm50.fxc", m_idwtShader);
		loadShader(L"Assets\\Shader\\idwt_dc_sm50.fxc", m_idwtDcShader);

		InitBlockMeta();
		if (!CreateResources(device))
			return false;
	}
	catch (Platform::Exception ^ e) {
		Utils::Logf("PyroWave: Init failed with 0x%08X (missing .fxc assets?)\n", e->HResult);
		return false;
	}
	catch (...) {
		Utils::Log("PyroWave: Init failed with unknown exception\n");
		return false;
	}

	m_offsetsCpu.resize(m_blockCount32x32);
	m_payloadCpu.reserve(1024 * 1024);
	Clear();

	Utils::Logf("PyroWave: decoder init %dx%d (aligned %dx%d), %s, %d blocks\n",
	    width, height, m_alignedWidth, m_alignedHeight,
	    chroma444 ? "4:4:4" : "4:2:0", m_blockCount32x32);
	return true;
}

// Port of WaveletBuffers::init_block_meta (pyrowave_common.cpp). Assigns
// global 8x8/32x32 block indices; iteration order must match the encoder
// exactly: level coarse->fine, component, band (LL only at coarsest level).
void Decoder::InitBlockMeta() {
	m_blockCount8x8 = 0;
	m_blockCount32x32 = 0;

	for (int level = kLevels - 1; level >= 0; level--) {
		for (int component = 0; component < kComponents; component++) {
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0 && !m_chroma444)
				continue;

			for (int band = (level == kLevels - 1 ? 0 : 1); band < 4; band++) {
				int levelWidth = LevelWidth(level);
				int levelHeight = LevelHeight(level);

				int blocksX8 = (levelWidth + 7) / 8;
				int blocksY8 = (levelHeight + 7) / 8;
				int blocksX32 = (levelWidth + 31) / 32;
				int blocksY32 = (blocksY8 + 3) / 4;

				m_blockMeta[component][level][band] = {
					m_blockCount8x8, blocksX8,
					m_blockCount32x32, blocksX32,
				};

				m_blockCount32x32 += blocksX32 * blocksY32;
				m_blockCount8x8 += blocksX8 * blocksY8;
			}
		}

		// Block ids are assigned coarsest level first, so everything below
		// this bound is levels 4..3 — the blocks whose loss is catastrophic
		// rather than blurry (see docs/pyrowave-partial-du-design.md).
		if (level == kLevels - 2)
			m_coarseBlockEnd = m_blockCount32x32;
	}
}

bool Decoder::CreateResources(ID3D11Device *device) {
	// GPU decode timing queries (ring; results polled via PollGpuTimeMs)
	for (int i = 0; i < kGpuTimerSlots; i++) {
		D3D11_QUERY_DESC qd = {};
		qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		DX::ThrowIfFailed(device->CreateQuery(&qd, &m_gpuTimers[i].disjoint), "disjoint query");
		qd.Query = D3D11_QUERY_TIMESTAMP;
		DX::ThrowIfFailed(device->CreateQuery(&qd, &m_gpuTimers[i].tsBegin), "timestamp query");
		DX::ThrowIfFailed(device->CreateQuery(&qd, &m_gpuTimers[i].tsEnd), "timestamp query");
	}

	// Wavelet band textures: one per level (instead of upstream's single
	// mipped image) so iDWT's read (level L) and write (level L-1) never
	// touch the same D3D11 resource. See docs/pyrowave-decoder-notes.md.
	for (int level = 0; level < kLevels; level++) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = LevelWidth(level);
		desc.Height = LevelHeight(level);
		desc.MipLevels = 1;
		desc.ArraySize = kBandsPerLevel * kComponents;
		desc.Format = DXGI_FORMAT_R16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, &m_bandTex[level]), "band texture");

		for (int comp = 0; comp < kComponents; comp++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			srvDesc.Texture2DArray.MipLevels = 1;
			srvDesc.Texture2DArray.FirstArraySlice = kBandsPerLevel * comp;
			srvDesc.Texture2DArray.ArraySize = kBandsPerLevel;
			DX::ThrowIfFailed(
			    device->CreateShaderResourceView(m_bandTex[level].Get(), &srvDesc, &m_bandSrv[comp][level]),
			    "band SRV");

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.FirstArraySlice = kBandsPerLevel * comp;
			uavDesc.Texture2DArray.ArraySize = kBandsPerLevel;
			DX::ThrowIfFailed(
			    device->CreateUnorderedAccessView(m_bandTex[level].Get(), &uavDesc, &m_bandUav[comp][level]),
			    "band UAV");

			// LL band only (slice 4*comp), viewed as a 1-slice array so the
			// idwt kernel's image2D store (mapped by spirv-cross to a
			// RWTexture2D) still works: keep it Texture2D dimension.
			D3D11_UNORDERED_ACCESS_VIEW_DESC llDesc = {};
			llDesc.Format = desc.Format;
			llDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			llDesc.Texture2DArray.FirstArraySlice = kBandsPerLevel * comp;
			llDesc.Texture2DArray.ArraySize = 1;
			DX::ThrowIfFailed(
			    device->CreateUnorderedAccessView(m_bandTex[level].Get(), &llDesc, &m_llUav[comp][level]),
			    "LL UAV");
		}
	}

	// Immutable per-dispatch constant buffers (contents are frame-invariant)
	auto makeCb = [&](const void *data, UINT bytes, ComPtr<ID3D11Buffer> &cb) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = bytes;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
		DX::ThrowIfFailed(device->CreateBuffer(&desc, &init, &cb), "PyroWave cbuffer");
	};

	for (int level = 0; level < kLevels; level++) {
		for (int comp = 0; comp < kComponents; comp++) {
			if (level == 0 && comp != 0 && !m_chroma444)
				continue;
			for (int band = (level == kLevels - 1 ? 0 : 1); band < 4; band++) {
				DequantConstants c = {};
				c.resolution[0] = LevelWidth(level);
				c.resolution[1] = LevelHeight(level);
				c.outputLayer = band;
				c.blockOffset32x32 = m_blockMeta[comp][level][band].blockOffset32x32;
				c.blockStride32x32 = m_blockMeta[comp][level][band].blockStride32x32;
				makeCb(&c, sizeof(c), m_dequantCb[comp][level][band]);
			}
		}

		// iDWT constants are transposed: x = height, y = width (the kernel
		// operates transposed; mirrors upstream idwt() exactly).
		IdwtConstants ic = {};
		ic.resolution[0] = LevelHeight(level);
		ic.resolution[1] = LevelWidth(level);
		ic.invResolution[0] = 1.0f / (float)ic.resolution[0];
		ic.invResolution[1] = 1.0f / (float)ic.resolution[1];
		makeCb(&ic, sizeof(ic), m_idwtCb[level]);
	}

	D3D11_SAMPLER_DESC samp = {};
	samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	samp.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR;
	samp.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR;
	samp.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
	DX::ThrowIfFailed(device->CreateSamplerState(&samp, &m_mirrorSampler), "mirror sampler");

	// Offsets buffer: one u32 per 32x32 block, raw SRV at t1
	D3D11_BUFFER_DESC offsetsDesc = {};
	offsetsDesc.ByteWidth = (UINT)(m_blockCount32x32 * sizeof(uint32_t));
	offsetsDesc.Usage = D3D11_USAGE_DEFAULT;
	offsetsDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	offsetsDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	DX::ThrowIfFailed(device->CreateBuffer(&offsetsDesc, nullptr, &m_offsetsBuffer), "offsets buffer");

	D3D11_SHADER_RESOURCE_VIEW_DESC rawDesc = {};
	rawDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	rawDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
	rawDesc.BufferEx.NumElements = offsetsDesc.ByteWidth / 4;
	rawDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
	DX::ThrowIfFailed(device->CreateShaderResourceView(m_offsetsBuffer.Get(), &rawDesc, &m_offsetsSrv), "offsets SRV");

	return EnsurePayloadBuffer(64 * 1024);
}

bool Decoder::EnsurePayloadBuffer(size_t requiredBytes) {
	// +16 padding avoids an OOB edge case in dequant (mirrors upstream).
	size_t requiredPadded = requiredBytes + 16;
	if (m_payloadBuffer && requiredPadded <= m_payloadBufferBytes)
		return true;

	size_t newSize = requiredPadded * 2;
	if (newSize < 64 * 1024)
		newSize = 64 * 1024;
	newSize = (newSize + 3) & ~size_t(3);

	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = (UINT)newSize;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ComPtr<ID3D11Buffer> buffer;
	DX::ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &buffer), "payload buffer");

	auto makeSrv = [&](DXGI_FORMAT format, UINT elements, ComPtr<ID3D11ShaderResourceView> &srv) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = elements;
		DX::ThrowIfFailed(m_device->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv), "payload SRV");
	};
	makeSrv(DXGI_FORMAT_R8_UINT, (UINT)newSize, m_payloadU8Srv);
	makeSrv(DXGI_FORMAT_R16_UINT, (UINT)(newSize / 2), m_payloadU16Srv);
	makeSrv(DXGI_FORMAT_R32_UINT, (UINT)(newSize / 4), m_payloadU32Srv);

	m_payloadBuffer = buffer;
	m_payloadBufferBytes = newSize;
	return true;
}

void Decoder::Clear() {
	std::fill(m_offsetsCpu.begin(), m_offsetsCpu.end(), UINT32_MAX);
	m_decodedBlocks = 0;
	m_sawBlockBeyondCoarse = false;
	m_lastSeq = UINT32_MAX;
	m_decodedFrameForCurrentSequence = false;
	m_totalBlocksInSequence = m_blockCount32x32;
	m_payloadCpu.clear();
}

// Port of Decoder::Impl::decode_packet
bool Decoder::DecodePacket(const BitstreamHeader *header) {
	auto &offset = m_offsetsCpu[header->block_index];
	if (offset == UINT32_MAX) {
		m_decodedBlocks++;
		offset = (uint32_t)m_payloadCpu.size();
	} else {
		return true; // duplicate packet
	}

	if (sizeof(*header) / sizeof(uint32_t) > header->payload_words) {
		Utils::Log("PyroWave: payload_words is not large enough\n");
		return false;
	}

	auto *payloadWords = reinterpret_cast<const uint32_t *>(header);
	m_payloadCpu.insert(m_payloadCpu.end(), payloadWords, payloadWords + header->payload_words);
	return true;
}

// Port of Decoder::Impl::push_packet
bool Decoder::PushPacket(const void *data_, size_t size, bool allowTruncated) {
	auto *data = static_cast<const uint8_t *>(data_);
	while (size >= sizeof(BitstreamHeader)) {
		auto *header = reinterpret_cast<const BitstreamHeader *>(data);

		if (header->extended != 0) {
			auto *seq = reinterpret_cast<const BitstreamSequenceHeader *>(header);

			if ((seq->chroma_resolution != 0) != m_chroma444) {
				Utils::Log("PyroWave: chroma resolution mismatch\n");
				return false;
			}

			uint8_t diff = (header->sequence - m_lastSeq) & kSequenceCountMask;
			if (m_lastSeq != UINT32_MAX && diff > (kSequenceCountMask / 2))
				return true; // stale sequence, ignore rest

			if (m_lastSeq == UINT32_MAX || diff != 0) {
				Clear();
				m_lastSeq = header->sequence;
			}

			if (seq->code == kStartOfFrame) {
				if ((int)seq->width_minus_1 + 1 != m_width || (int)seq->height_minus_1 + 1 != m_height) {
					Utils::Logf("PyroWave: dimension mismatch in seq packet (%u, %u) != (%d, %d)\n",
					    seq->width_minus_1 + 1, seq->height_minus_1 + 1, m_width, m_height);
					return false;
				}
				m_totalBlocksInSequence = (int)seq->total_blocks;

				m_colorimetry.colorPrimaries = seq->color_primaries;
				m_colorimetry.transferFunction = seq->transfer_function;
				m_colorimetry.ycbcrTransform = seq->ycbcr_transform;
				m_colorimetry.ycbcrRange = seq->ycbcr_range;
				m_colorimetry.chromaSiting = seq->chroma_siting;
				m_colorimetry.valid = true;
			} else {
				Utils::Logf("PyroWave: unrecognized sequence header mode %u\n", seq->code);
				return false;
			}

			data += sizeof(*header);
			size -= sizeof(*header);
			continue;
		}

		size_t packetSize = header->payload_words * sizeof(uint32_t);
		if (packetSize > size) {
			if (allowTruncated) {
				// Even the truncated packet's header is evidence for the
				// partial-frame readiness rule: packets arrive in ascending
				// block-index order, so a packet starting past the coarse
				// levels proves every transmitted coarse block came before it.
				if (m_lastSeq != UINT32_MAX &&
				    ((header->sequence - m_lastSeq) & kSequenceCountMask) == 0 &&
				    header->block_index < (uint32_t)m_blockCount32x32 &&
				    header->block_index >= (uint32_t)m_coarseBlockEnd)
					m_sawBlockBeyondCoarse = true;
				return true; // cut off mid-packet; keep what we decoded
			}
			Utils::Logf("PyroWave: packet header states %zu bytes, but only %zu left\n", packetSize, size);
			return false;
		}

		bool restart;
		if (m_lastSeq == UINT32_MAX) {
			restart = true;
		} else {
			uint8_t diff = (header->sequence - m_lastSeq) & kSequenceCountMask;
			if (diff > (kSequenceCountMask / 2))
				return true; // stale
			restart = diff != 0;
		}

		if (restart) {
			Clear();
			m_lastSeq = header->sequence;
		}

		if (header->block_index >= (uint32_t)m_blockCount32x32) {
			Utils::Logf("PyroWave: block_index %u out of bounds (>= %d)\n", header->block_index, m_blockCount32x32);
			return false;
		}

		if (header->block_index >= (uint32_t)m_coarseBlockEnd)
			m_sawBlockBeyondCoarse = true;

		if (!DecodePacket(header))
			return false;

		data += packetSize;
		size -= packetSize;
	}

	if (size != 0 && !allowTruncated) {
		Utils::Log("PyroWave: did not consume packet completely\n");
		return false;
	}
	return true;
}

bool Decoder::DecodeIsReady(bool allowPartialFrame) const {
	if (m_decodedFrameForCurrentSequence)
		return false;
	if (m_lastSeq == UINT32_MAX)
		return false;
	// Partial frames are worth showing as long as the coarse levels (4 and 3)
	// are fully present: the image degrades to blur, never to garbage. A
	// prefix carrying only ~20% of the frame's bytes decodes at ~37 dB on
	// real game content (docs/pyrowave-partial-du-design.md), which upstream's
	// ">half the blocks" rule would have rejected.
	//
	// Coverage cannot be counted directly — only blocks with data are
	// transmitted, and the receiver has no per-level transmitted counts. But
	// packetize() emits blocks in ascending index order and partial delivery
	// is a byte-prefix of that stream, so having seen any packet start at or
	// past the coarse boundary proves every transmitted coarse block arrived.
	if (m_decodedBlocks < m_totalBlocksInSequence)
		if (!allowPartialFrame || !m_sawBlockBeyondCoarse)
			return false;
	return true;
}

void Decoder::Dequant(ID3D11DeviceContext *ctx) {
	ctx->CSSetShader(m_dequantShader.Get(), nullptr, 0);
	ID3D11ShaderResourceView *srvs[] = {
		nullptr, m_offsetsSrv.Get(), m_payloadU32Srv.Get(), m_payloadU16Srv.Get(), m_payloadU8Srv.Get()
	};
	ctx->CSSetShaderResources(0, 5, srvs);

	for (int level = 0; level < kLevels; level++) {
		for (int comp = 0; comp < kComponents; comp++) {
			if (level == 0 && comp != 0 && !m_chroma444)
				continue;

			ID3D11UnorderedAccessView *uavs[] = { m_bandUav[comp][level].Get() };
			ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

			for (int band = (level == kLevels - 1 ? 0 : 1); band < 4; band++) {
				ID3D11Buffer *cbs[] = { m_dequantCb[comp][level][band].Get() };
				ctx->CSSetConstantBuffers(0, 1, cbs);
				ctx->Dispatch((LevelWidth(level) + 31) / 32, (LevelHeight(level) + 31) / 32, 1);
			}
		}
	}
}

void Decoder::Idwt(ID3D11DeviceContext *ctx, ID3D11UnorderedAccessView *const planeUavs[3]) {
	ID3D11SamplerState *samplers[] = { m_mirrorSampler.Get() };
	ctx->CSSetSamplers(0, 1, samplers);

	for (int inputLevel = kLevels - 1; inputLevel >= 0; inputLevel--) {
		ID3D11Buffer *cbs[] = { m_idwtCb[inputLevel].Get() };
		ctx->CSSetConstantBuffers(0, 1, cbs);

		// Transposed dispatch dims, mirrors upstream idwt() exactly.
		UINT groupsX = (UINT)((LevelHeight(inputLevel) + 15) / 16);
		UINT groupsY = (UINT)((LevelWidth(inputLevel) + 15) / 16);

		for (int c = 0; c < kComponents; c++) {
			if (inputLevel == 0 && c != 0 && !m_chroma444)
				continue;

			bool finalOutput = (inputLevel == 0) ||
			                   (!m_chroma444 && c != 0 && inputLevel == 1);

			ctx->CSSetShader(finalOutput ? m_idwtDcShader.Get() : m_idwtShader.Get(), nullptr, 0);

			ID3D11ShaderResourceView *srvs[] = { m_bandSrv[c][inputLevel].Get() };
			ctx->CSSetShaderResources(0, 1, srvs);

			ID3D11UnorderedAccessView *uavs[] = {
				nullptr,
				finalOutput ? planeUavs[c] : m_llUav[c][inputLevel - 1].Get()
			};
			ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

			ctx->Dispatch(groupsX, groupsY, 1);

			// Unbind the SRV before the next level writes this texture.
			ID3D11ShaderResourceView *nullSrv[] = { nullptr };
			ctx->CSSetShaderResources(0, 1, nullSrv);
		}
	}
}

bool Decoder::Decode(ID3D11DeviceContext *ctx, ID3D11UnorderedAccessView *const planeUavs[3]) {
	// Uploads
	size_t payloadBytes = m_payloadCpu.size() * sizeof(uint32_t);
	if (!EnsurePayloadBuffer(payloadBytes))
		return false;

	// Time this frame's decode on the GPU if a ring slot is free (skipping
	// frames when the ring is full is fine — this feeds an average).
	GpuTimerSlot &slot = m_gpuTimers[m_gpuTimerWrite];
	const bool timeThisFrame = !slot.pending;
	if (timeThisFrame) {
		ctx->Begin(slot.disjoint.Get());
		ctx->End(slot.tsBegin.Get());
	}

	if (payloadBytes) {
		D3D11_BOX box = { 0, 0, 0, (UINT)payloadBytes, 1, 1 };
		ctx->UpdateSubresource(m_payloadBuffer.Get(), 0, &box, m_payloadCpu.data(), 0, 0);
	}
	ctx->UpdateSubresource(m_offsetsBuffer.Get(), 0, nullptr, m_offsetsCpu.data(), 0, 0);

	Dequant(ctx);
	Idwt(ctx, planeUavs);

	if (timeThisFrame) {
		ctx->End(slot.tsEnd.Get());
		ctx->End(slot.disjoint.Get());
		slot.pending = true;
		m_gpuTimerWrite = (m_gpuTimerWrite + 1) % kGpuTimerSlots;
	}

	// Leave compute state clean: the render thread binds plane SRVs next.
	ID3D11UnorderedAccessView *nullUavs[2] = {};
	ID3D11ShaderResourceView *nullSrvs[5] = {};
	ID3D11Buffer *nullCb[1] = {};
	ID3D11SamplerState *nullSampler[1] = {};
	ctx->CSSetShader(nullptr, nullptr, 0);
	ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	ctx->CSSetShaderResources(0, 5, nullSrvs);
	ctx->CSSetConstantBuffers(0, 1, nullCb);
	ctx->CSSetSamplers(0, 1, nullSampler);

	m_decodedFrameForCurrentSequence = true;
	return true;
}

bool Decoder::PollGpuTimeMs(ID3D11DeviceContext *ctx, double *outMs) {
	GpuTimerSlot &slot = m_gpuTimers[m_gpuTimerRead];
	if (!slot.pending)
		return false;

	// The disjoint query completes last; once it's ready the timestamps are
	// too, but check each without flushing so we never stall the pipeline.
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
	if (ctx->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		return false;
	UINT64 tBegin, tEnd;
	if (ctx->GetData(slot.tsBegin.Get(), &tBegin, sizeof(tBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
	    ctx->GetData(slot.tsEnd.Get(), &tEnd, sizeof(tEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		return false;

	slot.pending = false;
	m_gpuTimerRead = (m_gpuTimerRead + 1) % kGpuTimerSlots;

	if (disjoint.Disjoint || disjoint.Frequency == 0 || tEnd < tBegin)
		return false; // GPU clock changed mid-measurement; discard

	*outMs = double(tEnd - tBegin) * 1000.0 / double(disjoint.Frequency);
	return true;
}

} // namespace PyroWaveD3D11
} // namespace moonlight_xbox_dx
