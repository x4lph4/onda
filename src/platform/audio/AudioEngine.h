#pragma once

#include <chrono>

#include <juce_audio_devices/juce_audio_devices.h>

#include "core/rt/AudioStats.h"
#include "core/rt/CommandQueue.h"

namespace onda { class Config; }

namespace onda::audio {

/// Real-time audio engine. Owns the device manager and the RT callback path.
/// RT rules inside the callback: no allocation, no locks, no syscalls,
/// no logging, no exceptions escaping. All state is preallocated.
class AudioEngine : private juce::AudioIODeviceCallback {
public:
    explicit AudioEngine(Config& config);
    ~AudioEngine() override;

    /// Opens the audio device (restoring the last persisted setup) and
    /// starts the RT callback. Call from the UI thread only.
    void start();
    void shutdown();

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }
    const rt::AudioStats&   stats()          noexcept { return stats_; }
    rt::CommandQueue&       commands()       noexcept { return commandQueue_; }

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

    // ---- state owned by the RT thread ----
    float masterGain_     { 1.0f };
    float cueMix_         { 0.5f };
    bool  limiterEnabled_ { true };
    double sampleRate_    { 0.0 };
    float smoothedLoad_   { 0.0f };
    std::chrono::steady_clock::time_point lastCbStart_ {};
    bool haveLastCb_ { false };
    bool started_ { false };
};

} // namespace onda::audio
