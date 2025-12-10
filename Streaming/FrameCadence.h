#pragma once

#include <algorithm>
#include <cstdint>

// FrameCadence
//
// Map incoming stream framerate to display refresh rate.
//
// Usage pattern:
//
//   FrameCadence cadence(59.94);
//
//   // Decoder thread: whenever a decoded frame arrives
//   cadence.observeFramePts(frame->pts);
//
//   // Render thread: once per present slot
//   int advanceCount = cadence.decideAdvanceCount();
//
//   // Use advanceCount to decide how many frames to dequeue and render;
//   // usually 0 or 1, sometimes >1 when stream fps > display Hz.
//
class FrameCadence {
  public:
	explicit FrameCadence(double displayHz) {
		setDisplayHz(displayHz);

		m_streamPeriodMs = 1000.0 / displayHz;
		m_streamPeriodEwmaMs = m_streamPeriodMs;
		m_streamEwmaAlpha = 0.10;
		m_minStreamPeriodMs = 1000.0 / 120.0;
		m_maxStreamPeriodMs = 1000.0 / 10.0;
		m_lastPts90k = 0;
		m_haveLastPts = false;
		m_phase = 0.0;
		m_framesPerVblank = m_displayPeriodMs / m_streamPeriodMs;
		m_maxAdvancePerPresent = 2; // do not eat more than N frames in one slot (120fps in 60hz)
	}

	// Set or update display refresh rate in Hz.
	void setDisplayHz(double hz) {
		if (hz <= 0.0) {
			hz = 59.94;
		}
		m_displayHz = hz;
		m_displayPeriodMs = 1000.0 / m_displayHz;
		updateFramesPerVblank();
	}

	double displayHz() const {
		return m_displayHz;
	}
	double displayPeriodMs() const {
		return m_displayPeriodMs;
	}

	void reset() {
		m_streamPeriodEwmaMs = m_streamPeriodMs;
		m_lastPts90k = 0;
		m_haveLastPts = false;
		m_phase = 0.0;
		updateFramesPerVblank();
	}

	// Observe an incoming frame timestamp in 90 kHz RTP timebase
	void observeFramePts(int64_t pts90k) {
		if (pts90k == INT64_MIN) {
			return;
		}

		if (m_haveLastPts) {
			int64_t deltaPts = pts90k - m_lastPts90k;
			if (deltaPts > 0) {
				double deltaMs = static_cast<double>(deltaPts) / 90.0;
				deltaMs = std::clamp(deltaMs, m_minStreamPeriodMs, m_maxStreamPeriodMs);

				m_streamPeriodEwmaMs = (deltaMs * m_streamEwmaAlpha) + (m_streamPeriodEwmaMs * (1.0 - m_streamEwmaAlpha));

				m_streamPeriodMs = m_streamPeriodEwmaMs;
				updateFramesPerVblank();
			}
		}

		m_lastPts90k = pts90k;
		m_haveLastPts = true;
	}

	// Force a specific stream fps. This may be useful if a very old server is not sending
	// valid pts timestamps.
	void setStreamFpsOverride(double fps) {
		if (fps <= 0.0) {
			return;
		}
		double periodMs = 1000.0 / fps;
		periodMs = std::clamp(periodMs, m_minStreamPeriodMs, m_maxStreamPeriodMs);

		m_streamPeriodMs = periodMs;
		m_streamPeriodEwmaMs = periodMs;
		updateFramesPerVblank();
	}

	double streamPeriodMs() const {
		return m_streamPeriodMs;
	}
	double streamFps() const {
		return (m_streamPeriodMs > 0.0) ? (1000.0 / m_streamPeriodMs) : 0.0;
	}
	double framesPerVblank() const {
		return m_framesPerVblank;
	}

	// Decide how many stream frames should be consumed for this present interval.
	//
	// Return value:
	//   0  -> reuse the current displayed frame
	//   1  -> advance to the next frame
	//   2+ -> drop some incoming frames (e.g. 120 fps stream on 60 Hz display)
	int decideAdvanceCount() {
		if (m_framesPerVblank <= 0.0) {
			m_framesPerVblank = 1.0;
		}

		m_phase += m_framesPerVblank;

		int advanceCount = 0;
		while (m_phase >= 1.0 && advanceCount < m_maxAdvancePerPresent) {
			m_phase -= 1.0;
			++advanceCount;
		}

		if (m_phase < 0.0) {
			m_phase = 0.0;
		} else if (m_phase >= 1.0) {
			m_phase -= static_cast<int>(m_phase);
		}

		return advanceCount;
	}

  private:
	void updateFramesPerVblank() {
		if (m_streamPeriodMs <= 0.0) {
			m_framesPerVblank = 1.0;
			return;
		}

		m_framesPerVblank = m_displayPeriodMs / m_streamPeriodMs;
	}

  private:
	double m_displayHz;
	double m_displayPeriodMs;

	double m_streamPeriodMs;
	double m_streamPeriodEwmaMs;
	double m_streamEwmaAlpha;

	double m_minStreamPeriodMs;
	double m_maxStreamPeriodMs;

	int64_t m_lastPts90k;
	bool m_haveLastPts;

	double m_framesPerVblank;
	double m_phase;

	int m_maxAdvancePerPresent;
};
