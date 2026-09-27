#include "EngineCommands.h"

#include <optional>

#include "FXSurface.h"
#include "UndoManager.h"
#include "core/analysis/AnalysisTypes.h"
#include "core/mixer/MixerTypes.h"
#include "core/rec/RecordingEngine.h"
#include "core/sampler/SamplerEngine.h"

namespace onda::ui {

namespace {

const char* kDeckName[4] = { "Deck A", "Deck B", "Deck C", "Deck D" };

/// §41 undo helpers — capture value copies before the op mutates.
std::optional<Cue> grabCue(PerformanceEngine* e, int d, int s) {
    if (!e) return std::nullopt;
    for (const auto& c : e->cues(d))
        if (c.slot == s) return c;
    return std::nullopt;
}
std::shared_ptr<BeatGrid> grabGrid(PerformanceEngine* e, int d) {
    const BeatGrid* g = e ? e->grid(d) : nullptr;
    return g ? std::make_shared<BeatGrid>(*g) : nullptr;
}

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
                // §3 — the unified transport engine owns play/pause;
                // direct push only when the perf layer isn't wired.
                if (p.perf) { p.perf->transport(d, Transport::Toggle);
                              return; }
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
                if (p.perf) p.perf->transport(d, Transport::Stop);
                else push(p.queue, rt::DeckTransport{(uint8_t)d, 2});
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckCueTrigger, d),
            "deck" + dn + ".cueTrigger", cat,
            [p, d](float v, CommandSource) {
                // Phase 20 §24-25 — dedicated cue button: press while
                // playing returns+pauses; press while paused defines
                // the point + previews; release returns to it.
                if (p.perf) p.perf->cueButton(d,
                    v > 0.5f ? CuePhase::Press : CuePhase::Release);
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
                // §29 — key lock lives in the perf layer (state +
                // pref persistence); RT echo stays the source of truth
                // for the button state.
                if (p.perf) {
                    p.perf->setKeyLock(d, !p.perf->keyLockOn(d));
                    return;
                }
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
                if (!p.perf) return;
                int64_t i0 = -1, o0 = -1;
                p.perf->loopRange(d, i0, o0);
                const auto* st = p.deckStats ? p.deckStats(d) : nullptr;
                const bool wasOn = st && st->loopOn.load();
                p.perf->loopToggle(d);
                int64_t i1 = -1, o1 = -1;
                p.perf->loopRange(d, i1, o1);
                const bool on1 = !wasOn && i1 >= 0 && o1 > i1;
                if (p.undo)
                    p.undo->record("loop toggle",
                        [p, d, i0, o0, wasOn] {
                            p.perf->restoreLoopRange(d, i0, o0, wasOn); },
                        [p, d, i1, o1, on1] {
                            p.perf->restoreLoopRange(d, i1, o1, on1); });
            },
            [ds]() { DeckStats* s = ds();
                     return s && s->loopOn.load() ? 1 : 0; }));

        cm.add(mk(cmd::forDeck(cmd::DeckLoopIn, d),
            "deck" + dn + ".loopIn", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->loopIn(d);
            }));
        // §41 — loop ops record range+state so undo restores both.
        // `endedOn` is the post-op loop-enable the redo must restore
        // (setLoopFrames always re-arms a valid range).
        auto loopOp = [&](cmd::Id id, const char* name,
                          std::function<void()> op,
                          std::function<bool(bool)> endedOn) {
            cm.add(mk(cmd::forDeck(id, d), "deck" + dn + "." + name, cat,
                [p, d, op, endedOn, name](float, CommandSource) {
                    if (!p.perf) return;
                    int64_t i0 = -1, o0 = -1;
                    p.perf->loopRange(d, i0, o0);
                    const auto* st = p.deckStats ? p.deckStats(d) : nullptr;
                    const bool wasOn = st && st->loopOn.load();
                    op();
                    int64_t i1 = -1, o1 = -1;
                    p.perf->loopRange(d, i1, o1);
                    const bool on1 = endedOn(wasOn);
                    if (p.undo) {
                        const std::string lbl =
                            std::string("loop ") + name;
                        p.undo->record(lbl,
                            [p, d, i0, o0, wasOn] {
                                p.perf->restoreLoopRange(d, i0, o0, wasOn); },
                            [p, d, i1, o1, on1] {
                                p.perf->restoreLoopRange(d, i1, o1, on1); });
                    }
                }));
        };
        loopOp(cmd::DeckLoopOut, "loopOut",
               [p, d] { p.perf->loopOut(d); }, [](bool) { return true; });
        loopOp(cmd::DeckLoopHalve, "loopHalve",
               [p, d] { p.perf->loopHalve(d); }, [](bool) { return true; });
        loopOp(cmd::DeckLoopDouble, "loopDouble",
               [p, d] { p.perf->loopDouble(d); }, [](bool) { return true; });
        loopOp(cmd::DeckLoopMoveL, "loopMoveLeft",
               [p, d] { p.perf->loopMove(d, -1.0); }, [](bool) { return true; });
        loopOp(cmd::DeckLoopMoveR, "loopMoveRight",
               [p, d] { p.perf->loopMove(d, 1.0); }, [](bool) { return true; });
        // sized auto-loop: arg >= 10.5 → explicit index (pads send
        // 10+idx); otherwise the deck's selected size index.
        auto sized = [p, d](float v) {
            const int idx = v >= 10.5f ? (int)v - 10
                                       : p.perf->loopSizeIndex(d);
            p.perf->loopBeatsIdx(d, idx);
        };
        cm.add(mk(cmd::forDeck(cmd::DeckLoopSizeTrig, d),
            "deck" + dn + ".loopSized", cat,
            [p, d, sized](float v, CommandSource) {
                if (!p.perf) return;
                int64_t i0 = -1, o0 = -1;
                p.perf->loopRange(d, i0, o0);
                const auto* st = p.deckStats ? p.deckStats(d) : nullptr;
                const bool wasOn = st && st->loopOn.load();
                sized(v);
                int64_t i1 = -1, o1 = -1;
                p.perf->loopRange(d, i1, o1);
                if (p.undo)
                    p.undo->record("loop sized",
                        [p, d, i0, o0, wasOn] {
                            p.perf->restoreLoopRange(d, i0, o0, wasOn); },
                        [p, d, i1, o1] {
                            p.perf->restoreLoopRange(d, i1, o1, true); });
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckJumpBack, d),
            "deck" + dn + ".jumpBack", cat,
            [p, d](float v, CommandSource) {
                if (p.perf) p.perf->beatJump(d, -(
                    v > 0.0 ? (double)v : p.perf->jumpSizeBeats(d)));
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckJumpFwd, d),
            "deck" + dn + ".jumpFwd", cat,
            [p, d](float v, CommandSource) {
                if (p.perf) p.perf->beatJump(d,
                    v > 0.0 ? (double)v : p.perf->jumpSizeBeats(d));
            }));

        cm.add(mk(cmd::forDeck(cmd::DeckBendDown, d),
            "deck" + dn + ".bendDown", cat,
            [p, d](float v, CommandSource) {
                // §37 — temporary bend through the pitch engine.
                const float b = v > 0.5f ? -4.0f : 0.0f;
                if (p.perf) p.perf->pitchBend(d, b);
                else push(p.queue, rt::SetPitchBend{(uint8_t)d, b});
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckBendUp, d),
            "deck" + dn + ".bendUp", cat,
            [p, d](float v, CommandSource) {
                const float b = v > 0.5f ? 4.0f : 0.0f;
                if (p.perf) p.perf->pitchBend(d, b);
                else push(p.queue, rt::SetPitchBend{(uint8_t)d, b});
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

        // ---- analysis edits (Phase 14): value -1 = derive at handler.
        // §41: capture the effective grid before/after — undo restores
        // it verbatim through the gridRestore port (reanalysis is not
        // an edit; it is intentionally not undoable).
        auto ana = [&](uint32_t id, const char* name, int action,
                       double value) {
            cm.add(mk(cmd::forDeck((cmd::Id)id, d),
                "deck" + dn + "." + name, cat,
                [p, d, action, value, name](float, CommandSource) {
                    if (!p.analysisEdit) return;
                    const auto before = grabGrid(p.perf, d);
                    p.analysisEdit(d, action, value);
                    const auto after = grabGrid(p.perf, d);
                    if (p.undo && before && after &&
                        action != (int)AnalysisEdit::Reanalyze)
                        p.undo->record(std::string("grid ") + name,
                            [p, d, before] {
                                if (p.gridRestore)
                                    p.gridRestore(d, *before); },
                            [p, d, after] {
                                if (p.gridRestore)
                                    p.gridRestore(d, *after); });
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

        // ---- Phase 19: waveform/deck view actions — presentation
        // state only; the app shell routes them to the views. ----
        auto view = [&](cmd::Id base, const char* name) {
            cm.add(mk(cmd::forDeck(base, d), "deck" + dn + "." + name,
                cat, [p, d, base](float v, CommandSource) {
                    if (p.viewAction)
                        p.viewAction(d, (int)cmd::actionOf(
                            cmd::forDeck(base, d)), (double)v);
                }));
        };
        view(cmd::WaveZoomIn,      "wave.zoomIn");
        view(cmd::WaveZoomOut,     "wave.zoomOut");
        view(cmd::WaveZoomFit,     "wave.zoomFit");
        view(cmd::WaveAutoScroll,  "wave.autoScroll");
        view(cmd::WaveGridEditTgl, "wave.gridEdit");
        view(cmd::DeckTimeMode,    "deck.timeMode");

        // hot cues: trigger/set/delete per slot (§4 — set/delete are
        // undoable metadata edits; trigger is a transport action)
        for (int s = 0; s < 8; ++s) {
            const std::string base =
                "deck" + dn + ".hotcue" + std::to_string(s);
            cm.add(mk(cmd::forHotCue(d, s, 0), base + ".trigger", cat,
                [p, d, s](float, CommandSource) {
                    if (p.perf) p.perf->triggerCue(d, s);
                }));
            cm.add(mk(cmd::forHotCue(d, s, 1), base + ".set", cat,
                [p, d, s](float, CommandSource) {
                    if (!p.perf) return;
                    const auto before = grabCue(p.perf, d, s);
                    p.perf->setCue(d, s, CueType::Hot);
                    const auto after = grabCue(p.perf, d, s);
                    if (p.undo && after)
                        p.undo->record("hot cue set",
                            [p, d, s, before] {
                                if (before) p.perf->restoreCue(d, *before);
                                else        p.perf->deleteCue(d, s); },
                            [p, d, after] {
                                p.perf->restoreCue(d, *after); });
                }));
            cm.add(mk(cmd::forHotCue(d, s, 2), base + ".delete", cat,
                [p, d, s](float, CommandSource) {
                    if (!p.perf) return;
                    const auto before = grabCue(p.perf, d, s);
                    if (p.perf->deleteCue(d, s) && p.undo && before)
                        p.undo->record("hot cue delete",
                            [p, d, before] {
                                p.perf->restoreCue(d, *before); },
                            [p, d, s] {
                                p.perf->deleteCue(d, s); });
                }));
        }

        // memory cues: goto/set/delete per slot (§7-8 — navigation
        // markers; goto = seek only, never auto-plays)
        for (int s = 0; s < 8; ++s) {
            const std::string base =
                "deck" + dn + ".memcue" + std::to_string(s);
            cm.add(mk(cmd::forMemCue(d, s, 0), base + ".goto", cat,
                [p, d, s](float, CommandSource) {
                    // memory trigger = navigation only (seek, no play)
                    if (!p.perf) return;
                    const Cue* c = nullptr;
                    for (const auto* m : p.perf->memoryCues(d))
                        if (m->slot == s) { c = m; break; }
                    if (!c) return;
                    // reuse triggerCue: Memory type already seeks-only
                    // — find its real slot index in the shared table
                    if (p.perf) p.perf->triggerCue(d, c->slot);
                }));
            cm.add(mk(cmd::forMemCue(d, s, 1), base + ".set", cat,
                [p, d, s](float, CommandSource) {
                    if (!p.perf) return;
                    const auto before = grabCue(p.perf, d, s);
                    p.perf->setCue(d, s, CueType::Memory, -1, -1,
                                   0xFF33B5E5);
                    const auto after = grabCue(p.perf, d, s);
                    if (p.undo && after)
                        p.undo->record("memory cue set",
                            [p, d, s, before] {
                                if (before) p.perf->restoreCue(d, *before);
                                else        p.perf->deleteCue(d, s); },
                            [p, d, after] {
                                p.perf->restoreCue(d, *after); });
                }));
            cm.add(mk(cmd::forMemCue(d, s, 2), base + ".delete", cat,
                [p, d, s](float, CommandSource) {
                    if (!p.perf) return;
                    const auto before = grabCue(p.perf, d, s);
                    if (before && before->type != CueType::Memory)
                        return; // never delete a hot cue from mem space
                    if (p.perf->deleteCue(d, s) && p.undo && before)
                        p.undo->record("memory cue delete",
                            [p, d, before] {
                                p.perf->restoreCue(d, *before); },
                            [p, d, s] {
                                p.perf->deleteCue(d, s); });
                }));
        }

        // ---- Phase 20: cue point / restart / sizes / roll / rev ----
        cm.add(mk(cmd::forDeck(cmd::DeckCueSet, d),
            "deck" + dn + ".cueSet", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->setCuePoint(d, -1); // = playhead
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckInstantRestart, d),
            "deck" + dn + ".instantRestart", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->instantRestart(d);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckLoopSizeUp, d),
            "deck" + dn + ".loopSizeUp", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->setLoopSizeIndex(d,
                    LoopSizeController::stepIndex(
                        p.perf->loopSizeIndex(d), 1,
                        LoopSizeController::loopCount()));
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckLoopSizeDn, d),
            "deck" + dn + ".loopSizeDn", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->setLoopSizeIndex(d,
                    LoopSizeController::stepIndex(
                        p.perf->loopSizeIndex(d), -1,
                        LoopSizeController::loopCount()));
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckRollGate, d),
            "deck" + dn + ".roll", cat,
            [p, d](float v, CommandSource) {
                if (!p.perf) return;
                if (v > 0.5f)   // press = roll; arg = idx+1 (pads)
                    p.perf->loopRollIdx(d,
                        v > 1.5f ? (int)v - 1 : 3); // default 1/4
                else
                    p.perf->loopRollEnd(d);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckJumpSizeUp, d),
            "deck" + dn + ".jumpSizeUp", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->setJumpSizeIndex(d,
                    LoopSizeController::stepIndex(
                        p.perf->jumpSizeIndex(d), 1,
                        LoopSizeController::jumpCount()));
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckJumpSizeDn, d),
            "deck" + dn + ".jumpSizeDn", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->setJumpSizeIndex(d,
                    LoopSizeController::stepIndex(
                        p.perf->jumpSizeIndex(d), -1,
                        LoopSizeController::jumpCount()));
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckRevHold, d),
            "deck" + dn + ".revHold", cat,
            [p, d](float v, CommandSource) {
                if (p.perf) p.perf->reverseMomentary(d, v > 0.5f);
            }));

        // ---- Phase 28: tempo reset / brake / cue-behaviour cycle ----
        cm.add(mk(cmd::forDeck(cmd::DeckPitchReset, d),
            "deck" + dn + ".pitchReset", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->pitchReset(d);
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckBrakeTgl, d),
            "deck" + dn + ".brake", cat,
            [p, d](float, CommandSource) {
                if (p.perf) p.perf->transport(d, Transport::Brake);
            },
            [p, d]() {
                return p.perf && p.perf->brakeOn(d) ? 1 : 0;
            }));
        cm.add(mk(cmd::forDeck(cmd::DeckCueMode, d),
            "deck" + dn + ".cueMode", cat,
            [p, d](float, CommandSource) {
                if (p.perf)
                    p.perf->setCueBehavior(d,
                        (CueBehavior)(((int)p.perf->cueBehavior(d) + 1)
                                      % 3));
            },
            [p, d]() {
                return p.perf ? (int)p.perf->cueBehavior(d) : 0;
            }));

        // ---- Phase 21: stems surface -------------------------------
        cm.add(mk(cmd::forDeck(cmd::DeckStemModeTgl, d),
            "deck" + dn + ".stemsMode", cat,
            [p, d](float, CommandSource) {
                if (p.stemModeToggle) p.stemModeToggle(d); }));
        cm.add(mk(cmd::forDeck(cmd::DeckStemAnalyze, d),
            "deck" + dn + ".stemsAnalyze", cat,
            [p, d](float, CommandSource) {
                if (p.stemsAnalyze) p.stemsAnalyze(d); }));
        cm.add(mk(cmd::forDeck(cmd::DeckStemCancel, d),
            "deck" + dn + ".stemsCancel", cat,
            [p, d](float, CommandSource) {
                if (p.stemsCancel) p.stemsCancel(d); }));
        cm.add(mk(cmd::forDeck(cmd::DeckStemUnmute, d),
            "deck" + dn + ".stemsUnmute", cat,
            [p, d](float, CommandSource) {
                if (p.stemAction) p.stemAction(d, -1, 8, 0.0f); }));
        // per-stem: base + stem*8 + kind (mute/solo/gain±/pan±/gate/set)
        for (int s = 0; s < 4; ++s) {
            static const char* kKind[8] = {
                "mute", "solo", "gainUp", "gainDn",
                "panL", "panR", "gate", "gainSet" };
            for (int k = 0; k < 8; ++k) {
                cm.add(mk(cmd::forDeck(cmd::DeckStemBase, d) + s * 8 + k,
                    "deck" + dn + ".stem" + std::to_string(s) + "." + kKind[k],
                    cat,
                    [p, d, s, k](float v, CommandSource) {
                        if (p.stemAction) p.stemAction(d, s, k, v); }));
            }
        }
    }

    // ---- Phase 20 §41: performance undo/redo (global) ----
    cm.add(mk(cmd::PerfUndo, "perf.undo", "Performance",
        [p](float, CommandSource) { if (p.undo) p.undo->undo(); },
        [p]() { return p.undo && p.undo->canUndo() ? 1 : 0; }));
    cm.add(mk(cmd::PerfRedo, "perf.redo", "Performance",
        [p](float, CommandSource) { if (p.undo) p.undo->redo(); },
        [p]() { return p.undo && p.undo->canRedo() ? 1 : 0; }));

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

    // ---- Phase 17: master cue / deck mode / scenes / presets / diag ----
    cm.add(mk(cmd::MixMasterCueTgl, "mixer.masterCueToggle", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer) {
                using R = mixer::ParameterRegistry;
                p.mixer->masterCue(p.mixer->value(R::MASTER_CUE) < 0.5f);
            }
        }));
    cm.add(mk(cmd::MixDeckModeTgl, "mixer.deckModeToggle", "Mixer",
        [p](float, CommandSource) {
            if (p.mixer)
                p.mixer->setDeckMode(p.mixer->deckMode() > 2 ? 2 : 4);
        }));
    cm.add(mk(cmd::MixDiagCopy, "mixer.diagCopy", "Mixer",
        [p](float, CommandSource) {
            if (p.copyMixDiag) p.copyMixDiag();
        }));
    cm.add(mk(cmd::MixSceneSave, "mixer.sceneSave", "Mixer",
        [p](float v, CommandSource) {
            if (p.saveMixScene) p.saveMixScene((int)v);
        }));
    cm.add(mk(cmd::MixSceneRecall, "mixer.sceneRecall", "Mixer",
        [p](float v, CommandSource) {
            if (p.recallMixScene) p.recallMixScene((int)v);
        }));
    cm.add(mk(cmd::MixPresetNext, "mixer.presetNext", "Mixer",
        [p](float, CommandSource) {
            if (p.applyMixPreset) p.applyMixPreset(+1);
        }));
    cm.add(mk(cmd::MixPresetPrev, "mixer.presetPrev", "Mixer",
        [p](float, CommandSource) {
            if (p.applyMixPreset) p.applyMixPreset(-1);
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
            for (int d = 0; d < 4; ++d) {
                if (p.perf) p.perf->transport(d, Transport::Pause);
                else push(p.queue, rt::DeckTransport{(uint8_t)d, 0});
            }
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

    // ---- FX surface (Phase 16): focus + slot ops + presets/macros ----
    cm.add(mk(cmd::FxFocusTarget, "fx.focusTarget", "FX",
        [p](float v, CommandSource) {
            if (p.fx) p.fx->setFocus((int)v, p.fx->focusSlot());
        },
        [p]() { return p.fx ? p.fx->focusTarget() : -1; }));
    cm.add(mk(cmd::FxFocusSlot, "fx.focusSlot", "FX",
        [p](float v, CommandSource) {
            if (p.fx) p.fx->setFocus(p.fx->focusTarget(), (int)v);
        },
        [p]() { return p.fx ? p.fx->focusSlot() : -1; }));
    cm.add(mk(cmd::FxEnable, "fx.enable", "FX",
        [p](float, CommandSource) {
            if (p.fx) p.fx->toggleEnabled();
        },
        [p]() {
            if (!p.fx || !p.mixer) return 0;
            using R = mixer::ParameterRegistry;
            return p.mixer->value(
                R::fxEnabled(p.fx->focusTarget(),
                             p.fx->focusSlot())) >= 0.5f ? 1 : 0;
        }));
    cm.add(mk(cmd::FxTypeNext, "fx.typeNext", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->typeStep(1); }));
    cm.add(mk(cmd::FxTypePrev, "fx.typePrev", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->typeStep(-1); }));
    cm.add(mk(cmd::FxTypeSet, "fx.typeSet", "FX",
        [p](float v, CommandSource) {
            if (p.fx) p.fx->setType((int)v);
        },
        [p]() {
            if (!p.fx || !p.mixer) return 0;
            using R = mixer::ParameterRegistry;
            return (int)p.mixer->value(
                R::fxType(p.fx->focusTarget(), p.fx->focusSlot()));
        }));
    cm.add(mk(cmd::FxMix, "fx.mix", "FX",
        [p](float v, CommandSource) {
            if (p.fx) p.fx->setMix(v);
        },
        [p]() {
            if (!p.fx || !p.mixer) return 0;
            using R = mixer::ParameterRegistry;
            return (int)(p.mixer->value(
                R::fxMix(p.fx->focusTarget(), p.fx->focusSlot()))
                * 100.0f);
        }));
    cm.add(mk(cmd::FxSyncToggle, "fx.syncToggle", "FX",
        [p](float, CommandSource) {
            if (p.fx) p.fx->toggleSync();
        }));
    cm.add(mk(cmd::FxDivNext, "fx.divNext", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->divisionStep(1); }));
    cm.add(mk(cmd::FxDivPrev, "fx.divPrev", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->divisionStep(-1); }));
    cm.add(mk(cmd::FxDivSet, "fx.divSet", "FX",
        [p](float v, CommandSource) {
            if (p.fx) p.fx->setDivision((int)v);
        },
        [p]() {
            if (!p.fx || !p.mixer) return 0;
            using R = mixer::ParameterRegistry;
            return (int)p.mixer->value(
                R::fxDivision(p.fx->focusTarget(), p.fx->focusSlot()));
        }));
    for (int i = 0; i < 8; ++i) {
        const std::string pn = "fx.p" + std::to_string(i + 1);
        cm.add(mk(cmd::FxParamSetBase + (uint32_t)i, pn + "Set", "FX",
            [p, i](float v, CommandSource) {
                if (p.fx) p.fx->setParam(i, v);
            }));
        cm.add(mk(cmd::FxParamUpBase + (uint32_t)i, pn + "Up", "FX",
            [p, i](float, CommandSource) {
                if (p.fx) p.fx->nudgeParam(i, 0.02f);
            }));
        cm.add(mk(cmd::FxParamDnBase + (uint32_t)i, pn + "Dn", "FX",
            [p, i](float, CommandSource) {
                if (p.fx) p.fx->nudgeParam(i, -0.02f);
            }));
    }
    cm.add(mk(cmd::FxPresetNext, "fx.presetNext", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->presetStep(1); },
        [p]() { return p.fx ? p.fx->presetSel() : -1; }));
    cm.add(mk(cmd::FxPresetPrev, "fx.presetPrev", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->presetStep(-1); }));
    cm.add(mk(cmd::FxPresetApply, "fx.presetApply", "FX",
        [p](float v, CommandSource) {
            if (p.fx) p.fx->applyPreset(v < 0.0f ? -1 : (int)v);
        }));
    for (int i = 0; i < 8; ++i) {
        cm.add(mk(cmd::FxMacroBase + (uint32_t)i,
            "fx.macro" + std::to_string(i), "FX",
            [p, i](float v, CommandSource) {
                if (p.fx) p.fx->macroSet(i, v);
            }));
    }
    cm.add(mk(cmd::FxSlotMoveL, "fx.slotMoveL", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->moveSlot(-1); }));
    cm.add(mk(cmd::FxSlotMoveR, "fx.slotMoveR", "FX",
        [p](float, CommandSource) { if (p.fx) p.fx->moveSlot(1); }));
    cm.add(mk(cmd::FxDiagCopy, "fx.diagCopy", "FX",
        [p](float, CommandSource) {
            if (p.copyFxDiag) p.copyFxDiag();
        }));
}

} // namespace onda::ui
