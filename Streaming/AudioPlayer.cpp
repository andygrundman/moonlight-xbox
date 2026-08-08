#include "pch.h"
#include <Streaming\AudioPlayer.h>
#include <Utils.hpp>
#include "..\Plot\ImGuiPlots.h"
#include "State\Stats.h"
#include <opus/opus_multistream.h>
#include <algorithm>

#if defined(_DEBUG)
#define MA_DEBUG_OUTPUT
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"

static void AudioPlayer_LogCallback(void *pUserData, ma_uint32 level, const char *pMessage) {
	(void)pUserData;
	if (level <= MA_LOG_LEVEL_INFO) {
		moonlight_xbox_dx::Utils::Logf("[miniaudio] %s", pMessage);
	}
}

namespace moonlight_xbox_dx {

	static OpusMSDecoder *s_opusDecoder = nullptr;
	static int s_channelCount = 0;

	static int audioInitCallback(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void *context, int arFlags) noexcept {
		(void)audioConfiguration;
		(void)context;
		(void)arFlags;
		int rc;

		if (opusConfig->channelCount > MAX_CHANNEL_COUNT ||
		    opusConfig->samplesPerFrame > MAX_SAMPLES_PER_FRAME) {
			Utils::Logf("Unsupported audio config: %d channels, %d samples/frame\n",
			            opusConfig->channelCount, opusConfig->samplesPerFrame);
			return -1;
		}

		s_opusDecoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount,
		                                                opusConfig->streams, opusConfig->coupledStreams,
		                                                opusConfig->mapping, &rc);
		if (rc != 0) {
			s_opusDecoder = nullptr;
			return rc;
		}
		s_channelCount = opusConfig->channelCount;

		if (!AudioPlayer::instance().prepareForPlayback(opusConfig)) {
			return -1;
		}
		return 0;
	}

	static void audioStartCallback() noexcept {
		AudioPlayer::instance().start();
	}

	static void audioStopCallback() noexcept {
		AudioPlayer::instance().stop();
	}

	static void audioCleanupCallback() noexcept {
		AudioPlayer::instance().cleanup();
	}

	static void audioDecodeAndPlaySampleCallback(char *sampleData, int sampleLength) noexcept {
		if (!s_channelCount) {
			Utils::Logf("AudioPlayer not initialized, can't decode\n");
			return;
		}
		int desiredBufferSize = 0; // indicates we want the optimal size
		float *buffer = (float *)AudioPlayer::instance().getAudioBuffer(&desiredBufferSize);
		int maxFrames = desiredBufferSize / (s_channelCount * (int)sizeof(float));
		int decodeLen = opus_multistream_decode_float(s_opusDecoder, (unsigned char *)sampleData,
		                                              sampleLength, buffer, maxFrames, 0);
		if (decodeLen < 0) {
			Utils::Logf("opus_multistream_decode_float failed: %d\n", decodeLen);
			Stats::instance().SubmitAudioGlitch();
			return;
		}
		else if (decodeLen > 0) {
			uint32_t framesDecoded = decodeLen * s_channelCount * sizeof(float);
			if (!AudioPlayer::instance().submitAudio(framesDecoded)) {
				Stats::instance().SubmitAudioGlitch();
			}
		}
	}

	AudioPlayer &AudioPlayer::instance() {
		static AudioPlayer inst;
		return inst;
	}

	AUDIO_RENDERER_CALLBACKS AudioPlayer::getDecoder() {
		AUDIO_RENDERER_CALLBACKS callbacks;
		LiInitializeAudioCallbacks(&callbacks);
		callbacks.init = audioInitCallback;
		callbacks.start = audioStartCallback;
		callbacks.stop = audioStopCallback;
		callbacks.cleanup = audioCleanupCallback;
		callbacks.decodeAndPlaySample = audioDecodeAndPlaySampleCallback;
		callbacks.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
		return callbacks;
	}

	void AudioPlayer::deviceDataCallback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
		(void)pInput;
		AudioPlayer *me = (AudioPlayer *)pDevice->pUserData;

		ma_uint32 bpf = ma_pcm_rb_get_bpf(&me->rb);
		ma_uint32 framesRead = 0;
		while (framesRead < frameCount) {
			void *buffer;
			ma_uint32 len = frameCount - framesRead;
			ma_result res = ma_pcm_rb_acquire_read(&me->rb, &len, &buffer);
			if (res != MA_SUCCESS) {
				Utils::Log("Failed to acquire audio ring buffer for reading\n");
				break;
			}
			if (len == 0) {
				// Underrun
				ma_pcm_rb_commit_read(&me->rb, 0);
				break;
			}
			memcpy((ma_uint8 *)pOutput + (size_t)framesRead * bpf, buffer, (size_t)len * bpf);
			res = ma_pcm_rb_commit_read(&me->rb, len);
			if (res != MA_SUCCESS && res != MA_AT_END) {
				Utils::Log("Failed to commit audio ring buffer read\n");
				break;
			}
			framesRead += len;
		}
	}

	bool AudioPlayer::prepareForPlayback(const POPUS_MULTISTREAM_CONFIGURATION opusConfig) {
		this->samplesPerFrame = opusConfig->samplesPerFrame;
		this->channelCount = opusConfig->channelCount;

		ma_device_config deviceConfig;
		deviceConfig = ma_device_config_init(ma_device_type_playback);
		deviceConfig.playback.format = ma_format_f32;
		deviceConfig.playback.channels = opusConfig->channelCount;
		deviceConfig.sampleRate = opusConfig->sampleRate;
		deviceConfig.dataCallback = deviceDataCallback;
		deviceConfig.pUserData = this;
		deviceConfig.noFixedSizedCallback = true; // reduces latency by not creating miniaudio's intermediate buffer
		deviceConfig.wasapi.usage = ma_wasapi_usage_pro_audio; // give WASAPI thread high priority

		// Specify a custom log object in the config so any logs that are posted
		// from ma_context_init() are captured.
		if (ma_log_init(NULL, &log) == MA_SUCCESS) {
			logInitialized = true;
			ma_log_register_callback(&log, ma_log_callback_init(&AudioPlayer_LogCallback, NULL));
		}
		ma_context_config config = ma_context_config_init();
		config.pLog = logInitialized ? &log : NULL;

		if (ma_context_init(NULL, 1, &config, &context) != MA_SUCCESS) {
			Utils::Log("Failed to create miniaudio context.\n");
			goto fail;
		}
		contextInitialized = true;

		if (ma_device_init(&context, &deviceConfig, &device) != MA_SUCCESS) {
			Utils::Log("Failed to open playback device.\n");
			goto fail;
		}
		deviceInitialized = true;

		if (ma_pcm_rb_init(ma_format_f32, opusConfig->channelCount,
		                   opusConfig->samplesPerFrame * RB_FRAME_CAPACITY,
		                   NULL, NULL, &rb) != MA_SUCCESS) {
			Utils::Log("Failed to create audio ring buffer\n");
			goto fail;
		}
		rbInitialized = true;

		ma_pcm_rb_set_sample_rate(&rb, 48000);

		memset(decodeBuffer, 0, sizeof(decodeBuffer));

		Utils::Logf("Opus stream config: %d-channel 48kHz, %d samples per frame, internal period size %lu, buffer %dms\n",
			opusConfig->channelCount, opusConfig->samplesPerFrame,
			device.playback.internalPeriodSizeInFrames, bufferSizeMs.load());

		return true;

	fail:
		cleanup();
		return false;
	}

	void AudioPlayer::cleanup() {
		Utils::Log("Audio Cleanup\n");
		if (deviceInitialized) {
			ma_device_uninit(&device); // implicitly stops the device first
			deviceInitialized = false;
		}
		if (rbInitialized) {
			ma_pcm_rb_uninit(&rb);
			rbInitialized = false;
		}
		if (contextInitialized) {
			ma_context_uninit(&context);
			contextInitialized = false;
		}
		if (logInitialized) {
			ma_log_uninit(&log);
			logInitialized = false;
		}
		if (s_opusDecoder != nullptr) {
			opus_multistream_decoder_destroy(s_opusDecoder);
			s_opusDecoder = nullptr;
		}
		s_channelCount = 0;
	}

	void *AudioPlayer::getAudioBuffer(int *size) {
		// The scratch buffer (decodeBuffer) is always large enough for one Opus frame.
		int capacity = (int)sizeof(decodeBuffer);
		*size = *size > 0 ? std::min(*size, capacity) : capacity;
		return decodeBuffer;
	}

	bool AudioPlayer::submitAudio(int bytesWritten) {
		if (bytesWritten <= 0) {
			return true;
		}

		ma_uint32 bpf = ma_pcm_rb_get_bpf(&rb);
		ma_uint32 framesTotal = (ma_uint32)bytesWritten / bpf;

		// our audio latency is the sum of the network buffers in common-c, plus miniaudio's ring buffer (plus
		// additional OS buffers out of our control. For now we will try to mimic moonlight-qt (which drops
		// at 30 network or 50 ring), and also let the user set the ring buffer drop amount.
		int pendingNetworkMs = LiGetPendingAudioDuration();
		ma_uint32 pendingAudioMs = ma_pcm_rb_available_read(&rb) / 48;

		ImGuiPlots::instance().observeFloat(PLOT_AUDIO_BUFFER_MS, (float)pendingNetworkMs + pendingAudioMs);

		// Don't queue if there's already more than 30 ms of audio data waiting
		// in Moonlight's audio queue. This is hardcoded in all Moonlight clients.
		if (pendingNetworkMs > 30) {
			return false;
		}

		// The user-controllable buffer size is for the ring buffer. Moonlight uses a 50ms check here,
		// but I think 30ms is a better default
		if (pendingAudioMs > bufferSizeMs.load()) {
			return false;
		}

		// Copy the decoded frame into the ring buffer, looping across the
		// wrap point if necessary.
		ma_uint32 framesWritten = 0;
		while (framesWritten < framesTotal) {
			void *buffer;
			ma_uint32 len = framesTotal - framesWritten;
			ma_result r = ma_pcm_rb_acquire_write(&rb, &len, &buffer);
			if (r != MA_SUCCESS) {
				Utils::Log("Failed to acquire audio ring buffer for writing\n");
				return false;
			}
			if (len == 0) {
				// Ring buffer is full
				ma_pcm_rb_commit_write(&rb, 0);
				Utils::Logf("Audio ring buffer full, dropping %u of %u frames\n",
				            framesTotal - framesWritten, framesTotal);
				return false;
			}
			memcpy(buffer, (ma_uint8 *)decodeBuffer + (size_t)framesWritten * bpf, (size_t)len * bpf);
			r = ma_pcm_rb_commit_write(&rb, len);
			if (r != MA_SUCCESS && r != MA_AT_END) {
				Utils::Log("Failed to commit audio ring buffer write\n");
				return false;
			}
			framesWritten += len;
		}

		return true;
	}

	void AudioPlayer::start() {
		if (ma_device_start(&device) != MA_SUCCESS) {
			Utils::Log("Failed to start playback device.\n");
		}
	}

	void AudioPlayer::stop() {
		if (ma_device_stop(&device) != MA_SUCCESS) {
			Utils::Log("Failed to stop playback device.\n");
		}
	}

	int AudioPlayer::GetAudioBufferMs() {
		return bufferSizeMs.load();
	}

	int AudioPlayer::SetAudioBufferMs(int wantedMs) {
		int currentMs = bufferSizeMs.load();
		int actualMs = std::max(std::min(wantedMs, MAX_BUFFER_MS), MIN_BUFFER_MS);
		bufferSizeMs.store(actualMs);
		ImGuiPlots::instance().clearBuffer(PLOT_AUDIO_BUFFER_MS);
		Stats::instance().ResetAudioGlitchCount();

		Utils::Logf("Audio buffer is now %d ms\n", actualMs);
		return actualMs;
	}

} // namespace moonlight_xbox_dx
