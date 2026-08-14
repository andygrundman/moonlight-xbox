#pragma once
#include "pch.h"
extern "C" {
#include <Limelight.h>
}
#include "third_party/miniaudio.h"

#define MAX_CHANNEL_COUNT 8
#define MAX_SAMPLES_PER_FRAME (48000 / 1000 * 120)

// Ring buffer capacity, 10ms per frame, more than we should need with 50ms max
#define RB_FRAME_CAPACITY 8

// Min/Max buffer sizes the user can choose
#define DEFAULT_BUFFER_MS 30
#define MIN_BUFFER_MS 10
#define MAX_BUFFER_MS 50

namespace moonlight_xbox_dx
{

	// PCM audio renderer built on miniaudio. Opus decoding lives in the
	// moonlight-common-c callback glue in AudioPlayer.cpp; this class only
	// manages the playback device and the ring buffer feeding it.
	class AudioPlayer {
	public:
		static AudioPlayer &instance();
		static AUDIO_RENDERER_CALLBACKS getDecoder();

		bool prepareForPlayback(const POPUS_MULTISTREAM_CONFIGURATION opusConfig);
		void start();
		void stop();
		void cleanup();

		// Returns the scratch buffer decoded PCM should be written into,
		// with *size set to its capacity in bytes.
		void *getAudioBuffer(int *size);

		// Queues the first bytesWritten bytes of the scratch buffer for
		// playback. Returns false if the audio was dropped.
		bool submitAudio(int bytesWritten);

		int GetAudioBufferMs();
		int SetAudioBufferMs(int ms);

	private:
		AudioPlayer() = default;
		AudioPlayer(const AudioPlayer &) = delete;
		AudioPlayer &operator=(const AudioPlayer &) = delete;

		// miniaudio device data callback; pUserData is the AudioPlayer.
		static void deviceDataCallback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount);

		ma_pcm_rb rb{};
		ma_device device{};
		ma_context context{};
		ma_log log{};

		// Track what actually got initialized so cleanup() / error unwinding
		// only tears down what exists; a second session's prepareForPlayback
		// must not re-init live objects.
		bool rbInitialized = false;
		bool deviceInitialized = false;
		bool contextInitialized = false;
		bool logInitialized = false;

		float decodeBuffer[MAX_CHANNEL_COUNT * MAX_SAMPLES_PER_FRAME];
		int channelCount = 0;
		int samplesPerFrame = 0;
		std::atomic<int> bufferSizeMs{DEFAULT_BUFFER_MS};
	};
}
