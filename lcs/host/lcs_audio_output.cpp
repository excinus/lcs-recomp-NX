#include "lcs_audio_output.hpp"
#include "lcs_audio_mixer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

namespace lcs {
namespace {

struct WaveOutDevice {
    struct Block {
        WAVEHDR header{};
        std::vector<std::int16_t> samples;
    };

    HWAVEOUT device{nullptr};
    std::vector<Block> blocks;
    std::size_t next_block{};
    bool opened{};
    bool failed{};

    bool ensure_open(AudioMixer &mixer) {
        if (opened) return true;
        if (failed) return false;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(AudioMixer::kOutputChannels);
        format.nSamplesPerSec = AudioMixer::kSampleRate;
        format.wBitsPerSample = 16u;
        format.nBlockAlign = static_cast<WORD>(AudioMixer::kOutputChannels * sizeof(std::int16_t));
        format.nAvgBytesPerSec = AudioMixer::kSampleRate * format.nBlockAlign;
        const MMRESULT open_result = waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
        if (open_result != MMSYSERR_NOERROR) {
            failed = true;
            device = nullptr;
            return false;
        }
        waveOutPause(device);
        blocks.resize(AudioMixer::kBlockCount);
        next_block = 0u;
        opened = true;
        mixer.note_device_opened();
        return true;
    }

    [[nodiscard]] std::size_t outstanding_blocks() const {
        return static_cast<std::size_t>(std::count_if(
            blocks.begin(), blocks.end(), [](const Block &block) {
                return (block.header.dwFlags & WHDR_PREPARED) != 0u &&
                    (block.header.dwFlags & WHDR_DONE) == 0u;
            }));
    }

    bool begin_block(std::int16_t *&samples) {
        Block &block = blocks[next_block];
        if ((block.header.dwFlags & WHDR_PREPARED) != 0u) {
            if ((block.header.dwFlags & WHDR_DONE) == 0u) return false;
            waveOutUnprepareHeader(device, &block.header, sizeof(WAVEHDR));
        }
        block.samples.resize(AudioMixer::kBlockSamples);
        samples = block.samples.data();
        return true;
    }

    bool commit_block() {
        Block &block = blocks[next_block];
        block.header = WAVEHDR{};
        block.header.lpData = reinterpret_cast<LPSTR>(block.samples.data());
        block.header.dwBufferLength =
            static_cast<DWORD>(block.samples.size() * sizeof(std::int16_t));
        if (waveOutPrepareHeader(device, &block.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            return false;
        if (waveOutWrite(device, &block.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(device, &block.header, sizeof(WAVEHDR));
            return false;
        }
        next_block = (next_block + 1u) % blocks.size();
        return true;
    }

    void pause_playback() { waveOutPause(device); }
    void resume_playback() { waveOutRestart(device); }

    void close_device(bool playback_started) {
        if (!opened || device == nullptr) return;
        if (!playback_started) waveOutRestart(device);
        waveOutReset(device);
        for (Block &block : blocks) {
            if ((block.header.dwFlags & WHDR_PREPARED) != 0u)
                waveOutUnprepareHeader(device, &block.header, sizeof(WAVEHDR));
        }
        waveOutClose(device);
        device = nullptr;
        opened = false;
        blocks.clear();
    }
};

using AudioDevice = WaveOutDevice;

#elif defined(__SWITCH__)

// The Switch: libnx's audout, fed by a thread of our own. SDL's Switch audio
// took one block per pass of its thread; a late pass was a gap the hardware
// filled with silence and a block it never caught up on, so even at a steady
// 30 fps the queue overflowed six times a second (crackle), and once its
// thread stopped for good. Here the hardware's 48 kHz paces the reads: the
// thread waits for audout to hand a buffer back, refills it from the queue
// (silence while the mixer prebuffers) and returns it.

#include <switch.h>

#include "lcs_switch_hooks.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <thread>

namespace lcs {
namespace {

class AudoutDevice {
public:
    [[nodiscard]] const char *backend_name() const noexcept { return "audout"; }

    bool ensure_open(AudioMixer &mixer) {
        if (opened_) return true;
        if (failed_) return false;
        for (Hardware &hardware : hardware_) {
            hardware.memory = static_cast<std::int16_t *>(std::aligned_alloc(kBufferAlign, kBufferBytes));
            if (hardware.memory == nullptr) {
                failed_ = true;
                return false;
            }
            std::memset(hardware.memory, 0, kBufferBytes);
        }
        if (!start_audout()) {
            std::cerr << "[audio-host] audout could not start\n";
            failed_ = true;
            return false;
        }
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        opened_ = true;
        mixer.note_device_opened();
        std::cerr << "[audio-host] audout " << audoutGetSampleRate() << "Hz channels=" << audoutGetChannelCount()
                  << " prebuffer_ms="
                  << (mixer.prebuffer_blocks() * AudioMixer::kBlockFrames * 1000u / AudioMixer::kSampleRate)
                  << "\n";
        return true;
    }

    [[nodiscard]] std::size_t outstanding_blocks() const {
        std::lock_guard lock(mutex_);
        return count_;
    }

    bool begin_block(std::int16_t *&samples) {
        if (outstanding_blocks() >= AudioMixer::kBlockCount) return false;
        samples = staging_.data();
        return true;
    }

    bool commit_block() {
        std::lock_guard lock(mutex_);
        if (count_ >= queue_.size()) return false;
        queue_[(head_ + count_) % queue_.size()] = staging_;
        ++count_;
        return true;
    }

    void pause_playback() { playing_.store(false); }
    void resume_playback() { playing_.store(true); }

    void close_device(bool) {
        if (!opened_) return;
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        audoutStopAudioOut();
        audoutExit();
        for (Hardware &hardware : hardware_) {
            std::free(hardware.memory);
            hardware.memory = nullptr;
        }
        std::lock_guard lock(mutex_);
        head_ = count_ = 0u;
        opened_ = false;
    }

private:
    // One hardware buffer holds one mixer block (512 frames, 2 KiB); audout
    // wants buffers aligned and sized to 4 KiB.
    static constexpr std::size_t kBufferAlign = 0x1000u;
    static constexpr std::size_t kBufferBytes = 0x1000u;
    static constexpr std::size_t kBlockBytes = AudioMixer::kBlockSamples * sizeof(std::int16_t);
    static_assert(kBlockBytes <= kBufferBytes);
    // Buffers handed to audout at once: 4 x 10.7 ms.
    static constexpr std::size_t kHardwareBuffers = 4u;

    struct Hardware {
        AudioOutBuffer buffer{};
        std::int16_t *memory{};
    };

    bool start_audout() {
        if (R_FAILED(audoutInitialize())) return false;
        if (R_FAILED(audoutStartAudioOut())) {
            audoutExit();
            return false;
        }
        for (Hardware &hardware : hardware_) {
            std::memset(hardware.memory, 0, kBufferBytes);
            hardware.buffer = AudioOutBuffer{};
            hardware.buffer.buffer = hardware.memory;
            hardware.buffer.buffer_size = kBufferBytes;
            hardware.buffer.data_size = kBlockBytes;
            if (R_FAILED(audoutAppendAudioOutBuffer(&hardware.buffer))) return false;
        }
        return true;
    }

    // The next queued block into `buffer`, or silence while prebuffering or
    // when the queue is dry (the mixer counts that as a gap and re-buffers).
    void refill(AudioOutBuffer &buffer) {
        auto *out = static_cast<std::int16_t *>(buffer.buffer);
        {
            std::lock_guard lock(mutex_);
            if (playing_.load() && count_ != 0u) {
                std::memcpy(out, queue_[head_].data(), kBlockBytes);
                head_ = (head_ + 1u) % queue_.size();
                --count_;
                buffer.data_size = kBlockBytes;
                return;
            }
        }
        std::memset(out, 0, kBlockBytes);
        buffer.data_size = kBlockBytes;
    }

    void run() {
        lcs_switch_name_current_thread("audio");
        // Above the other threads of its core (GL, draw decoding): its work
        // is short and late is audible. Lower numbers come first; 0x2C is
        // the default.
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
        int idle_waits = 0;
        while (!stop_.load()) {
            AudioOutBuffer *released = nullptr;
            u32 released_count = 0u;
            const Result result = audoutWaitPlayFinish(&released, &released_count, 100'000'000ull);
            if (R_FAILED(result) || released == nullptr || released_count == 0u) {
                // Ten waits (1 s) without a buffer back: audout is stuck;
                // open it again with fresh buffers.
                if (++idle_waits < 10) continue;
                idle_waits = 0;
                ++restarts_;
                std::cerr << "[audio-host] audout stalled; restarting it (" << restarts_ << ")\n";
                audoutStopAudioOut();
                audoutExit();
                if (!start_audout()) svcSleepThread(500'000'000ll);
                continue;
            }
            idle_waits = 0;
            refill(*released);
            audoutAppendAudioOutBuffer(released);
            // More may have come back while this one was filled.
            for (;;) {
                AudioOutBuffer *more = nullptr;
                u32 more_count = 0u;
                if (R_FAILED(audoutGetReleasedAudioOutBuffer(&more, &more_count)) || more == nullptr ||
                    more_count == 0u)
                    break;
                refill(*more);
                audoutAppendAudioOutBuffer(more);
            }
        }
    }

    std::array<Hardware, kHardwareBuffers> hardware_{};
    std::array<std::array<std::int16_t, AudioMixer::kBlockSamples>, AudioMixer::kBlockCount> queue_{};
    std::array<std::int16_t, AudioMixer::kBlockSamples> staging_{};
    std::size_t head_{};
    std::size_t count_{};
    mutable std::mutex mutex_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::uint64_t restarts_{};
    bool opened_{};
    bool failed_{};
};

using AudioDevice = AudoutDevice;

#else

#include <SDL.h>

namespace lcs {
namespace {

struct SdlAudioDevice {
    SDL_AudioDeviceID device{};
    std::vector<std::int16_t> block;
    bool opened{};
    bool failed{};

    [[nodiscard]] const char *backend_name() const noexcept { return "SDL"; }

    bool ensure_open(AudioMixer &mixer) {
        if (opened) return true;
        if (failed) return false;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::cerr << "[audio-host] SDL_InitSubSystem failed: " << SDL_GetError() << "\n";
            failed = true;
            return false;
        }
        SDL_AudioSpec want{};
        want.freq = static_cast<int>(AudioMixer::kSampleRate);
        want.format = AUDIO_S16SYS;
        want.channels = static_cast<Uint8>(AudioMixer::kOutputChannels);
        want.samples = static_cast<Uint16>(AudioMixer::kBlockFrames);
        SDL_AudioSpec got{};
        device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (device == 0) {
            std::cerr << "[audio-host] SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
            failed = true;
            return false;
        }
        SDL_PauseAudioDevice(device, 1);
        block.resize(AudioMixer::kBlockSamples);
        opened = true;
        mixer.note_device_opened();
        std::cerr << "[audio-host] SDL " << got.freq << "Hz channels="
                  << static_cast<int>(got.channels) << " prebuffer_ms="
                  << (mixer.prebuffer_blocks() * AudioMixer::kBlockFrames * 1000u /
                      AudioMixer::kSampleRate) << "\n";
        return true;
    }

    [[nodiscard]] std::size_t outstanding_blocks() const {
        if (device == 0) return 0u;
        return SDL_GetQueuedAudioSize(device) /
            (AudioMixer::kBlockSamples * sizeof(std::int16_t));
    }

    bool begin_block(std::int16_t *&samples) {
        if (outstanding_blocks() >= AudioMixer::kBlockCount) return false;
        if (block.size() != AudioMixer::kBlockSamples) block.resize(AudioMixer::kBlockSamples);
        samples = block.data();
        return true;
    }

    bool commit_block() {
        return SDL_QueueAudio(device, block.data(),
                              static_cast<Uint32>(AudioMixer::kBlockSamples * sizeof(std::int16_t))) == 0;
    }

    void pause_playback() { SDL_PauseAudioDevice(device, 1); }
    void resume_playback() { SDL_PauseAudioDevice(device, 0); }

    void close_device(bool) {
        if (!opened || device == 0) return;
        SDL_PauseAudioDevice(device, 1);
        SDL_ClearQueuedAudio(device);
        SDL_CloseAudioDevice(device);
        device = 0;
        opened = false;
        block.clear();
    }
};

using AudioDevice = SdlAudioDevice;

#endif

struct AudioHost {
    std::mutex mutex;
    AudioMixer mixer;
    AudioDevice device;
};

AudioHost &audio_host() {
    static AudioHost host;
    return host;
}

}

bool audio_output_enabled() {
    static const bool enabled = [] {
        if (const char *text = std::getenv("PSPRECOMP_AUDIO"))
            return *text != '\0' && std::string(text) != "0";
        return true;
    }();
    return enabled;
}

void audio_output_submit(std::span<const std::int16_t> pcm, std::uint32_t frames,
                         bool stereo, std::uint32_t left, std::uint32_t right,
                         std::uint32_t source_rate, std::uint32_t channel,
                         std::uint64_t start_time_us, std::uint64_t now_us) {
    if (!audio_output_enabled()) return;
    AudioHost &host = audio_host();
    std::lock_guard<std::mutex> guard(host.mutex);
    host.mixer.submit(pcm, frames, stereo, left, right, source_rate, channel,
                      start_time_us, now_us, host.device);
}

AudioOutputStats audio_output_stats() {
    AudioHost &host = audio_host();
    std::lock_guard<std::mutex> guard(host.mutex);
    return AudioOutputStats{host.mixer.underruns(), host.mixer.dropped_blocks()};
}

void audio_output_advance(std::uint64_t guest_time_us) {
    if (!audio_output_enabled()) return;
    AudioHost &host = audio_host();
    std::lock_guard<std::mutex> guard(host.mutex);
    host.mixer.advance(guest_time_us, host.device);
}

void audio_output_reset_channel(std::uint32_t channel) {
    AudioHost &host = audio_host();
    std::lock_guard<std::mutex> guard(host.mutex);
    host.mixer.reset_channel(channel);
}

void audio_output_shutdown() {
    AudioHost &host = audio_host();
    std::lock_guard<std::mutex> guard(host.mutex);
    host.mixer.shutdown(host.device);
}

}
