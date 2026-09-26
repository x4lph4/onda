#include "AudioEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include "../config/Config.h"
#include "../log/Log.h"
#include "core/adevice/AudioTestTool.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/engine/DeckCommand.h"

namespace onda::audio {

AudioEngine::AudioEngine(Config& config, DecoderFactory decoderFactory)
    : config_(config) {
    mixerCtl_.setSink([this](uint16_t id, float v) {
        commandQueue_.tryPush(rt::SetParam{ id, v });
    });
    mixerCtl_.setFxLoader([this](int t, int s, int k) {
        // non-RT: build+park the processor, then reclaim any retired one
        mixer_.fx().loadEffect(t, s, (fx::FXKind)k);
        mixer_.fx().collectTrash();
    });
    mixerCtl_.setFxMover([this](int t, int from, int to) {
        mixer_.fx().moveSlot(t, from, to);
    });
    mixerCtl_.setSourceSink([this](int ch, mixer::ChSource src) {
        mixer_.setChannelSource(ch, src);
    });
    mixerCtl_.setDeckModeSink([this](int n) {
        mixer_.setDeckMode(n);
    });
    for (int i = 0; i < kNumDecks; ++i) {
        auto& d = decks_[(size_t)i];
        d.bind(&deckStats_[(size_t)i]);
        d.setStretcher(std::make_unique<WsolaStretcher>());
        mixer_.channel(i).bindStats(&deckStats_[(size_t)i]);
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
    const double sr = sampleRate_.load();
    const double rate = sr > 0.0 ? sr : 48000.0;
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
    const double sr = device->getCurrentSampleRate();
    sampleRate_.store(sr);
    for (auto& d : decks_)
        d.setDeviceRate(sr);
    for (auto& d : decks_)
        d.prepareDsp(sr);
    mixer_.prepare(sr);            // strips, xf, master, phones, FX
    if (sampler_) sampler_->prepare(sr);
    stats_.sampleRate.store(sr, std::memory_order_relaxed);
    stats_.bufferSize.store(device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
    stats_.running.store(true, std::memory_order_relaxed);
    stats_.xrunCount.store(0, std::memory_order_relaxed);
    stats_.clipCount.store(0, std::memory_order_relaxed);
    stats_.maxCallbackMs.store(0.0f, std::memory_order_relaxed);
    haveLastCb_ = false;
    smoothedLoad_ = 0.0f;
    log::info("Audio device started: " + device->getName()
              + " @ " + juce::String(sr) + " Hz, "
              + juce::String(device->getCurrentBufferSizeSamples()) + " smp");
}

void AudioEngine::audioDeviceStopped() {
    stats_.running.store(false, std::memory_order_relaxed);
    log::warn("Audio device stopped");
}

void AudioEngine::audioDeviceError(const juce::String& /*errorMessage*/) {
    // may arrive on a driver-adjacent thread — atomics only, no logging
    stats_.deviceErrors.fetch_add(1, std::memory_order_relaxed);
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

    // Sync pre-pass: computes each follower's tempo/phase correction for
    // this block (master phase predicted one block ahead so deck render
    // order is irrelevant). Pure math on RT-owned state.
    syncRt_.apply(decks_.data(), (int)decks_.size(), numSamples);

    // FX tempo source for sends/master: the sync master deck
    mixer_.setFxSyncMaster(syncRt_.masterDeck >= 0 ? syncRt_.masterDeck : 0);

    // sampler clock: master deck's beat phase feeds quantized triggers
    // + sync'd sample speed. Stopped/missing master → hold (bpm<=0).
    if (sampler_) {
        const int m = syncRt_.masterDeck;
        if (m >= 0 && m < kNumDecks) {
            const auto& ds = deckStats_[(size_t)m];
            sampler_->setClock(ds.effBpm.load(), ds.beatPos.load());
        } else {
            sampler_->setClock(0.0, 0.0);
        }
    }

    mixer_.render(outputChannelData, numOutputChannels, numSamples,
                  decks_.data(), (int)decks_.size(), stats_, sampler_);

    // recording tap — post-master program into the SPSC ring (bounded
    // copy + atomics only; the writer thread does all disk IO)
    if (recorder_)
        recorder_->pushPostMaster(mixer_.masterL(), mixer_.masterR(),
                                  numSamples);

    // routing-test signal — replaces program audio on the mapped outs
    const int tm = testMode_.load(std::memory_order_relaxed);
    if (tm != 0) {
        adevice::ChannelMapping m;
        m.masterL = mixer_.outMap(0); m.masterR = mixer_.outMap(1);
        m.cueL    = mixer_.outMap(2); m.cueR    = mixer_.outMap(3);
        adevice::AudioTestTool::render(
            (adevice::TestSignal)tm, m,
            outputChannelData, numOutputChannels, numSamples,
            sampleRate_.load(std::memory_order_relaxed), testPhase_);
    }

    // ---- diagnostics (atomics only) ----
    const double sr_ = sampleRate_.load();
    const double periodSec = sr_ > 0.0 ? numSamples / sr_ : 0.0;

    if (haveLastCb_ && periodSec > 0.0) {
        const double gapSec = std::chrono::duration<double>(start - lastCbStart_).count();
        if (gapSec > periodSec * 1.75)
            stats_.xrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    lastCbStart_ = start;
    haveLastCb_ = true;

    const auto end = std::chrono::steady_clock::now();
    const double durSec = std::chrono::duration<double>(end - start).count();
    if (periodSec > 0.0 && durSec > periodSec)  // over-budget = underrun
        stats_.underrunCount.fetch_add(1, std::memory_order_relaxed);
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
    std::visit([this, &cmd](const auto& c) {
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
        // channel-strip params (mixer-side since Phase 5)
        else if constexpr (std::is_same_v<T, rt::SetDeckGain>)
            mixer_.channel(deck).setInputGain(c.gain);
        else if constexpr (std::is_same_v<T, rt::SetEqBand>)
            mixer_.channel(deck).eq().setGainDb(c.band, c.db);
        else if constexpr (std::is_same_v<T, rt::SetFilter>)
            mixer_.channel(deck).filter().setPosition(c.pos);
        else if constexpr (std::is_same_v<T, rt::SetParam>)
            mixer_.setParam(c.id, c.v);
        else if constexpr (std::is_same_v<T, rt::SyncSetMaster>)
            syncRt_.masterDeck = (c.deck >= kNumDecks) ? -1 : (int)c.deck;
        else if constexpr (std::is_same_v<T, rt::SampleTrigger> ||
                           std::is_same_v<T, rt::SampleRelease> ||
                           std::is_same_v<T, rt::SampleStop> ||
                           std::is_same_v<T, rt::SampleSlotParam> ||
                           std::is_same_v<T, rt::SamplerParam>) {
            if (sampler_) sampler_->applyCommand(cmd);
        }
        else if (deck < 0 || deck >= kNumDecks) { /* invalid deck */ }
        else applyDeckCommand(decks_[(size_t)deck], deck, c);
    }, cmd);
}

} // namespace onda::audio
