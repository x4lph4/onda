#include "AudioEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include "../config/Config.h"
#include "../log/Log.h"

namespace onda::audio {

AudioEngine::AudioEngine(Config& config, DecoderFactory decoderFactory)
    : config_(config) {
    for (auto& s : streamers_)
        s = std::make_unique<AudioFileStreamer>(decoderFactory);
    for (int i = 0; i < kNumDecks; ++i)
        decks_[(size_t)i].bind(&streamers_[(size_t)i]->fifo(),
                               &streamers_[(size_t)i]->stats());
}

AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::start() {
    std::unique_ptr<juce::XmlElement> saved;
    const auto xmlString = config_.audioDeviceStateXml();
    if (xmlString.isNotEmpty())
        saved = juce::XmlDocument::parse(xmlString);

    const auto err = deviceManager_.initialise(0, 2, saved.get(), true);
    if (err.isNotEmpty())
        log::error("Audio device init failed: " + err);

    deviceManager_.addAudioCallback(this);
    started_ = true;
    log::info("Audio engine started");
}

void AudioEngine::shutdown() {
    if (!started_)
        return;
    started_ = false;
    // Capture state before removing the callback: the device manager stops
    // the device when its last callback goes away.
    if (auto xml = deviceManager_.createStateXml()) {
        const auto str = xml->toString();
        config_.setAudioDeviceStateXml(str);
    } else {
        log::info("No explicit device selection to persist (defaults in use)");
    }
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
    log::info("Audio engine stopped");
}

void AudioEngine::loadTrack(int deck, const juce::String& path) {
    if (deck < 0 || deck >= kNumDecks)
        return;
    const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    streamers_[(size_t)deck]->requestLoad(path.toStdString(), rate);
    log::info("Deck " + juce::String(deck) + " loading: " + path);
}

void AudioEngine::unloadTrack(int deck) {
    if (deck < 0 || deck >= kNumDecks)
        return;
    streamers_[(size_t)deck]->requestUnload();
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    sampleRate_ = device->getCurrentSampleRate();
    for (auto& d : decks_)
        d.setDeviceRate(sampleRate_);
    stats_.sampleRate.store(sampleRate_, std::memory_order_relaxed);
    stats_.bufferSize.store(device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
    stats_.running.store(true, std::memory_order_relaxed);
    stats_.xrunCount.store(0, std::memory_order_relaxed);
    stats_.clipCount.store(0, std::memory_order_relaxed);
    haveLastCb_ = false;
    smoothedLoad_ = 0.0f;
    log::info("Audio device started: " + device->getName()
              + " @ " + juce::String(sampleRate_) + " Hz, "
              + juce::String(device->getCurrentBufferSizeSamples()) + " smp");
}

void AudioEngine::audioDeviceStopped() {
    stats_.running.store(false, std::memory_order_relaxed);
    log::warn("Audio device stopped");
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* /*inputChannelData*/, int /*numInputChannels*/,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& /*context*/) {

    const auto start = std::chrono::steady_clock::now();

    // Drain UI -> RT commands. Bounded, non-blocking.
    rt::Command cmd;
    while (commandQueue_.tryPop(cmd))
        applyCommand(cmd);

    mixer_.render(outputChannelData, numOutputChannels, numSamples,
                  decks_.data(), decks_.size(), stats_);

    // ---- diagnostics (atomics only) ----
    const double periodSec = sampleRate_ > 0.0 ? numSamples / sampleRate_ : 0.0;

    if (haveLastCb_ && periodSec > 0.0) {
        const double gapSec = std::chrono::duration<double>(start - lastCbStart_).count();
        // A gap well above one period means the driver starved the RT thread.
        if (gapSec > periodSec * 1.75)
            stats_.xrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    lastCbStart_ = start;
    haveLastCb_ = true;

    const auto end = std::chrono::steady_clock::now();
    const double durSec = std::chrono::duration<double>(end - start).count();
    stats_.lastCallbackMs.store((float)(durSec * 1000.0), std::memory_order_relaxed);

    if (periodSec > 0.0) {
        const float load = (float)(durSec / periodSec);
        smoothedLoad_ += 0.05f * (load - smoothedLoad_);
        stats_.cpuLoad.store(smoothedLoad_, std::memory_order_relaxed);
    }
    stats_.callbackCount.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::applyCommand(const rt::Command& cmd) noexcept {
    std::visit([this](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, rt::SetMasterGain>)
            masterGain_ = c.gain;
        else if constexpr (std::is_same_v<T, rt::SetCueMix>)
            cueMix_ = c.mix;
        else if constexpr (std::is_same_v<T, rt::SetLimiterOn>) {
            limiterEnabled_ = c.enabled;
            mixer_.setLimiterEnabled(c.enabled);
        }
        else if constexpr (std::is_same_v<T, rt::DeckTransport>) {
            if (c.deck < kNumDecks)
                decks_[c.deck].setPlaying(c.playing);
        }
        else if constexpr (std::is_same_v<T, rt::SetDeckGain>) {
            if (c.deck < kNumDecks)
                decks_[c.deck].setGain(c.gain);
        }
    }, cmd);
    mixer_.setMasterGain(masterGain_);
}

} // namespace onda::audio
