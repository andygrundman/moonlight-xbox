#pragma once

// Moonlight-facing PyroWave decoder shim, sibling of FFMpegDecoder.
// The DECODER_RENDERER_CALLBACKS shims in FFmpegDecoder.cpp route here when
// the negotiated format has VIDEO_FORMAT_MASK_PYROWAVE set. Each decode unit
// carries one frame framed as [u32 count]{[u32 size][bytes]}*; the shim
// strips that transport framing, feeds the raw self-delimiting packets to
// PyroWaveD3D11::Decoder, decodes into FramePool planes and hands the
// wrapped AVFrame to Pacer (which owns it from then on; the pool recycles
// plane sets via the AVFrame free callback).

#include "PyroWave\D3D11Decoder.h"
#include "PyroWave\FramePool.h"
#include <atomic>
#include <memory>
#include <vector>

extern "C" {
#include <Limelight.h>
}

namespace DX {
class DeviceResources;
}

namespace moonlight_xbox_dx {

class PyroWaveDecoder {
  public:
	static PyroWaveDecoder &instance();

	// Called before connection start (alongside FFMpegDecoder's): stashes
	// device resources and the negotiated colorspace/range for AVFrame
	// color fields.
	void CompleteInitialization(const std::shared_ptr<DX::DeviceResources> &res, STREAM_CONFIGURATION *config);

	int Init(int videoFormat, int width, int height, int redrawRate);
	void Cleanup();
	int SubmitDecodeUnit(PDECODE_UNIT decodeUnit);

	bool IsActive() const { return m_active; }

	// Dev tool: arm a capture of the next few complete decode units, written to
	// LocalState as the same [u32 count]{[u32 size][bytes]}* framing the host
	// sent. Those files feed tools/pyrowave_dump_golden and the loss/FEC sims.
	// Safe to call from any thread; a no-op when PyroWave isn't the active
	// decoder. Returns the number of frames armed (0 if unavailable).
	int CaptureFrames(int count);

	int videoFormat = 0;
	int width = 0, height = 0;

	// For debugging, this value can be set to save the first few partial frames
	// for later analysis with python3 tools/pyrowave_preview.py <capture>.bin
	static constexpr int kPartialCapturesPerStream = 0;

  private:
	// lostPercent < 0 marks a complete capture; >= 0 a partial one, tagged
	// with that percentage in the filename.
	void WriteCaptureAsync(size_t length, int frameNumber, int lostPercent);
	void WriteCapture(const uint8_t *data, size_t length, int frameNumber, int lostPercent);

	PyroWaveDecoder() = default;
	PyroWaveDecoder(const PyroWaveDecoder &) = delete;
	PyroWaveDecoder &operator=(const PyroWaveDecoder &) = delete;

	void ApplyFrameColor(AVFrame *frame);

	std::shared_ptr<DX::DeviceResources> m_deviceResources;
	int m_negColorSpace = 0;  // COLORSPACE_* from STREAM_CONFIGURATION
	int m_negColorRange = 0;  // COLOR_RANGE_*

	std::unique_ptr<PyroWaveD3D11::Decoder> m_decoder;
	std::unique_ptr<PyroWaveD3D11::FramePool> m_pool;
	// Pools whose frames may still be in flight when a new session starts;
	// freed on the next Init (Pacer has long since drained them by then).
	std::vector<std::unique_ptr<PyroWaveD3D11::FramePool>> m_retiredPools;

	std::vector<uint8_t> m_duBuffer;
	int m_LastFrameNumber = 0;
	int64_t m_StreamEpochQpc = 0;
	bool m_active = false;

	// Armed from the UI thread, consumed on the VideoDec thread.
	std::atomic<int> m_captureFramesRemaining {0};
	// Auto-armed at stream start; only touched on the VideoDec thread but kept
	// atomic for symmetry with the manual counter.
	std::atomic<int> m_partialCapturesRemaining {0};
};

} // namespace moonlight_xbox_dx
