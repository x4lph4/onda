#include "EngineBridge.h"

#include "core/analysis/AnalysisTypes.h"
#include "core/analysis/KeyAnalyzer.h"
#include "core/mixer/ParameterRegistry.h"

namespace onda::ui {

EngineBridge::EngineBridge(audio::AudioEngine& engine,
                           PerformanceEngine& perf,
                           AnalysisService& analysis,
                           library::MusicLibraryEngine& lib,
                           CommandManager& cmds,
                           NotificationCenter& notes,
                           sampler::SamplerEngine* sampler,
                           rec::RecordingEngine*   recorder,
                           adevice::AudioDeviceEngine* adevice)
    : engine_(engine), perf_(perf), analysis_(analysis), lib_(lib),
      notes_(notes), recorder_(recorder), adevice_(adevice) {
    ports_.queue  = &engine_.commands();
    ports_.mixer  = &engine_.mixerCtl();
    ports_.perf   = &perf_;
    ports_.lib    = &lib_;
    ports_.sampler = sampler;
    ports_.recorder = recorder;
    ports_.deckStats = [this](int d) -> DeckStats* {
        return d >= 0 && d < audio::AudioEngine::kNumDecks
             ? &engine_.deckStats(d) : nullptr;
    };
    ports_.loadTrack = [this](int d, const std::string& path) {
        deckPath_[d & 3] = path;
        engine_.loadTrack(d, juce::String(path));
        // queue analysis so the deck gets its grid/waveform ASAP
        if (lib_.isOpen()) {
            library::Track t;
            if (lib_.tracks().getByPath(path, t))
                analysis_.analyzeTrack(path, 10);
            else
                analysis_.analyzeTrack(path, 10);
        }
        notifyTrackLoaded(d, path);
    };
    ports_.unloadTrack = [this](int d) {
        engine_.unloadTrack(d);
        deckPath_[d & 3].clear();
    };
    // app-level actions are bound by AppShell after construction
    registerEngineCommands(cmds, ports_);

    // Phase 11: domain pad layer — pad.press/pad.mode commands route
    // through CommandManager; sampler actions reach the engine
    // through its trigger/release/stop facade (rt queue inside).
    if (sampler) {
        pads_.setPorts({
            [sampler](int slot, float vel) { sampler->trigger(slot, vel); },
            [sampler](int slot)            { sampler->release(slot); },
            [sampler](int slot)            { sampler->stop(slot); },
        });
        pads_.registerCommands(cmds);
    }

    // Phase 12: recorder ports — deck play edges feed session_tracks,
    // track stops feed the library HistoryManager, and the recorder
    // reads the real device rate (never resamples).
    if (recorder) {
        rec::RecordingEngine::Ports rp;
        rp.notify = [this](std::string t, std::string d, int lvl) {
            if      (lvl == 1) notes_.warn(t, d);
            else if (lvl == 2) notes_.error(t, d);
            else if (lvl == 3) notes_.success(t, d);
            else               notes_.info(t, d);
        };
        rp.deckPlaying = [this](int d) -> int {
            if (d < 0 || d >= audio::AudioEngine::kNumDecks) return -1;
            return engine_.deckStats(d).state.load()
                == (int)DeckState::Playing ? 1 : 0;
        };
        rp.deckPath = [this](int d) -> std::string {
            return d >= 0 && d < 4 ? deckPath_[d] : std::string{};
        };
        rp.trackMeta = [this](int d,
                              const std::string& path) -> rec::TrackPlay {
            rec::TrackPlay t; t.deck = d; t.path = path;
            if (lib_.isOpen()) {
                library::Track lt;
                if (lib_.tracks().getByPath(path, lt)) {
                    t.trackId = lt.id;
                    t.title = lt.title.empty() ? lt.filename : lt.title;
                    t.artist = lt.artist;
                    t.bpm = lt.bpm;
                    if (lt.keyIndex >= 0)
                        t.key = KeyAnalyzer::camelotName(lt.keyIndex);
                }
            }
            return t;
        };
        rp.recordPlay = [this](int64_t trackId, int deck, int64_t ts,
                               double secs) {
            if (lib_.isOpen() && trackId >= 0)
                lib_.history().recordPlay(trackId, deck, ts, secs);
        };
        rp.sampleRate = [this] { return engine_.sampleRate(); };
        rp.audioRunning = [this] { return engine_.running(); };
        recorder->setPorts(std::move(rp));
        engine_.attachRecorder(&recorder->rt());
    }
}

void EngineBridge::snapshotDeck(int deck, DeckSnapshot& o) {
    const DeckStats& s = engine_.deckStats(deck);
    o.state        = s.state.load();
    o.durationSec  = s.durationSec.load();
    o.positionSec  = s.positionSec.load();
    o.loadProgress = s.loadProgress.load();
    o.levelL       = s.levelL.load();
    o.levelR       = s.levelR.load();
    o.pitchPct     = s.pitchPct.load();
    o.pitchRange   = s.pitchRange.load();
    o.bendPct      = s.bendPct.load();
    o.keylock      = s.keylock.load();
    o.reverse      = s.reverse.load();
    o.quantize     = s.quantize.load();
    o.slip         = s.slip.load();
    o.loopOn       = s.loopOn.load();
    o.loopInSec    = s.loopInSec.load();
    o.loopOutSec   = s.loopOutSec.load();
    o.bpm          = s.bpm.load();
    o.effBpm       = s.effBpm.load();
    o.beatPos      = s.beatPos.load();
    o.barPos       = s.barPos.load();
    o.syncMode     = s.syncMode.load();
    o.syncErrMs    = s.syncErrMs.load();
    o.syncLimited  = s.syncLimited.load();
    o.streamEnded  = s.streamEnded.load();
    o.pfl          = pfl_[deck];
    o.path         = deckPath_[deck];
    refreshDeckMeta(deck, o);
}

void EngineBridge::snapshot(UiSnapshot& o) {
    using R = mixer::ParameterRegistry;
    for (int d = 0; d < 4; ++d) snapshotDeck(d, o.deck[d]);

    for (int c = 0; c < 4; ++c) {
        ChannelSnapshot& ch = o.channel[c];
        auto& m = engine_.mixerCtl();
        ch.gain   = m.value(R::channelGain(c));
        ch.low    = m.value(R::channelLow(c));
        ch.mid    = m.value(R::channelMid(c));
        ch.high   = m.value(R::channelHigh(c));
        ch.filter = m.value(R::channelFilter(c));
        ch.fader  = m.value(R::channelFader(c));
        ch.sendA  = m.value(R::channelSendA(c));
        ch.sendB  = m.value(R::channelSendB(c));
        ch.cue    = m.value(R::channelCue(c))   >= 0.5f;
        ch.mute   = m.value(R::channelMute(c))  >= 0.5f;
        ch.solo   = m.value(R::channelSolo(c))  >= 0.5f;
        ch.xfAssign = (int)m.value(R::channelXfAssign(c));
        ch.levelL = o.deck[c].levelL;
        ch.levelR = o.deck[c].levelR;
    }

    auto& m = engine_.mixerCtl();
    MasterSnapshot& ms = o.master;
    ms.gain       = m.value(R::MASTER_GAIN);
    ms.cueVol     = m.value(R::CUE_VOLUME);
    ms.cueMix     = m.value(R::CUE_MIX);
    ms.crossfader = m.value(R::CROSSFADER);
    ms.xfCurve    = (int)m.value(R::XF_CURVE);
    ms.limiterOn  = m.value(R::LIMITER_ON)  >= 0.5f;
    ms.masterMute = m.value(R::MASTER_MUTE) >= 0.5f;
    const MasterStats& st = engine_.masterStats();
    ms.peakL = st.peakL.load(); ms.peakR = st.peakR.load();
    ms.rmsL  = st.rmsL.load();  ms.rmsR  = st.rmsR.load();
    ms.limiterGain = st.limiterGain.load();
    ms.clip  = st.clipCount.load() != lastClipCount_; // edge: new clips
    lastClipCount_ = st.clipCount.load();
    ms.cueAvailable = st.cueAvailable.load();
    ms.masterDeck = perf_.masterDeck();
    ms.masterBpm  = ms.masterDeck >= 0
                  ? o.deck[ms.masterDeck].effBpm : 0.0;

    const rt::AudioStats& as = engine_.stats();
    o.audio.sampleRate    = as.sampleRate.load();
    o.audio.bufferSize    = as.bufferSize.load();
    o.audio.cpuLoad       = as.cpuLoad.load();
    o.audio.maxCallbackMs = as.maxCallbackMs.load();
    o.audio.xruns         = as.xrunCount.load();
    o.audio.clips         = as.clipCount.load();
    if (auto* dev = engine_.deviceManager().getCurrentAudioDevice()) {
        o.audio.deviceName = dev->getName().toStdString();
        o.audio.running    = true;
        o.audio.inputLatencySec  = dev->getInputLatencyInSamples()
                                 / dev->getCurrentSampleRate();
        o.audio.outputLatencySec = dev->getOutputLatencyInSamples()
                                 / dev->getCurrentSampleRate();
    } else {
        o.audio.deviceName = "no device";
        o.audio.running    = false;
    }
    // Phase 13 — device engine layer (backend/latency/recovery/
    // underruns); all atomic or status-call reads, no locks
    o.audio.underruns    = as.underrunCount.load();
    o.audio.deviceErrors = as.deviceErrors.load();
    if (adevice_) {
        o.audio.backend      = adevice_->backendName();
        o.audio.estLatencyMs = adevice_->latency().roundTripMs;
        o.audio.recovery     = (int)adevice_->recoveryState();
    }

    // fx slots — read param echoes back from the controller store
    auto readFx = [&](int target, FxSlotSnapshot* dst, int slots) {
        for (int s = 0; s < slots; ++s) {
            FxSlotSnapshot& f = dst[s];
            f.kind     = (int)m.value(R::fxType(target, s));
            f.enabled  = m.value(R::fxEnabled(target, s)) >= 0.5f;
            f.mix      = m.value(R::fxMix(target, s));
            f.synced   = m.value(R::fxSyncMode(target, s)) >= 0.5f;
            f.division = (int)m.value(R::fxDivision(target, s));
            for (int pp = 0; pp < 8; ++pp)
                f.params[pp] = m.value(R::fxParam(target, s, pp));
        }
    };
    for (int d = 0; d < 4; ++d) {
        const int t = (int)fx::FXTarget::DeckA + d;
        readFx(t, o.fxDeck[d], 4);
        for (int s = 0; s < 4; ++s)
            o.fxDeck[d][s].busy =
                engine_.fxEngine().rack(t).slot(s).busy();
    }
    readFx((int)fx::FXTarget::SendA, o.fxSend[0], 4);
    readFx((int)fx::FXTarget::SendB, o.fxSend[1], 4);
    readFx((int)fx::FXTarget::Master, o.fxMaster, 4);
    o.fxReturn[0] = engine_.fxEngine().sendReturn(0);
    o.fxReturn[1] = engine_.fxEngine().sendReturn(1);

    // recording strip — atomic meter pull, no locks
    if (recorder_) {
        const auto rm = recorder_->meter();
        const auto rd = recorder_->diag();
        const auto rs = recorder_->session();
        o.rec.state = (int)recorder_->state();
        o.rec.elapsedSec = rs.durationSec;
        o.rec.peakL = rm.peakL;  o.rec.peakR = rm.peakR;
        o.rec.rmsL  = rm.rmsL;   o.rec.rmsR  = rm.rmsR;
        o.rec.maxPeak = std::max(rm.maxPeakL, rm.maxPeakR);
        o.rec.clips = rm.clipCount;
        o.rec.droppedFrames = rm.droppedFrames;
        o.rec.fillPct = rm.fillPct;
        o.rec.diskFreeBytes = rd.diskFreeBytes;
        o.rec.diskWarn = rd.diskWarn;
        o.rec.diskError = rd.diskError;
        o.rec.fileName = rs.filePath.empty() ? std::string{}
            : juce::File(juce::String(rs.filePath))
                  .getFileName().toStdString();
    }
    ++o.frame;
}

std::string EngineBridge::deckTrackPath(int deck) const {
    return deckPath_[deck & 3];
}

void EngineBridge::refreshDeckMeta(int deck, DeckSnapshot& o) {
    if (o.path.empty() || !lib_.isOpen()) return;
    if (o.trackId >= 0 && !o.title.empty()) return;   // cached
    library::Track t;
    if (!lib_.tracks().getByPath(o.path, t)) return;
    o.trackId  = t.id;
    o.title    = t.title.empty() ? t.filename : t.title;
    o.artist   = t.artist;
    o.analyzed = t.analysisState == (int)AnalysisState::Analyzed;
    if (t.bpm > 0.0 && o.bpm <= 0.0) o.bpm = t.bpm;
    if (t.keyIndex >= 0)
        o.keyText = KeyAnalyzer::camelotName(t.keyIndex);
}

void EngineBridge::notifyAnalysis(const std::string& path, bool ok) {
    if (ok) notes_.success("Analysis complete", path);
    else    notes_.error("Analysis failed", path);
}
void EngineBridge::notifyTrackLoaded(int deck,
                                     const std::string& path) {
    notes_.info("Deck " + std::string(1, char('A' + deck)) +
                " loaded", juce::File(path).getFileName().toStdString());
}
void EngineBridge::notifyTrackMissing(const std::string& path) {
    notes_.warn("Track missing", path);
}
void EngineBridge::notifyDeviceError(const std::string& what) {
    notes_.error("Audio error", what);
}

} // namespace onda::ui
