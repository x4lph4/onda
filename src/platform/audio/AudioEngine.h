#pragma once

#include <array>
#include <chrono>
#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>

#include "core/engine/AudioFileStreamer.h"
#include "core/engine/DeckEngine.h"
#include "core/engine/MixerEngine.h"
#include "core/rt/AudioStats.h"
#include "core/rt/CommandQueue.h"

namespace onda { class Config; }

namespace onda::audio {

/// Real-time audio engine. Owns the device manager, the RT callback path,
/// the deck array and the streamer threads feeding them.
/// RT rules inside the callback: no allocation, no locks, no syscalls,
/// no logging, no exceptions escaping. All state is preallocated.
class AudioEngine : private juce::AudioIODeviceCallback {
public:
    static constexpr int kNumDecks = 4; // engine is 4-deck from day one; UI shows 2

    /// decoderFactory creates decoders for the streamer threads (e.g. ffmpeg).
    AudioEngine(Config& config, DecoderFactory decoderFactory);
    ~AudioEngine() override;

    /// Opens the audio device (restoring the last persisted setup) and
    /// starts the RT callback. Call from the UI thread only.
    void start();
    void shutdown();

    /// UI thread: ask a deck's streamer to load a file.
    void loadTrack(int deck, const juce::String& path);
    void unloadTrack(int deck);

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }
    const rt::AudioStats&   stats()          noexcept { return stats_; }
    rt::CommandQueue&       commands()       noexcept { return commandQueue_; }
    DeckStats&              deckStats(int i) noexcept { return streamers_[(size_t)i]->stats(); }

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

    std::array<std::unique_ptr<AudioFileStreamer>, kNumDecks> streamers_;
    std::array<DeckEngine, kNumDecks> decks_;
    MixerEngine mixer_;

    // ---- state owned by the RT thread ----
    float masterGain_     { 1.0f };
    float cueMix_         { 0.5f };
    bool  limiterEnabled_ { true };
    double sampleRate_    { 0.0 };
    float smoothedLoad_   { 0.0f };
    std::chrono::steady_clock::time_point lastCbStart_ {};
    bool haveLastCb_ { false };
    bool started_    { false };
};

} // namespace onda::audio
