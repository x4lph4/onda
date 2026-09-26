#pragma once

#include <array>
#include <chrono>
#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>

#include "core/engine/DeckEngine.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerEngine.h"
#include "core/engine/TrackLoader.h"
#include "core/model/MasterStats.h"
#include "core/performance/SyncRt.h"
#include "core/rt/AudioStats.h"
#include "core/rt/CommandQueue.h"
#include "core/sampler/SamplerRt.h"
#include "core/rec/RecordingBuffer.h"
#include "core/adevice/AudioDeviceTypes.h"

namespace onda { class Config; }

namespace onda::audio {

/// Real-time audio engine: device manager + RT callback + deck array +
/// loader threads + mixer. RT rules inside the callback: no allocation,
/// no locks, no syscalls, no logging, no exceptions escaping.
class AudioEngine : private juce::AudioIODeviceCallback {
public:
    static constexpr int kNumDecks = MixerEngine::kMaxDecks;

    /// decoderFactory creates decoders for the loader threads (e.g. ffmpeg).
    AudioEngine(Config& config, DecoderFactory decoderFactory);
    ~AudioEngine() override;

    /// Opens the audio device (restoring the last persisted setup) and
    /// starts the RT callback. UI thread only.
    void start();
    void shutdown();

    /// UI thread: ask a deck's loader to decode a file (async).
    void loadTrack(int deck, const juce::String& path);
    void unloadTrack(int deck);
    void cancelLoad(int deck);

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }
    const rt::AudioStats&   stats()          noexcept { return stats_; }
    rt::CommandQueue&       commands()       noexcept { return commandQueue_; }
    /// Validated mixer parameter facade (UI / MIDI / HID / automation).
    /// Non-RT; each setParam() is sanitized by ParameterRegistry and
    /// queued to the audio thread as rt::SetParam.
    mixer::MixerController& mixerCtl()       noexcept { return mixerCtl_; }
    const MasterStats&      masterStats()    noexcept { return mixer_.masterStats(); }
    DeckStats&              deckStats(int i) noexcept { return deckStats_[(size_t)i]; }
    /// FX engine access (racks/events/meters). Effect loading via
    /// mixerCtl().setFxType(); call fxHousekeeping() periodically from
    /// a control thread to reclaim retired processors.
    fx::FXEngine&           fxEngine()       noexcept { return mixer_.fx(); }
    void                    fxHousekeeping() noexcept { mixer_.fxHousekeeping(); }
    /// Phase 17: mixer access for diagnostics/scenes (control thread).
    MixerEngine&            mixerEngine()    noexcept { return mixer_; }
    const MixerEngine&      mixerEngine() const noexcept { return mixer_; }
    /// Non-RT wiring accessor (grid handoff slots). Never call on RT path
    /// other than through commands.
    DeckEngine*             deckRaw(int i)   noexcept { return &decks_[(size_t)i]; }

    /// Phase 11: attach the sampler's RT engine — renders into mixer
    /// channel 4 as a first-class aux source (non-RT, call at startup).
    void attachSampler(sampler::SamplerRt* s) noexcept {
        sampler_ = s;
        mixer_.setActiveChannels(s ? MixerEngine::kSamplerChannel + 1
                                   : 4);
    }
    sampler::SamplerRt* sampler() noexcept { return sampler_; }

    /// Phase 12: attach the recorder's RT tap — post-master program
    /// is pushed into its SPSC ring after mixer_.render() each block.
    /// Non-RT; wire at startup before recording.
    void attachRecorder(rec::RecorderRt* r) noexcept { recorder_ = r; }
    rec::RecorderRt*    recorder() noexcept { return recorder_; }
    /// Real device sample rate — written by audioDeviceAboutToStart.
    double sampleRate() const noexcept { return sampleRate_.load(); }
    bool   running()    const noexcept { return started_; }

    /// Phase 13: physical output routing (master/cue stereo pairs onto
    /// device channel indices). Non-RT; the mixer reads atomics.
    void setOutputMap(int mL, int mR, int cL, int cR) noexcept {
        mixer_.setOutputMap(mL, mR, cL, cR);
    }
    /// Phase 13: routing-test signal (adevice::TestSignal). Replaces
    /// program audio on the mapped outs while active. Non-RT set.
    void setTestSignal(int mode) noexcept {
        testMode_.store(mode, std::memory_order_relaxed);
        if (mode == 0) testPhase_ = 0;
    }

private:
    // ---- AudioIODeviceCallback (RT thread) ----
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

    void applyCommand(const rt::Command& cmd) noexcept;

    Config& config_;
    juce::AudioDeviceManager deviceManager_;
    rt::AudioStats stats_;
    rt::CommandQueue commandQueue_;

    std::array<DeckStats, kNumDecks> deckStats_;
    std::array<std::unique_ptr<TrackLoader>, kNumDecks> loaders_;
    std::array<DeckEngine, kNumDecks> decks_;
    MixerEngine mixer_;
    mixer::MixerController mixerCtl_;
    SyncRtCore  syncRt_;
    sampler::SamplerRt* sampler_ = nullptr;  // borrowed; owned by app
    rec::RecorderRt*    recorder_ = nullptr; // borrowed; owned by app

    // ---- state owned by the RT thread ----
    std::atomic<double> sampleRate_   { 0.0 };
    float smoothedLoad_   { 0.0f };
    std::chrono::steady_clock::time_point lastCbStart_ {};
    bool haveLastCb_ { false };
    bool started_    { false };
    std::atomic<int> testMode_ { 0 };   // adevice::TestSignal, non-RT set
    uint64_t         testPhase_ { 0 };  // RT-owned continuity
};

} // namespace onda::audio
