#include "AudioEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include "../config/Config.h"
#include "../log/Log.h"
#include "core/dsp/WsolaStretcher.h"

namespace onda::audio {

AudioEngine::AudioEngine(Config& config, DecoderFactory decoderFactory)
    : config_(config) {
    for (int i = 0; i < kNumDecks; ++i) {
        auto& d = decks_[(size_t)i];
        d.bind(&deckStats_[(size_t)i]);
        d.setStretcher(std::make_unique<WsolaStretcher>());
        loaders_[(size_t)i] = std::make_unique<TrackLoader>(
            decoderFactory, &deckStats_[(size_t)i],
            &d.pendingTrack_, &d.retiredTrack_);
    }
}

AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::start() {
    std::unique_ptr<juce::XmlElement> saved;
    const auto xmlString = config_.audioDeviceStateXml();
    if (xmlString.isNotEmpty())
        saved = juce::XmlDocument::parse(xmlString);

    const auto err = deviceManager_.initialise(0, 4, saved.get(), true);
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
    if (auto xml = deviceManager_.createStateXml())
        config_.setAudioDeviceStateXml(xml->toString());
    else
        log::info("No explicit device selection to persist (defaults in use)");
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
    log::info("Audio engine stopped");
}

void AudioEngine::loadTrack(int deck, const juce::String& path) {
    if (deck < 0 || deck >= kNumDecks)
        return;
    const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    loaders_[(size_t)deck]->requestLoad(path.toStdString(), rate);
    log::info("Deck " + juce::String(deck) + " loading: " + path);
}

void AudioEngine::unloadTrack(int deck) {
    commandQueue_.tryPush(rt::DeckUnload{(uint8_t)deck});
}

void AudioEngine::cancelLoad(int deck) {
    if (deck >= 0 && deck < kNumDecks)
        loaders_[(size_t)deck]->cancel();
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    sampleRate_ = device->getCurrentSampleRate();
    for (auto& d : decks_)
        d.setDeviceRate(sampleRate_);
    for (auto& d : decks_)
        d.prepareDsp(sampleRate_);
    mixer_.prepare(sampleRate_);
    stats_.sampleRate.store(sampleRate_, std::memory_order_relaxed);
    stats_.bufferSize.store(device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
    stats_.running.store(true, std::memory_order_relaxed);
    stats_.xrunCount.store(0, std::memory_order_relaxed);
    stats_.clipCount.store(0, std::memory_order_relaxed);
    stats_.maxCallbackMs.store(0.0f, std::memory_order_relaxed);
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
                  decks_.data(), (int)decks_.size(), stats_);

    // ---- diagnostics (atomics only) ----
    const double periodSec = sampleRate_ > 0.0 ? numSamples / sampleRate_ : 0.0;

    if (haveLastCb_ && periodSec > 0.0) {
        const double gapSec = std::chrono::duration<double>(start - lastCbStart_).count();
        if (gapSec > periodSec * 1.75)
            stats_.xrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    lastCbStart_ = start;
    haveLastCb_ = true;

    const auto end = std::chrono::steady_clock::now();
    const double durSec = std::chrono::duration<double>(end - start).count();
    stats_.lastCallbackMs.store((float)(durSec * 1000.0), std::memory_order_relaxed);
    float prev = stats_.maxCallbackMs.load(std::memory_order_relaxed);
    while ((float)(durSec * 1000.0) > prev &&
           !stats_.maxCallbackMs.compare_exchange_weak(prev, (float)(durSec * 1000.0),
                                                       std::memory_order_relaxed)) {}

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
        const int deck = [&] { if constexpr (requires { c.deck; }) return (int)c.deck;
                               else return 0; }();

        if constexpr (std::is_same_v<T, rt::SetMasterGain>)   mixer_.setMasterGain(c.gain);
        else if constexpr (std::is_same_v<T, rt::SetLimiterOn>)   mixer_.setLimiterEnabled(c.enabled);
        else if constexpr (std::is_same_v<T, rt::SetCueVol>)      mixer_.setCueVol(c.vol);
        else if constexpr (std::is_same_v<T, rt::SetCueMix>)      mixer_.setCueMix(c.mix);
        else if constexpr (std::is_same_v<T, rt::SetCrossfader>)  mixer_.setCrossfader(c.pos);
        else if constexpr (std::is_same_v<T, rt::SetXfCurve>)     mixer_.setXfCurve(c.curve);
        else if constexpr (std::is_same_v<T, rt::SetPfl>)      mixer_.setPfl(deck, c.on);
        else if constexpr (std::is_same_v<T, rt::SetFader>)    mixer_.setFader(deck, c.gain);
        else if (deck < 0 || deck >= kNumDecks) { /* invalid deck */ }
        else if constexpr (std::is_same_v<T, rt::DeckTransport>) {
            auto& d = decks_[(size_t)deck];
            if (c.action == 1) d.setPlaying(true);
            else if (c.action == 0) d.setPlaying(false);
            else d.stop();
        }
        else if constexpr (std::is_same_v<T, rt::DeckSeek>)     decks_[(size_t)deck].seekSec(c.sec);
        else if constexpr (std::is_same_v<T, rt::DeckUnload>)   decks_[(size_t)deck].unloadTrack();
        else if constexpr (std::is_same_v<T, rt::SetDeckGain>)  decks_[(size_t)deck].setGain(c.gain);
        else if constexpr (std::is_same_v<T, rt::SetTrackGain>) decks_[(size_t)deck].setTrackGain(c.gain);
        else if constexpr (std::is_same_v<T, rt::SetPitch>)     decks_[(size_t)deck].setPitchPercent(c.pct);
        else if constexpr (std::is_same_v<T, rt::SetPitchRange>)decks_[(size_t)deck].setPitchRange(c.pct);
        else if constexpr (std::is_same_v<T, rt::SetPitchBend>) decks_[(size_t)deck].setPitchBend(c.pct);
        else if constexpr (std::is_same_v<T, rt::SetKeyLock>)   decks_[(size_t)deck].setKeyLock(c.on);
        else if constexpr (std::is_same_v<T, rt::SetReverse>)   decks_[(size_t)deck].setReverse(c.on);
        else if constexpr (std::is_same_v<T, rt::SetQuantize>)  decks_[(size_t)deck].setQuantize(c.on);
        else if constexpr (std::is_same_v<T, rt::SetLoop>)      decks_[(size_t)deck].setLoop(c.inSec, c.outSec);
        else if constexpr (std::is_same_v<T, rt::SetLoopOn>)    decks_[(size_t)deck].setLoopEnabled(c.on);
        else if constexpr (std::is_same_v<T, rt::SetLoopSize>)  decks_[(size_t)deck].setLoopSizeSec(c.sizeSec);
        else if constexpr (std::is_same_v<T, rt::SetEqBand>)    decks_[(size_t)deck].setEqDb(c.band, c.db);
        else if constexpr (std::is_same_v<T, rt::SetFilter>)    decks_[(size_t)deck].setFilterPos(c.pos);
    }, cmd);
}

} // namespace onda::audio
