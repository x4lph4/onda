#include "PerformanceEngine.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace onda {

void PerformanceEngine::setCommandSink(
    std::function<void(const rt::Command&)> sink) {
    sink_ = std::move(sink);
}

void PerformanceEngine::setGridSlot(
    int deck, std::atomic<const BeatGrid*>* pending,
    std::atomic<const BeatGrid*>* retired, const DeckStats* stats) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.pending = pending; d.retired = retired; d.stats = stats;
}

void PerformanceEngine::send(const rt::Command& c) {
    if (sink_) sink_(c);
}

void PerformanceEngine::emit(PerfEventType t, int deck, int slot, double v) {
    PerfEvent e; e.type = t; e.deck = deck; e.slot = slot; e.value = v;
    for (auto& l : listeners_) l(e);
}

int64_t PerformanceEngine::curFrame(int deck) const {
    const auto* s = ctx(deck).stats;
    return s ? s->framesRead.load(std::memory_order_relaxed) : 0;
}

// ============================ deck context =============================

void PerformanceEngine::attachGrid(int deck,
                                   std::shared_ptr<const BeatGrid> g,
                                   int64_t trackId, float confidence) {
    if (deck < 0 || deck >= kMaxDecks || !g) return;
    auto& d = ctx(deck);
    // §24/§32: a different track's grid arriving while synced —
    // sync lock keeps the mode (new grid re-locks to the session
    // clock); without it the deck drops to manual.
    const bool trackChanged = trackId >= 0 && d.trackId != trackId;
    if (trackChanged && d.mode != SyncMode::Off && !d.syncLock)
        setSyncMode(deck, SyncMode::Off);
    if (d.grid && d.grid != g)
        d.retiring.push_back(d.grid);   // freed once RT retires it
    d.grid = std::move(g);
    d.trackId = trackId;
    d.gridConf = std::clamp(confidence, 0.0f, 1.0f);
    if (d.pending)
        d.pending->store(d.grid.get(), std::memory_order_release);
    if (d.mode != SyncMode::Off)
        emit(PerfEventType::SyncStateChanged, deck); // re-anchored

    // Restore persisted performance data for this track.
    if (store_ && trackId >= 0) {
        d.cues.load(store_->loadCues(trackId));
        d.loops = store_->loadLoops(trackId);
    }
    emit(PerfEventType::GridAttached, deck);
}

void PerformanceEngine::detachDeck(int deck) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (d.grid) d.retiring.push_back(d.grid);
    d.grid.reset();
    d.trackId = -1;
    d.cues.clearAll();
    d.loops.clear();
    d.mode = SyncMode::Off;
    d.rolling = false;
}

const BeatGrid* PerformanceEngine::grid(int deck) const {
    if (deck < 0 || deck >= kMaxDecks) return nullptr;
    return ctx(deck).grid.get();
}

// ================================ sync ================================

void PerformanceEngine::setMasterDeck(int deck) {
    if (master_ == deck) return;
    master_ = std::clamp(deck, -1, kMaxDecks - 1);
    rtMasterActive_ = master_ >= 0;
    send(rt::SyncSetMaster{ (uint8_t)(master_ < 0 ? 255 : master_) });
    emit(PerfEventType::MasterDeckChanged, master_);
    for (int i = 0; i < kMaxDecks; ++i)
        if (ctx(i).mode != SyncMode::Off)
            emit(PerfEventType::SyncStateChanged, i);
}

void PerformanceEngine::setSyncMode(int deck, SyncMode mode) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (mode != SyncMode::Off && !hasGrid(deck)) {
        // Deterministic failure: reported, never silently engaged.
        d.mode = SyncMode::Off;
        d.lastError = SyncStatus::NoGrid;
        emit(PerfEventType::SyncStateChanged, deck, -1,
             (double)SyncStatus::NoGrid);
        return;
    }
    d.mode = mode;
    d.lastError = SyncStatus::Off;
    send(rt::SyncSetMode{ (uint8_t)deck, (uint8_t)(int)mode });
    emit(PerfEventType::SyncStateChanged, deck, -1, (double)mode);
}

void PerformanceEngine::enableSync(int deck, SyncMode mode) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (masterSel_.isNone()) {
        // structurally no master — sync cannot engage, deterministically
        d.mode = SyncMode::Off;
        d.lastError = SyncStatus::NoMaster;
        emit(PerfEventType::SyncStateChanged, deck, -1,
             (double)SyncStatus::NoMaster);
        return;
    }
    if (mode != SyncMode::Off && !hasGrid(deck)) {
        // never arm a master/follower without a grid (§30)
        d.mode = SyncMode::Off;
        d.lastError = SyncStatus::NoGrid;
        emit(PerfEventType::SyncStateChanged, deck, -1,
             (double)SyncStatus::NoGrid);
        return;
    }
    if (master_ < 0) {
        if (masterSel_.pinnedDeck() >= 0) {
            setMasterDeck(masterSel_.pinnedDeck());
            // fall through: this deck is a FOLLOWER — needs the RT
            // sync-mode command, not just the control-side field.
        } else {
            // first synced deck = AUTO master: keep the mode field
            // (RT master needs no correction mode) and report.
            d.mode = mode;
            setMasterDeck(deck);
            emit(PerfEventType::SyncStateChanged, deck, -1, (double)mode);
            return;
        }
    }
    setSyncMode(deck, mode); // validates grid + sends SyncSetMode
}

void PerformanceEngine::disableSync(int deck) { setSyncMode(deck, SyncMode::Off); }

void PerformanceEngine::syncDeck(int deck, SyncMode mode) {
    if (ctx(deck).mode == mode) disableSync(deck);
    else                        enableSync(deck, mode);
}

SyncMode PerformanceEngine::syncMode(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).mode : SyncMode::Off;
}

SyncStatus PerformanceEngine::syncStatus(int deck) const {
    if (deck < 0 || deck >= kMaxDecks) return SyncStatus::Off;
    const auto& d = ctx(deck);
    if (d.mode == SyncMode::Off)
        return d.lastError != SyncStatus::Off ? d.lastError
                                              : SyncStatus::Off;
    if (deck == master_)         return SyncStatus::Synced;
    if (master_ < 0)             return SyncStatus::NoMaster;
    if (!hasGrid(deck))          return SyncStatus::NoGrid;
    if (!hasGrid(master_))       return SyncStatus::NoGridMaster;
    const auto* s = d.stats;
    if (!s) return SyncStatus::Off;
    if (s->syncLimited.load(std::memory_order_relaxed))
        return SyncStatus::OutOfRange;
    const int st = s->state.load(std::memory_order_relaxed);
    if (st != (int)DeckState::Playing) return SyncStatus::DeckStopped;
    const float err = std::fabs(s->syncErrMs.load(std::memory_order_relaxed));
    return err < 5.0f ? SyncStatus::Synced : SyncStatus::Syncing;
}

PerformanceEngine::ClockSample PerformanceEngine::masterClock() const {
    return masterClockView().sample;
}

sync::MasterClockView PerformanceEngine::masterClockView() const {
    sync::MasterClockView v;
    // External feed (§5): consulted only when the source is External.
    if (masterSel_.isExternal()) {
        if (externalClock_) {
            v.sample = externalClock_();
            v.sample.external = true;
        }
        return v;
    }
    if (master_ < 0) return v;
    const auto* s = ctx(master_).stats;
    const auto* g = grid(master_);
    v.deck = master_;
    if (!s || !g) return v;
    auto& c = v.sample;
    c.valid   = true;
    c.bpm     = s->effBpm.load(std::memory_order_relaxed);
    c.beatPos = s->beatPos.load(std::memory_order_relaxed);
    c.bar     = (int)s->barPos.load(std::memory_order_relaxed);
    c.phase   = c.beatPos >= 0.0 ? c.beatPos - std::floor(c.beatPos) : 0.0;
    c.beatsPerBar = g->beatsPerBar;
    c.beatDurSec  = c.bpm > 0.0 ? 60.0 / c.bpm : 0.0;
    c.barDurSec   = c.beatDurSec * c.beatsPerBar;
    c.timestampSec = s->positionSec.load(std::memory_order_relaxed);
    v.stopped = s->state.load(std::memory_order_relaxed)
                != (int)DeckState::Playing;
    return v;
}

void PerformanceEngine::setMasterSource(sync::MasterSource s) {
    if (masterSel_.source == s) return;
    masterSel_.source = s;
    if (s == sync::MasterSource::None ||
        s == sync::MasterSource::External) {
        setMasterDeck(-1);
        return;
    }
    if (const int p = masterSel_.pinnedDeck(); p >= 0)
        setMasterDeck(p);
    // AUTO resolves lazily at the next enableSync/poll.
}

void PerformanceEngine::setMasterPausePolicy(sync::MasterPausePolicy p) {
    masterSel_.pause = p;
}

void PerformanceEngine::cycleMaster() {
    using S = sync::MasterSource;
    const int cur = (int)masterSel_.source;
    // Auto(-1) → A → B → C → D → Auto
    const int next = cur < 0 ? 0 : (cur + 1 > 3 ? -1 : cur + 1);
    setMasterSource((S)next);
}

void PerformanceEngine::setExternalClock(sync::ClockSource src) {
    externalClock_ = std::move(src);
}

// ============================== quantize ==============================

void PerformanceEngine::setQuantize(int deck, bool on) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.quantizeOn = on;
    send(rt::SetQuantize{ (uint8_t)deck, on });
    emit(PerfEventType::QuantizeChanged, deck, -1, on ? 1.0 : 0.0);
    if (store_ && d.trackId >= 0)
        store_->setKv("quantize.deck" + std::to_string(deck), on ? "1" : "0");
}

void PerformanceEngine::setQuantizeResolution(int deck, Quantize q) {
    if (deck >= 0 && deck < kMaxDecks) ctx(deck).quantum = q;
}

bool PerformanceEngine::quantizeOn(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).quantizeOn;
}

double PerformanceEngine::quantizeBeatsFor(int deck) const {
    if (deck < 0 || deck >= kMaxDecks) return 1.0;
    const auto& d = ctx(deck);
    const int bpb = d.grid ? d.grid->beatsPerBar : 4;
    return quantizeBeats(d.quantum, bpb);
}

void PerformanceEngine::cycleQuantize(int deck) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.quantum = (Quantize)(((int)d.quantum + 1) % ((int)Quantize::Bar + 1));
    emit(PerfEventType::QuantizeChanged, deck, -1, (double)d.quantum);
}

int PerformanceEngine::quantizeResolution(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? (int)ctx(deck).quantum : 0;
}

// ================================ cues ================================

const std::vector<Cue>& PerformanceEngine::cues(int deck) const {
    static const std::vector<Cue> empty;
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).cues.cues() : empty;
}

Cue PerformanceEngine::setCue(int deck, int slot, CueType type,
                              int64_t frame, int64_t frame2,
                              uint32_t argb, std::string label) {
    auto& d = ctx(deck);
    if (frame < 0) frame = curFrame(deck);
    Cue c = d.cues.setCue(slot, type, frame, grid(deck), frame2, argb,
                          std::move(label), (int64_t)std::time(nullptr));
    persistCue(d, c);
    emit(PerfEventType::CueCreated, deck, slot);
    return c;
}

bool PerformanceEngine::deleteCue(int deck, int slot) {
    auto& d = ctx(deck);
    const Cue* c = d.cues.get(slot);
    if (!c) return false;
    const int type = (int)c->type;
    const bool ok = d.cues.deleteCue(slot);
    if (ok && store_ && d.trackId >= 0)
        store_->deleteCue(d.trackId, slot, type);
    if (ok) emit(PerfEventType::CueDeleted, deck, slot);
    return ok;
}

bool PerformanceEngine::updateCue(int deck, int slot, uint32_t argb,
                                  const std::string& label) {
    auto& d = ctx(deck);
    const bool ok = d.cues.updateCue(slot, argb, label);
    if (ok)
        if (const Cue* c = d.cues.get(slot)) persistCue(d, *c);
    return ok;
}

void PerformanceEngine::clearAllCues(int deck) {
    auto& d = ctx(deck);
    d.cues.clearAll();
    if (store_ && d.trackId >= 0) store_->deleteAllCues(d.trackId);
    emit(PerfEventType::CueDeleted, deck, -2); // -2 = all
}

bool PerformanceEngine::triggerCue(int deck, int slot) {
    auto& d = ctx(deck);
    const Cue* c = d.cues.get(slot);
    if (!c || !d.stats) return false;

    int64_t loopOut = -1;
    const auto target = d.cues.trigger(slot, grid(deck), d.quantizeOn,
                                       quantizeBeats(ctx(deck).quantum),
                                       &loopOut);
    if (!target) return false;

    const bool playing = d.stats->state.load(std::memory_order_relaxed)
                         == (int)DeckState::Playing;
    if (d.slipOn) send(rt::SlipBegin{ (uint8_t)deck });
    send(rt::DeckSeekFrame{ (uint8_t)deck, *target });
    if (c->type == CueType::Loop && loopOut > *target)
        send(rt::SetLoopFrames{ (uint8_t)deck, *target, loopOut });
    if (c->type == CueType::Hot && !playing)
        send(rt::DeckTransport{ (uint8_t)deck, 1 }); // play
    emit(PerfEventType::CueTriggered, deck, slot);
    return true;
}

// ============================= beat jump ==============================

void PerformanceEngine::beatJump(int deck, double beats) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || !d.stats) return;
    const int64_t t = BeatJumpEngine::target(*g, curFrame(deck), beats,
                                             d.quantizeOn);
    if (d.slipOn) send(rt::SlipBegin{ (uint8_t)deck });
    send(rt::DeckSeekFrame{ (uint8_t)deck, t });
    emit(PerfEventType::BeatJumped, deck, -1, beats);
}

// ================================ loops ===============================

void PerformanceEngine::loopIn(int deck) {
    ctx(deck).pendingLoopIn = curFrame(deck);
}

void PerformanceEngine::loopOut(int deck) {
    auto& d = ctx(deck);
    if (d.pendingLoopIn < 0) return;
    int64_t in = d.pendingLoopIn, out = curFrame(deck);
    if (const auto* g = grid(deck); g && d.quantizeOn) {
        in  = QuantizeEngine::snap(*g, in, 1.0);
        out = QuantizeEngine::snap(*g, out, 1.0);
    }
    d.pendingLoopIn = -1;
    if (out <= in) return;
    storeLoopRange(d, in, out);
    send(rt::SetLoopFrames{ (uint8_t)deck, in, out });
    emit(PerfEventType::LoopEntered, deck);
}

void PerformanceEngine::loopBeats(int deck, double beats) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g) return;
    const auto l = SmartLoopEngine::beatLoop(*g, curFrame(deck), beats,
                                             d.quantizeOn);
    if (l.out <= l.in) return;
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
    emit(PerfEventType::LoopEntered, deck);
}

void PerformanceEngine::loopToggle(int deck) {
    auto& d = ctx(deck);
    const bool on = d.stats && d.stats->loopOn.load(std::memory_order_relaxed);
    if (on) exitLoop(deck);
    else    reloop(deck);
}

void PerformanceEngine::reloop(int deck) {
    auto& d = ctx(deck);
    if (d.lastLoopOut <= d.lastLoopIn) return;
    send(rt::SetLoopFrames{ (uint8_t)deck, d.lastLoopIn, d.lastLoopOut });
    send(rt::DeckSeekFrame{ (uint8_t)deck, d.lastLoopIn });
    emit(PerfEventType::LoopEntered, deck);
}

void PerformanceEngine::loopHalve(int deck) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || d.lastLoopOut <= d.lastLoopIn) return;
    const auto l = SmartLoopEngine::halve(*g, {d.lastLoopIn, d.lastLoopOut});
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
}

void PerformanceEngine::loopDouble(int deck) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || d.lastLoopOut <= d.lastLoopIn) return;
    const auto l = SmartLoopEngine::double_(*g, {d.lastLoopIn, d.lastLoopOut});
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
}

void PerformanceEngine::loopMove(int deck, double beats) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || d.lastLoopOut <= d.lastLoopIn) return;
    const auto l = SmartLoopEngine::move(*g, {d.lastLoopIn, d.lastLoopOut},
                                         beats);
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
    send(rt::DeckSeekFrame{ (uint8_t)deck, l.in });
}

void PerformanceEngine::exitLoop(int deck) {
    send(rt::SetLoopOn{ (uint8_t)deck, false });
    if (ctx(deck).rolling) {           // exiting a roll restores the shadow
        ctx(deck).rolling = false;
        send(rt::SlipEnd{ (uint8_t)deck });
    }
    emit(PerfEventType::LoopExited, deck);
}

void PerformanceEngine::loopRoll(int deck, double beats) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || d.rolling) return;
    d.rolling = true;
    send(rt::SlipBegin{ (uint8_t)deck });
    const auto l = SmartLoopEngine::beatLoop(*g, curFrame(deck), beats, true);
    if (l.out > l.in)
        send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
}

void PerformanceEngine::loopRollEnd(int deck) {
    auto& d = ctx(deck);
    if (!d.rolling) return;
    d.rolling = false;
    send(rt::SetLoopOn{ (uint8_t)deck, false });
    send(rt::SlipEnd{ (uint8_t)deck });
    emit(PerfEventType::LoopExited, deck);
}

void PerformanceEngine::storeLoopRange(DeckCtx& d, int64_t in, int64_t out) {
    d.lastLoopIn = in; d.lastLoopOut = out;
}

// ---------------------------- saved loops -----------------------------

void PerformanceEngine::saveLoop(int deck, int slot) {
    auto& d = ctx(deck);
    if (d.lastLoopOut <= d.lastLoopIn) return;
    SavedLoop l;
    l.slot = slot; l.inFrame = d.lastLoopIn; l.outFrame = d.lastLoopOut;
    if (const auto* g = grid(deck)) {
        l.inBeat = g->frameToBeat(l.inFrame);
        l.outBeat = g->frameToBeat(l.outFrame);
    }
    for (auto& e : d.loops) if (e.slot == slot) { e = l; goto saved; }
    d.loops.push_back(l);
saved:
    if (store_ && d.trackId >= 0) store_->saveLoop(d.trackId, l);
    emit(PerfEventType::LoopSaved, deck, slot);
}

void PerformanceEngine::triggerSavedLoop(int deck, int slot) {
    auto& d = ctx(deck);
    for (const auto& l : d.loops) {
        if (l.slot != slot) continue;
        storeLoopRange(d, l.inFrame, l.outFrame);
        send(rt::DeckSeekFrame{ (uint8_t)deck, l.inFrame });
        send(rt::SetLoopFrames{ (uint8_t)deck, l.inFrame, l.outFrame });
        emit(PerfEventType::LoopEntered, deck, slot);
        return;
    }
}

void PerformanceEngine::deleteSavedLoop(int deck, int slot) {
    auto& d = ctx(deck);
    std::erase_if(d.loops, [slot](const SavedLoop& l){ return l.slot==slot; });
    persistLoopDelete(d, slot);
}

std::vector<SavedLoop> PerformanceEngine::savedLoops(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).loops
                                         : std::vector<SavedLoop>{};
}

// ============================ slip / reverse ===========================

void PerformanceEngine::setSlip(int deck, bool on) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (d.slipOn == on) return;
    d.slipOn = on;
    if (on) send(rt::SlipBegin{ (uint8_t)deck });
    else {
        d.rolling = false;
        send(rt::SlipEnd{ (uint8_t)deck });
    }
    emit(PerfEventType::SlipModeChanged, deck, -1, on ? 1.0 : 0.0);
}

bool PerformanceEngine::slipOn(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).slipOn;
}

void PerformanceEngine::setReverse(int deck, bool on) {
    send(rt::SetReverse{ (uint8_t)deck, on });
}

// ==================== sync state / confidence / lock ====================

sync::DeckSyncState PerformanceEngine::deckSyncState(int deck) const {
    sync::DeckSyncState s;
    if (deck < 0 || deck >= kMaxDecks) return s;
    const auto& d = ctx(deck);
    s.enabled   = d.mode != SyncMode::Off;
    s.mode      = d.mode;
    s.isMaster  = master_ == deck;
    s.status    = syncStatus(deck);
    s.tempoLock = s.status == SyncStatus::OutOfRange;
    s.syncLock  = d.syncLock;
    s.confidence = syncConfidence(deck);
    if (const auto* st = d.stats) {
        s.bpm     = st->effBpm.load(std::memory_order_relaxed);
        s.beatPos = st->beatPos.load(std::memory_order_relaxed);
        s.phase   = s.beatPos >= 0.0
                    ? s.beatPos - std::floor(s.beatPos) : 0.0;
    }
    const auto clk = masterClock();
    s.targetBpm = s.isMaster ? s.bpm : (clk.valid ? clk.bpm : 0.0);
    return s;
}

float PerformanceEngine::syncConfidence(int deck) const {
    if (deck < 0 || deck >= kMaxDecks) return 0.0f;
    const auto& d = ctx(deck);
    if (d.mode == SyncMode::Off || !d.grid) return 0.0f;
    float c = d.gridConf;                    // analysis-side quality
    if (deck == master_) return c;           // master doesn't chase
    const auto* s = d.stats;
    if (!s) return c * 0.5f;
    if (s->state.load(std::memory_order_relaxed)
            != (int)DeckState::Playing)
        return c * 0.5f;                     // paused: data stale
    // phase stability: error-free convergence → full confidence
    const float err = std::fabs(s->syncErrMs.load(std::memory_order_relaxed));
    const float stab = 1.0f - std::min(err / 40.0f, 1.0f);
    if (s->syncLimited.load(std::memory_order_relaxed))
        return c * 0.2f;                     // can't reach target
    return c * stab;
}

sync::SyncDiagnostics PerformanceEngine::syncDiagnostics(int deck) const {
    sync::SyncDiagnostics d;
    if (deck < 0 || deck >= kMaxDecks) return d;
    const auto clk = masterClock();
    d.masterDeck = master_;
    d.masterBpm  = clk.valid ? clk.bpm : 0.0;
    d.deck       = deck;
    d.status     = syncStatus(deck);
    d.confidence = syncConfidence(deck);
    d.targetBpm  = d.masterBpm;
    if (const auto* s = ctx(deck).stats) {
        d.deckBpm    = s->effBpm.load(std::memory_order_relaxed);
        d.beat       = s->beatPos.load(std::memory_order_relaxed);
        d.phase      = d.beat >= 0.0 ? d.beat - std::floor(d.beat) : 0.0;
        d.phaseErrMs = s->syncErrMs.load(std::memory_order_relaxed);
        d.limited    = s->syncLimited.load(std::memory_order_relaxed);
        if (clk.valid && d.beat >= 0.0 && clk.beatPos >= 0.0) {
            double off = d.beat - clk.beatPos;
            d.gridOffsetBeats =
                off - std::floor(off + 0.5); // wrap ±0.5 beat
        }
    }
    if (d.targetBpm > 0.0)
        d.tempoErrPct = std::fabs(d.deckBpm - d.targetBpm) / d.targetBpm;
    return d;
}

std::string PerformanceEngine::syncDiagText(int deck) const {
    return syncDiagnostics(deck).toText();
}

void PerformanceEngine::setSyncLock(int deck, bool on) {
    if (deck >= 0 && deck < kMaxDecks) ctx(deck).syncLock = on;
}

bool PerformanceEngine::syncLockOn(int deck) const {
    return deck < 0 || deck >= kMaxDecks || ctx(deck).syncLock;
}

void PerformanceEngine::onUserPitch(int deck, float pct) {
    if (deck < 0 || deck >= kMaxDecks) return;
    const auto& d = ctx(deck);
    const bool synced = d.mode != SyncMode::Off && deck != master_;
    if (!synced) {
        send(rt::SetPitch{ (uint8_t)deck, pct });
        return;
    }
    switch (pitchPolicy_) {
        case sync::PitchPolicy::Disable:
            setSyncMode(deck, SyncMode::Off);   // user takes manual control
            send(rt::SetPitch{ (uint8_t)deck, pct });
            break;
        case sync::PitchPolicy::ManualAsBend:
            send(rt::SetPitchBend{ (uint8_t)deck, pct });
            break;
        case sync::PitchPolicy::Override:
        default:
            break;   // fader ignored while synced
    }
}

// ================================ poll ================================

void PerformanceEngine::poll() {
    // ---- tempo-master maintenance (§4, §22-23) ----
    if (!masterSel_.isNone() && !masterSel_.isExternal() &&
        masterSel_.pinnedDeck() < 0) {
        std::array<bool, 4> synced{}, playing{};
        for (int d = 0; d < 4; ++d) {
            synced[(size_t)d] = ctx(d).mode != SyncMode::Off;
            const auto* s = ctx(d).stats;
            playing[(size_t)d] = s &&
                s->state.load(std::memory_order_relaxed)
                    == (int)DeckState::Playing;
        }
        const int want =
            masterSel_.resolveAuto(synced, playing, master_);
        const bool switchOk = want >= 0 && want != master_ &&
            (master_ < 0 || !synced[(size_t)master_] ||
             (!playing[(size_t)master_] &&
              masterSel_.pause == sync::MasterPausePolicy::FollowOtherDeck));
        if (switchOk) setMasterDeck(want);
    }
    // ClockStops: while the master transport is down the session clock
    // is invalid — followers free-run (RT master = none).
    if (master_ >= 0) {
        const auto* s = ctx(master_).stats;
        const bool mPlaying = s &&
            s->state.load(std::memory_order_relaxed)
                == (int)DeckState::Playing;
        const bool wantRt =
            masterSel_.pause != sync::MasterPausePolicy::ClockStops ||
            mPlaying;
        if (wantRt != rtMasterActive_) {
            rtMasterActive_ = wantRt;
            send(rt::SyncSetMaster{ (uint8_t)(wantRt ? master_ : 255) });
        }
    } else if (rtMasterActive_) {
        rtMasterActive_ = false;
    }

    for (int i = 0; i < kMaxDecks; ++i) {
        auto& d = ctx(i);
        // Release grids the RT thread has retired.
        if (d.retired && !d.retiring.empty()) {
            const BeatGrid* r = d.retired->exchange(nullptr,
                                                    std::memory_order_acq_rel);
            if (r != nullptr)
                std::erase_if(d.retiring,
                    [r](const auto& p){ return p.get() == r; });
        }
        const auto* s = d.stats;
        if (!s) continue;

        const double beat = s->beatPos.load(std::memory_order_relaxed);
        if (beat >= 0.0) {
            const int bi = (int)std::floor(beat);
            if (bi != d.lastBeatInt) {
                d.lastBeatInt = bi;
                emit(PerfEventType::BeatChanged, i, -1, (double)bi);
            }
            const int bar = (int)s->barPos.load(std::memory_order_relaxed);
            if (bar != d.lastBarInt) {
                d.lastBarInt = bar;
                emit(PerfEventType::BarChanged, i, -1, (double)bar);
            }
        }
        const bool loop = s->loopOn.load(std::memory_order_relaxed);
        if (loop != d.lastLoopOn) {
            d.lastLoopOn = loop;
            emit(loop ? PerfEventType::LoopEntered
                      : PerfEventType::LoopExited, i);
        }
        const bool slip = s->slip.load(std::memory_order_relaxed);
        if (slip != d.lastSlip) {
            d.lastSlip = slip;
            // RT-side echo only; slip toggles are already emitted on set.
        }
    }
}

// ============================ persistence ==============================

void PerformanceEngine::persistCue(const DeckCtx& d, const Cue& c) {
    if (store_ && d.trackId >= 0) store_->saveCue(d.trackId, c);
}

void PerformanceEngine::persistLoopDelete(const DeckCtx& d, int slot) {
    if (store_ && d.trackId >= 0) store_->deleteLoop(d.trackId, slot);
}

} // namespace onda
