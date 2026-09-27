#include "EngineBridge.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "core/analysis/AnalysisTypes.h"
#include "core/analysis/KeyAnalyzer.h"
#include "core/harmonic/KeyShiftEngine.h"
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
                           adevice::AudioDeviceEngine* adevice,
                           stems::StemsEngine*     stems)
    : engine_(engine), perf_(perf), analysis_(analysis), lib_(lib),
      notes_(notes), recorder_(recorder), adevice_(adevice),
      stems_(stems) {
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
        // If the track is already analyzed, attach its grid NOW (the
        // completion path below only covers fresh analysis).
        if (analysis_.stateFor(path) == AnalysisState::Analyzed) {
            if (auto b = analysis_.getAnalysis(path); b && !b->grid.empty()) {
                auto g = std::make_shared<BeatGrid>(
                    b->effectiveGrid().rescaled(engine_.sampleRate()));
                perf_.attachGrid(d, g, analysis_.trackIdFor(path),
                                 b->result.bpm.confidence);
            }
        }
        // queue analysis so the deck gets its grid/waveform ASAP
        analysis_.analyzeTrack(path, (int)AnalysisPriority::High);
        // Phase 21: resolve stems availability — deck plays the
        // original regardless; a cached set joins asynchronously.
        if (stems_) {
            library::Track t;
            const bool known = lib_.tracks().getByPath(path, t);
            stems_->onTrackLoading(d, known ? t.id : -1,
                                   known ? t.contentHash : 0, path,
                                   engine_.sampleRate() > 0.0
                                       ? engine_.sampleRate() : 48000.0);
        }
        notifyTrackLoaded(d, path);
    };
    ports_.unloadTrack = [this](int d) {
        engine_.unloadTrack(d);
        if (stems_) stems_->onTrackUnloaded(d);
        deckPath_[d & 3].clear();
    };
    // ---- Phase 14: analysis edit surface (0x9000 commands) ----
    ports_.analysisEdit = [this](int d, int action, double value) {
        const std::string path = deckPath_[d & 3];
        if (path.empty()) return;
        const auto a = (AnalysisEdit)action;
        // Playhead-derived values: deck frames are device-rate, the
        // stored grid lives at the analysis reference rate.
        const double ar = [&] {
            if (auto b = analysis_.getAnalysis(path))
                if (b->result.sampleRate > 0.0) return b->result.sampleRate;
            return 48000.0;
        }();
        const double dev = engine_.sampleRate() > 0.0
                         ? engine_.sampleRate() : ar;
        if (a == AnalysisEdit::SetFirstBeat && value < 0.0) {
            value = (double)engine_.deckStats(d).framesRead.load()
                  * ar / dev;
        } else if (a == AnalysisEdit::SetDownbeat && value < 0.0) {
            const double anaPos =
                (double)engine_.deckStats(d).framesRead.load() * ar / dev;
            if (auto b = analysis_.getAnalysis(path))
                value = std::floor(
                    b->effectiveGrid().frameToBeat((int64_t)anaPos) + 0.5);
        }
        BeatGrid g = analysis_.applyEdit(path, a, value);
        if (!g.empty()) {  // republish effective grid to the deck
            float conf = 0.7f;
            if (auto b = analysis_.getAnalysis(path))
                conf = b->result.bpm.confidence;
            perf_.attachGrid(d,
                std::make_shared<BeatGrid>(g.rescaled(dev)),
                analysis_.trackIdFor(path), conf);
        }
    };
    // Phase 20 §41 — undo restores a captured effective grid verbatim:
    // rescale back to analysis units, persist as the user override,
    // republish to the deck. Used ONLY by the undo layer.
    ports_.gridRestore = [this](int d, const BeatGrid& captured) {
        const std::string path = deckPath_[d & 3];
        if (path.empty() || captured.empty()) return;
        const double ar = [&] {
            if (auto b = analysis_.getAnalysis(path))
                if (b->result.sampleRate > 0.0) return b->result.sampleRate;
            return 48000.0; }();
        const double dev = engine_.sampleRate() > 0.0
                         ? engine_.sampleRate() : ar;
        BeatGrid g = captured.rescaled(ar);
        analysis_.saveUserGrid(path, g);
        float conf = 0.7f;
        if (auto b = analysis_.getAnalysis(path))
            conf = b->result.bpm.confidence;
        perf_.attachGrid(d,
            std::make_shared<BeatGrid>(g.rescaled(dev)),
            analysis_.trackIdFor(path), conf);
    };

    // Phase 21: stems surface — actions resolve in StemsEngine and
    // reach the deck as rt::SetStemParam/SetStemMode (non-RT path).
    ports_.stemAction = [this](int d, int stem, int kind, float v) {
        if (stems_) stems_->stemAction(d, stem, kind, v);
    };
    ports_.stemModeToggle = [this](int d) {
        if (stems_) stems_->setStemMode(d,
            stems_->stemMode(d) == 1 ? 0 : 1);
    };
    ports_.stemsAnalyze = [this](int d) {
        if (!stems_) return;
        const std::string path = deckPath_[d & 3];
        if (path.empty()) return;
        library::Track t;
        if (!lib_.tracks().getByPath(path, t) || t.contentHash == 0)
            return;                      // unknown track → nothing to key
        stems_->requestSeparation(t.id, t.contentHash, path,
                                  -1, 1); // user request → high priority
    };
    ports_.stemsCancel = [this](int d) {
        if (stems_) stems_->cancelForDeck(d);
    };

    // Phase 15: copyable sync diagnostics → clipboard
    ports_.copySyncDiag = [this](int d) {
        juce::SystemClipboard::copyTextToClipboard(
            juce::String(perf_.syncDiagText(d)));
    };

    // Phase 16: FX focus surface — bound to MixerController + the
    // preset manager; macros seeded against the focus rack.
    fxSurface_.bind(&engine_.mixerCtl(), &fxPresets_);
    fxSurface_.seedDefaultMacros();
    ports_.fx = &fxSurface_;
    ports_.copyFxDiag = [this] {
        juce::SystemClipboard::copyTextToClipboard(
            juce::String(engine_.fxEngine().diagnosticReport()));
    };

    // Phase 17: mixer scenes (8 slots), preset bank and diagnostics.
    auto& mctl = engine_.mixerCtl();
    ports_.saveMixScene = [this, &mctl](int slot) {
        if (slot >= 0 && slot < 8) mctl.captureScene(mixScenes_[slot]);
    };
    ports_.recallMixScene = [this, &mctl](int slot) {
        if (slot >= 0 && slot < 8) mctl.applyScene(mixScenes_[slot]);
    };
    ports_.applyMixPreset = [this, &mctl](int delta) {
        const int i = mixPresets_.select(delta);
        if (i >= 0) mctl.applyScene(mixPresets_.scene(i));
    };
    ports_.copyMixDiag = [this] {
        juce::SystemClipboard::copyTextToClipboard(
            juce::String(engine_.mixerEngine().diagnosticReport()));
    };

    // Completed analysis → attach the effective grid (rescaled to the
    // device rate) to every deck holding that track. The queue fires
    // on a worker thread; attachGrid is control-thread only.
    analysis_.addListener([this](const AnalysisQueue::Notification& n) {
        if (n.event != AnalysisQueue::Event::Completed || !n.bundle)
            return;
        const std::string path = n.path;
        juce::MessageManager::callAsync([this, path] {
            for (int d = 0; d < 4; ++d) {
                if (deckPath_[d] != path) continue;
                BeatGrid g = analysis_.getBeatGrid(path);
                if (g.empty()) continue;
                float conf = 0.7f;
                if (auto b = analysis_.getAnalysis(path))
                    conf = b->result.bpm.confidence;
                perf_.attachGrid(d,
                    std::make_shared<BeatGrid>(
                        g.rescaled(engine_.sampleRate())),
                    analysis_.trackIdFor(path), conf);
            }
            notifyAnalysis(path, true);
        });
    });

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
    // Phase 21 — stems state: atomics for RT echoes, engine model
    // for presence/mutes (control-thread owned, cheap reads).
    o.stemMode     = s.stemMode.load();
    o.stemsAudible = s.stemsAudible.load();
    o.stemsAvail   = s.stemsAvail.load();    // published by StemsEngine
    if (stems_) {
        const auto& mgr = stems_->manager(deck);
        const auto& mx  = stems_->mixer(deck);
        for (int i = 0; i < 8; ++i) {
            uint8_t f = 0;
            if (mgr.presentMask() & (1u << (i & 3))) f |= 0x01;
            if (mx.param(i & 3).muted)               f |= 0x02;
            if (mx.param(i & 3).solo)                f |= 0x04;
            if (o.stemsAvail == (int)stems::StemAvailability::Loading ||
                o.stemsAvail == (int)stems::StemAvailability::Separating)
                f |= 0x08;
            o.stemFlags[i] = f;
        }
        for (int i = 0; i < 4; ++i)
            o.stemGain[i] = mx.param(i).gain * 0.5f;   // 0..2 → 0..1
    }
    // Phase 15 — sync control plane (control-thread reads; cheap)
    const auto ss = perf_.deckSyncState(deck);
    o.syncStatus   = (int)ss.status;
    o.syncConf     = ss.confidence;
    o.isMaster     = ss.isMaster;
    o.syncLock     = ss.syncLock;
    o.targetBpm    = ss.targetBpm;
    o.phase        = ss.phase;
    if (const auto* g = perf_.grid(deck))
        o.beatsPerBar = g->beatsPerBar;
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
    ms.masterCue  = m.value(R::MASTER_CUE) >= 0.5f;
    ms.deckMode   = m.deckMode();
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
    // Phase 14 §29: analysis workers yield while playback/recording
    // is live — only ≥HIGH priority jobs run under the gate.
    bool busy = false;
    for (int d = 0; d < 4 && !busy; ++d)
        busy = o.deck[d].state == (int)DeckState::Playing;
    if (!busy && recorder_)
        busy = o.rec.state == (int)rec::State::Recording;
    if (busy != lastBusy_) {
        analysis_.setPlaybackActive(busy);
        lastBusy_ = busy;
    }
    ++o.frame;
}

std::string EngineBridge::deckTrackPath(int deck) const {
    return deckPath_[deck & 3];
}

void EngineBridge::refreshDeckMeta(int, DeckSnapshot& o) {
    if (o.path.empty() || !lib_.isOpen()) return;
    if (o.trackId >= 0 && !o.title.empty()) return;   // cached
    library::Track t;
    if (!lib_.tracks().getByPath(o.path, t)) return;
    o.trackId  = t.id;
    o.title    = t.title.empty() ? t.filename : t.title;
    o.artist   = t.artist;
    o.analyzed = t.analysisState == (int)AnalysisState::Analyzed;
    if (t.bpm > 0.0 && o.bpm <= 0.0) o.bpm = t.bpm;
    if (t.keyIndex >= 0) {
        o.keyIndex = t.keyIndex;
        o.keyText = KeyAnalyzer::camelotName(t.keyIndex);
        // Phase 22 — tonal detail: confidence, mode, ambiguity and
        // effective key (tempo drift transposes when !keylock, §19/§20)
        o.keyConfidence = t.keyConfidence;
        o.keyIsMinor    = t.keyIndex >= 12;
        // ambiguous = persisted flag OR weak margin (both surfaces
        // say "treat as estimate", §7/§8)
        o.keyAmbiguous  = t.keyAmbiguous != 0
                       || (t.keyConfidence > 0.0f && t.keyConfidence < 0.25f);
        const double ratio = o.bpm > 0.0 ? o.effBpm / o.bpm : 1.0;
        const auto km = harmonic::KeyShiftEngine::model(
            t.keyIndex, o.keyShiftSemis, ratio, o.keylock);
        o.keyEffectiveIndex = km.effectiveIndex != t.keyIndex
            ? km.effectiveIndex : -1;
        if (o.keyEffectiveIndex >= 0)
            o.keyText = harmonic::KeyShiftEngine::display(km);
    }
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
