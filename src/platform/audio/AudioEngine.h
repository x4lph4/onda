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
    /// Non-RT wiring accessor (grid handoff slots). Never call on RT path
    /// other than through commands.
    DeckEngine*             deckRaw(int i)   noexcept { return &decks_[(size_t)i]; }

private:
    // ---- AudioIODeviceCallback (RT thread) ----
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

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

    // ---- state owned by the RT thread ----
    double sampleRate_    { 0.0 };
    float smoothedLoad_   { 0.0f };
    std::chrono::steady_clock::time_point lastCbStart_ {};
    bool haveLastCb_ { false };
    bool started_    { false };
};

} // namespace onda::audio
