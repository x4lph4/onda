#include "EngineCommands.h"

#include "core/analysis/AnalysisTypes.h"
#include "core/mixer/MixerTypes.h"
#include "core/rec/RecordingEngine.h"
#include "core/sampler/SamplerEngine.h"

namespace onda::ui {

namespace {

const char* kDeckName[4] = { "Deck A", "Deck B", "Deck C", "Deck D" };

Command mk(uint32_t id, std::string name, std::string cat,
           std::function<void(float, CommandSource)> run,
           std::function<int()> state = {}) {
    Command c;
    c.id = id; c.name = std::move(name); c.category = std::move(cat);
    c.run = std::move(run); c.state = std::move(state);
    return c;
}

void push(rt::CommandQueue* q, const rt::Command& c) {
    if (q) q->tryPush(c);
}

} // namespace

void registerEngineCommands(CommandManager& cm,
                            const EnginePorts& p) {
    for (int d = 0; d < 4; ++d) {
        const std::string cat = kDeckName[d];
        const std::string dn  = std::string(1, char('a' + d));

        auto ds = [p, d]() -> DeckStats* {
            return p.deckStats ? p.deckStats(d) : nullptr;
        };

        cm.add(mk(cmd::forDeck(cmd::DeckPlayToggle, d),
            "deck" + dn + ".playToggle", cat,
            [p, d](float, CommandSource) {
                DeckStats* s = p.deckStats ? p.deckStats(d) : nullptr;
                const bool playing =
                    s && s->state.load() == (int)DeckState::Playing;
                push(p.queue, rt::DeckTransport{
                     (uint8_t)d, (uint8_t)(playing ? 0 : 1) });
            },
            [ds]() { DeckStats* s = ds();
                     return s ? (s->state.load() == (int)DeckState::Playing
                                 ? 1 : 0) : 0; }));

        cm.add(mk(cmd::forDeck(cmd::DeckStop, d),
            "deck" + dn + ".stop", cat,
            [p, d](float, CommandSource) {
                push(p.queue, rt::DeckTransport{(uint8_t)d, 2});
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckCueTrigger, d),
            "deck" + dn + ".cueTrigger", cat,
            [p, d](float v, CommandSource) {
                // value>0 press → seek to cue + play; release → stop
                if (v > 0.5f) {
                    if (p.perf) p.perf->triggerCue(d, 0);
                    push(p.queue, rt::DeckTransport{(uint8_t)d, 1});
                } else {
                    push(p.queue, rt::DeckTransport{(uint8_t)d, 0});
                    if (p.perf) p.perf->triggerCue(d, 0);
                }
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckSyncToggle, d),
            "deck" + dn + ".syncToggle", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->syncDeck(d, SyncMode::Beat);
            },
            [p, d]() {
                return p.perf
                     ? (p.perf->syncMode(d) != SyncMode::Off ? 1 : 0) : 0;
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckQuantizeTgl, d),
            "deck" + dn + ".quantizeToggle", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->setQuantize(d, !p.perf->quantizeOn(d));
            },
            [p, d]() { return p.perf && p.perf->quantizeOn(d) ? 1 : 0; }));

        cm.add(mk(cmd::forDeck(cmd::DeckSlipToggle, d),
            "deck" + dn + ".slipToggle", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->setSlip(d, !p.perf->slipOn(d));
            },
            [p, d]() { return p.perf && p.perf->slipOn(d) ? 1 : 0; }));

        cm.add(mk(cmd::forDeck(cmd::DeckKeyLockTgl, d),
            "deck" + dn + ".keyLockToggle", cat,
            [p, d, ds](float, CommandSource) {
                DeckStats* s = ds();
                const bool on = s && s->keylock.load();
                push(p.queue, rt::SetKeyLock{(uint8_t)d, !on});
            },
            [ds]() { DeckStats* s = ds();
                     return s && s->keylock.load() ? 1 : 0; }));

        cm.add(mk(cmd::forDeck(cmd::DeckReverseTgl, d),
            "deck" + dn + ".reverseToggle", cat,
            [p, d, ds](float, CommandSource) {
                if (p.perf) p.perf->setReverse(d,
                    !(ds() && ds()->reverse.load()));
            },
            [ds]() { DeckStats* s = ds();
                     return s && s->reverse.load() ? 1 : 0; }));

        cm.add(mk(cmd::forDeck(cmd::DeckLoopToggle, d),
            "deck" + dn + ".loopToggle", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopToggle(d);
            },
            [ds]() { DeckStats* s = ds();
                     return s && s->loopOn.load() ? 1 : 0; }));

        cm.add(mk(cmd::forDeck(cmd::DeckLoopIn, d),
            "deck" + dn + ".loopIn", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopIn(d);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckLoopOut, d),
            "deck" + dn + ".loopOut", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopOut(d);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckLoopHalve, d),
            "deck" + dn + ".loopHalve", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopHalve(d);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckLoopDouble, d),
            "deck" + dn + ".loopDouble", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopDouble(d);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckLoopMoveL, d),
            "deck" + dn + ".loopMoveLeft", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopMove(d, -1.0);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckLoopMoveR, d),
            "deck" + dn + ".loopMoveRight", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopMove(d, 1.0);
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckJumpBack, d),
            "deck" + dn + ".jumpBack", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->beatJump(d, -4.0);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckJumpFwd, d),
            "deck" + dn + ".jumpFwd", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->beatJump(d, 4.0);
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckBendDown, d),
            "deck" + dn + ".bendDown", cat,
            [p, d](float v, CommandSource) {
                push(p.queue, rt::SetPitchBend{(uint8_t)d,
                     v > 0.5f ? -4.0f : 0.0f});
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckBendUp, d),
            "deck" + dn + ".bendUp", cat,
            [p, d](float v, CommandSource) {
                push(p.queue, rt::SetPitchBend{(uint8_t)d,
                     v > 0.5f ? 4.0f : 0.0f});
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckLoad, d),
            "deck" + dn + ".load", cat,
            [p, d](float, CommandSource) {
                if (p.loadTrack && p.browserSelection) {
                    const std::string sel = p.browserSelection();
                    if (!sel.empty()) p.loadTrack(d, sel);
                }
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckUnload, d),
            "deck" + dn + ".unload", cat,
            [p, d](float, CommandSource) {
                if (p.unloadTrack) p.unloadTrack(d);
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckPflToggle, d),
            "deck" + dn + ".pflToggle", cat,
            [p, d](float, CommandSource) {
                if (p.mixer) {
                    using R = mixer::ParameterRegistry;
                    const float cur = p.mixer->value(R::channelCue(d));
                    p.mixer->channelCue(d, cur < 0.5f);
                }
            },
            [p, d]() {
                if (!p.mixer) return 0;
                using R = mixer::ParameterRegistry;
                return p.mixer->value(R::channelCue(d)) >= 0.5f ? 1 : 0;
            }));

        // ---- analysis edits (Phase 14): value -1 = derive at handler ----
        auto ana = [&](uint32_t id, const char* name, int action,
                       double value) {
            cm.add(mk(cmd::forDeck((cmd::Id)id, d),
                "deck" + dn + "." + name, cat,
                [p, d, action, value](float, CommandSource) {
                    if (p.analysisEdit) p.analysisEdit(d, action, value);
                }));
        };
        ana(cmd::AnaSetFirstBeat, "ana.setFirstBeat",
            (int)AnalysisEdit::SetFirstBeat, -1.0);
        ana(cmd::AnaNudgeLeft, "ana.nudgeLeft",
            (int)AnalysisEdit::NudgeGrid, -10.0);
        ana(cmd::AnaNudgeRight, "ana.nudgeRight",
            (int)AnalysisEdit::NudgeGrid, 10.0);
        ana(cmd::AnaBpmDouble, "ana.bpmDouble",
            (int)AnalysisEdit::DoubleTempo, 0.0);
        ana(cmd::AnaBpmHalf, "ana.bpmHalf",
            (int)AnalysisEdit::HalfTempo, 0.0);
        ana(cmd::AnaSetDownbeat, "ana.setDownbeat",
            (int)AnalysisEdit::SetDownbeat, -1.0);
        ana(cmd::AnaGridReset, "ana.gridReset",
            (int)AnalysisEdit::ResetGrid, 0.0);
        ana(cmd::AnaReanalyze, "ana.reanalyze",
            (int)AnalysisEdit::Reanalyze, 0.0);

        // ---- sync control (Phase 15): lock / quantum / tempo-only ----
        cm.add(mk(cmd::forDeck(cmd::SyncLockTgl, d),
            "deck" + dn + ".syncLock", cat,
            [p, d](float, CommandSource) {
                if (p.perf)
                    p.perf->setSyncLock(d, !p.perf->syncLockOn(d));
            },
            [p, d]() {
                return p.perf && p.perf->syncLockOn(d) ? 1 : 0;
            }));

        cm.add(mk(cmd::forDeck(cmd::SyncQuantRes, d),
            "deck" + dn + ".syncQuantRes", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->cycleQuantize(d);
            },
            [p, d]() {
                return p.perf ? p.perf->quantizeResolution(d) : 0;
            }));

        cm.add(mk(cmd::forDeck(cmd::SyncBpmMode, d),
            "deck" + dn + ".syncBpmMode", cat,
            [p, d](float, CommandSource) {
                // tempo-only: toggles SyncMode::Bpm on the deck
                if (p.perf) p.perf->syncDeck(d, SyncMode::Bpm);
            },
            [p, d]() {
                return p.perf && p.perf->syncMode(d) == SyncMode::Bpm
                     ? 1 : 0;
            }));

        cm.add(mk(cmd::forDeck(cmd::SyncDiagCopy, d),
            "deck" + dn + ".syncDiagCopy", cat,
            [p, d](float, CommandSource) {
                if (p.copySyncDiag) p.copySyncDiag(d);
            }));

        // hot cues: trigger/set/delete per slot
        for (int s = 0; s < 8; ++s) {
            const std::string base =
                "deck" + dn + ".hotcue" + std::to_string(s);
            cm.add(mk(cmd::forHotCue(d, s, 0), base + ".trigger", cat,
                [p, d, s](float, CommandSource) {
                    if (p.perf) p.perf->triggerCue(d, s);
                }));
            cm.add(mk(cmd::forHotCue(d, s, 1), base + ".set", cat,
                [p, d, s](float, CommandSource) {
                    if (p.perf) p.perf->setCue(d, s, CueType::Hot);
                }));
            cm.add(mk(cmd::forHotCue(d, s, 2), base + ".delete", cat,
                [p, d, s](float, CommandSource) {
                    if (p.perf) p.perf->deleteCue(d, s);
                }));
        }
    }

    // ---- mixer app-level nudges (fine controls live on the strip) ----
    cm.add(mk(cmd::MixMasterGainUp, "mixer.masterGainUp", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) {
                using R = mixer::ParameterRegistry;
                p.mixer->masterGain(std::min(2.0f,
                    p.mixer->value(R::MASTER_GAIN) + 0.02f));
            }
        }));
    cm.add(mk(cmd::MixMasterGainDn, "mixer.masterGainDn", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) {
                using R = mixer::ParameterRegistry;
                p.mixer->masterGain(std::max(0.0f,
                    p.mixer->value(R::MASTER_GAIN) - 0.02f));
            }
        }));
    cm.add(mk(cmd::MixLimiterTgl, "mixer.limiterToggle", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) {
                using R = mixer::ParameterRegistry;
                p.mixer->limiterOn(p.mixer->value(R::LIMITER_ON) < 0.5f);
            }
        }));
    cm.add(mk(cmd::MixCueVolUp, "mixer.cueVolUp", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) {
                using R = mixer::ParameterRegistry;
                p.mixer->cueVolume(std::min(1.0f,
                    p.mixer->value(R::CUE_VOLUME) + 0.05f));
            }
        }));
    cm.add(mk(cmd::MixCueVolDn, "mixer.cueVolDn", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) {
                using R = mixer::ParameterRegistry;
                p.mixer->cueVolume(std::max(0.0f,
                    p.mixer->value(R::CUE_VOLUME) - 0.05f));
            }
        }));
    cm.add(mk(cmd::MixXfToA, "mixer.xfToA", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) p.mixer->crossfader(-1.0f);
        }));
    cm.add(mk(cmd::MixXfToB, "mixer.xfToB", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) p.mixer->crossfader(1.0f);
        }));
    cm.add(mk(cmd::MixXfCenter, "mixer.xfCenter", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) p.mixer->crossfader(0.0f);
        }));

    // ---- browser / app (routed through appAction port) ----
    auto app = [&](uint32_t id, const char* name, const char* cat) {
        cm.add(mk(id, name, cat,
            [p, id](float, CommandSource) {
                if (p.appAction) p.appAction(id);
            }));
    };
    app(cmd::BrowserFocusSearch, "browser.focusSearch", "Browser");
    app(cmd::BrowserNextTrack,   "browser.nextTrack",   "Browser");
    app(cmd::BrowserPrevTrack,   "browser.prevTrack",   "Browser");
    app(cmd::BrowserTogglePanel, "browser.togglePanel", "Browser");
    app(cmd::AppToggleSettings,  "app.toggleSettings",  "App");
    app(cmd::AppLayoutNext,      "app.layoutNext",      "App");
    app(cmd::AppFullscreen,      "app.fullscreen",      "App");
    app(cmd::AppToggleFx,        "app.toggleFx",        "App");

    // tempo master: AUTO → deck A..D → AUTO (Phase 15)
    cm.add(mk(cmd::SyncMasterCycle, "sync.masterCycle", "Sync",
        [p](float, CommandSource) {
            if (p.perf) p.perf->cycleMaster();
        },
        [p]() {
            // state = resolved master deck (-1 none)
            return p.perf ? p.perf->masterDeck() : -1;
        }));

    // panic: stop all decks + center crossfader
    cm.add(mk(cmd::AppPanic, "app.panic", "App",
        [p](float, CommandSource) {
            for (int d = 0; d < 4; ++d)
                push(p.queue, rt::DeckTransport{(uint8_t)d, 0});
            if (p.mixer) p.mixer->crossfader(0.0f);
        }));

    // browser load-to-deck uses selection + loadTrack directly
    cm.add(mk(cmd::BrowserLoadDeckA, "browser.loadDeckA", "Browser",
        [p](float, CommandSource) {
            if (p.loadTrack && p.browserSelection) {
                const std::string s = p.browserSelection();
                if (!s.empty()) p.loadTrack(0, s);
            }
        }));
    cm.add(mk(cmd::BrowserLoadDeckB, "browser.loadDeckB", "Browser",
        [p](float, CommandSource) {
            if (p.loadTrack && p.browserSelection) {
                const std::string s = p.browserSelection();
                if (!s.empty()) p.loadTrack(1, s);
            }
        }));

    // ---- sampler (Phase 11): trigger/stop + bank + sync controls ----
    for (int s = 0; s < 32; ++s) {
        const std::string sn = "sampler." + std::to_string(s);
        cm.add(mk(cmd::SamplerTrigBase + (uint32_t)s, sn + ".trigger",
            "Sampler",
            [p, s](float v, CommandSource) {
                if (p.sampler) p.sampler->trigger(s, v > 0.0f ? v : 1.0f);
            }));
        cm.add(mk(cmd::SamplerStopBase + (uint32_t)s, sn + ".stop",
            "Sampler",
            [p, s](float, CommandSource) {
                if (p.sampler) p.sampler->stop(s);
            }));
    }
    cm.add(mk(cmd::SamplerStopAll, "sampler.stopAll", "Sampler",
        [p](float, CommandSource) {
            if (p.sampler) p.sampler->stopAll();
        }));
    cm.add(mk(cmd::SamplerQuantize, "sampler.quantize", "Sampler",
        [p](float v, CommandSource) {
            if (p.sampler)
                for (int s = 0; s < 8; ++s)
                    p.sampler->setSlotParam(s, sampler::SP_Quantize, v);
        }));
    cm.add(mk(cmd::SamplerSyncTgl, "sampler.syncToggle", "Sampler",
        [p](float, CommandSource) {
            if (!p.sampler) return;
            const float cur =
                p.sampler->slotParam(0, sampler::SP_SyncOn);
            for (int s = 0; s < 8; ++s)
                p.sampler->setSlotParam(s, sampler::SP_SyncOn,
                                        cur < 0.5f ? 1.f : 0.f);
        },
        [p]() {
            return p.sampler
                 ? (p.sampler->slotParam(0, sampler::SP_SyncOn) > 0.5f
                    ? 1 : 0) : 0;
        }));
    cm.add(mk(cmd::SamplerBankNext, "sampler.bankNext", "Sampler",
        [p](float, CommandSource) {
            if (p.sampler && p.sampler->bankCount() > 0)
                p.sampler->selectBank(
                    (p.sampler->activeBank() + 1) %
                    p.sampler->bankCount());
        }));
    cm.add(mk(cmd::SamplerBankPrev, "sampler.bankPrev", "Sampler",
        [p](float, CommandSource) {
            if (p.sampler && p.sampler->bankCount() > 0)
                p.sampler->selectBank(
                    (p.sampler->activeBank() +
                     p.sampler->bankCount() - 1) %
                    p.sampler->bankCount());
        }));
    cm.add(mk(cmd::SamplerBankSet, "sampler.bankSet", "Sampler",
        [p](float v, CommandSource) {
            if (p.sampler) p.sampler->selectBank((int)v);
        },
        [p]() { return p.sampler ? p.sampler->activeBank() : -1; }));

    // ---- recording (Phase 12) ----
    cm.add(mk(cmd::RecArm, "rec.arm", "Recording",
        [p](float, CommandSource) {
            if (p.recorder) p.recorder->arm();
        }));
    cm.add(mk(cmd::RecStart, "rec.start", "Recording",
        [p](float, CommandSource) {
            if (p.recorder) p.recorder->start();
        }));
    cm.add(mk(cmd::RecStop, "rec.stop", "Recording",
        [p](float, CommandSource) {
            if (p.recorder) p.recorder->stop();
        }));
    cm.add(mk(cmd::RecPause, "rec.pause", "Recording",
        [p](float, CommandSource) {
            if (p.recorder) p.recorder->pause();
        }));
    cm.add(mk(cmd::RecResume, "rec.resume", "Recording",
        [p](float, CommandSource) {
            if (p.recorder) p.recorder->resume();
        }));
    cm.add(mk(cmd::RecSplit, "rec.split", "Recording",
        [p](float, CommandSource) {
            if (p.recorder) p.recorder->split();
        }));
    cm.add(mk(cmd::RecToggle, "rec.toggle", "Recording",
        [p](float, CommandSource) {
            if (p.recorder) p.recorder->toggle();
        },
        [p]() {
            if (!p.recorder) return 0;
            const auto st = p.recorder->state();
            return (st == rec::State::Recording
                 || st == rec::State::Paused) ? 1 : 0;
        }));
}

} // namespace onda::ui
