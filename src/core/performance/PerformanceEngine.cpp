#include "PerformanceEngine.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace onda {

void PerformanceEngine::setCommandSink(
    std::function<void(const rt::Command&)> sink) {
    sink_ = std::move(sink);
    sched_.setSink(sink_);               // scheduler owns the tap now
}

void PerformanceEngine::setStore(PerformanceStore* store) noexcept {
    store_ = store;
    if (store_)
        for (int d = 0; d < kMaxDecks; ++d) restorePrefs(d);
}

void PerformanceEngine::setGridSlot(
    int deck, std::atomic<const BeatGrid*>* pending,
    std::atomic<const BeatGrid*>* retired, const DeckStats* stats) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.pending = pending; d.retired = retired; d.stats = stats;
}

int PerformanceEngine::deckOf(const rt::Command& c) {
    return std::visit([](const auto& x) -> int {
        if constexpr (requires { x.deck; }) return (int)x.deck;
        return 255;
    }, c);
}

void PerformanceEngine::send(const rt::Command& c) {
    const int deck = deckOf(c);
    diag_.deck(deck & 3).actionsPosted++;
    if (sched_.post(deck, c))
        diag_.deck(deck & 3).rtCmdsSent++;
    else
        diag_.deck(deck & 3).actionsDropped++;
}

void PerformanceEngine::commit() {
    sched_.flush();
}

void PerformanceEngine::reject(int deck, PerfError e, int slot) {
    auto& d = ctx(deck);
    d.lastErr = e;
    diag_.deck(deck).rejects++;
    emit(PerfEventType::ActionRejected, deck, slot, (double)e);
}

PerfError PerformanceEngine::lastError(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).lastErr
                                         : PerfError::NoDeck;
}

void PerformanceEngine::emit(PerfEventType t, int deck, int slot, double v) {
    PerfEvent e; e.type = t; e.deck = deck; e.slot = slot; e.value = v;
    for (auto& l : listeners_) l(e);
}

bool PerformanceEngine::hasTrack(int deck) const {
    const auto* s = ctx(deck).stats;
    return s != nullptr
        && s->state.load(std::memory_order_relaxed)
               != (int)DeckState::Empty;
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
    const bool trackChanged = trackId >= 0 && d.gridCtl.trackId != trackId;
    if (trackChanged && d.mode != SyncMode::Off && !d.syncLock)
        setSyncMode(deck, SyncMode::Off);
    if (d.gridCtl.grid && d.gridCtl.grid != g)
        d.retiring.push_back(d.gridCtl.grid); // freed once RT retires it
    d.gridCtl.grid = std::move(g);
    d.gridCtl.trackId = trackId;
    d.gridCtl.confidence = std::clamp(confidence, 0.0f, 1.0f);
    if (d.pending)
        d.pending->store(d.gridCtl.grid.get(), std::memory_order_release);
    if (d.mode != SyncMode::Off)
        emit(PerfEventType::SyncStateChanged, deck); // re-anchored

    // Restore persisted performance data for this track.
    if (store_ && trackId >= 0) {
        d.cues.load(store_->loadCues(trackId));
        d.loops = store_->loadLoops(trackId);
    }
    // §24 — the temporary cue point belongs to the track; a new grid
    // starts it at the beginning (hot cues came from the store).
    if (trackChanged) { d.cuePoint = {}; d.cuePreview = false; }
    emit(PerfEventType::GridAttached, deck);
}

void PerformanceEngine::detachDeck(int deck) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (d.gridCtl.grid) d.retiring.push_back(d.gridCtl.grid);
    d.gridCtl = {};
    d.cues.clearAll();
    d.loops.clear();
    d.mode = SyncMode::Off;
    // §17 — release every slip ref (user mode + an in-flight roll) so
    // the RT deck never stays shadowed on an unloaded track.
    if (d.slip.active()) send(rt::SlipEnd{ (uint8_t)deck });
    d.slip.clear();
    d.rev.clear();
    d.roll.clear();
    d.jog.clear();
    d.transport.clear();
    d.keyLock.clear();
    d.pitch.clear();
    for (auto& a : d.actions) a.state = ActionState::Idle;
    d.cuePoint = {}; d.cuePreview = false;
    d.activeHotCue = d.activeMemCue = -1;
    d.lastErr = PerfError::Ok;
    commit();
}

const BeatGrid* PerformanceEngine::grid(int deck) const {
    if (deck < 0 || deck >= kMaxDecks) return nullptr;
    return ctx(deck).gridCtl.raw();
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
    commit();
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
    commit();
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
    if (store_ && d.gridCtl.trackId >= 0)
        store_->setKv("quantize.deck" + std::to_string(deck), on ? "1" : "0");
    commit();
}

void PerformanceEngine::setQuantizeResolution(int deck, Quantize q) {
    if (deck >= 0 && deck < kMaxDecks) {
        ctx(deck).quantum = q;
        if (store_)
            store_->setKv("pref.quantumRes.deck" + std::to_string(deck),
                          std::to_string((int)q));
    }
}

bool PerformanceEngine::quantizeOn(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).quantizeOn;
}

double PerformanceEngine::quantizeBeatsFor(int deck) const {
    if (deck < 0 || deck >= kMaxDecks) return 1.0;
    const auto& d = ctx(deck);
    const int bpb = d.gridCtl.beatsPerBar();
    return quantizeBeats(d.quantum, bpb, d.customQuantum);
}

void PerformanceEngine::cycleQuantize(int deck) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    // §27 — the hardware-style cycle steps through the seven named
    // resolutions only; Custom is reached by explicit selection
    // (encoder/config), not by cycling.
    d.quantum = (Quantize)(((int)d.quantum + 1)
                           % ((int)Quantize::Bar + 1));
    if (store_)
        store_->setKv("pref.quantumRes.deck" + std::to_string(deck),
                      std::to_string((int)d.quantum));
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
    // §10 — when Quantize is ON a newly created hot/loop cue snaps to
    // the deck's configured grid. Memory cues are free-form markers:
    // they keep their authored position. Without a grid nothing snaps
    // (the cue still stores its frame; §41 fallback).
    if (d.quantizeOn && type != CueType::Memory && d.gridCtl.has()) {
        const double q = quantizeBeatsFor(deck);
        frame = d.gridCtl.snap(frame, q);
        if (frame2 >= 0) frame2 = d.gridCtl.snap(frame2, q);
        diag_.deck(deck).quantizeSnaps++;
    }
    Cue c = d.cues.setCue(slot, type, frame, grid(deck), frame2, argb,
                          std::move(label), (int64_t)std::time(nullptr));
    persistCue(d, c);
    diag_.deck(deck).cuesSet++;
    emit(PerfEventType::CueCreated, deck, slot);
    return c;
}

bool PerformanceEngine::deleteCue(int deck, int slot) {
    auto& d = ctx(deck);
    const Cue* c = d.cues.get(slot);
    if (!c) return false;
    const int type = (int)c->type;
    const bool ok = d.cues.deleteCue(slot);
    if (ok && store_ && d.gridCtl.trackId >= 0)
        store_->deleteCue(d.gridCtl.trackId, slot, type);
    if (ok) { diag_.deck(deck).cuesDeleted++;
              emit(PerfEventType::CueDeleted, deck, slot); }
    else reject(deck, PerfError::EmptySlot, slot);
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
    if (store_ && d.gridCtl.trackId >= 0) store_->deleteAllCues(d.gridCtl.trackId);
    emit(PerfEventType::CueDeleted, deck, -2); // -2 = all
}

bool PerformanceEngine::triggerCue(int deck, int slot) {
    if (deck < 0 || deck >= kMaxDecks) { return false; }
    auto& d = ctx(deck);
    const Cue* c = d.cues.get(slot);
    if (!c)            { reject(deck, PerfError::EmptySlot, slot); return false; }
    if (!c->enabled)   { reject(deck, PerfError::Disabled, slot);  return false; }
    if (!hasTrack(deck)) { reject(deck, PerfError::NoTrack, slot); return false; }

    int64_t loopOut = -1;
    const auto target = d.cues.trigger(slot, grid(deck), d.quantizeOn,
                                       quantizeBeats(ctx(deck).quantum, ctx(deck).gridCtl.beatsPerBar(), ctx(deck).customQuantum),
                                       &loopOut);
    if (!target) { reject(deck, PerfError::EmptySlot, slot); return false; }
    if (d.quantizeOn && c->quantized && hasGrid(deck))
        diag_.deck(deck).quantizeSnaps++;

    const bool playing = d.stats->state.load(std::memory_order_relaxed)
                         == (int)DeckState::Playing;
    send(rt::DeckSeekFrame{ (uint8_t)deck, *target });
    // §17: under slip a hot/memory/loop-cue jump is a PERMANENT move —
    // re-anchor the shadow at the NEW position so slip-off later
    // doesn't jump back to the pre-cue timeline. Emitted AFTER the
    // seek: reanchorSlip() snapshots displayedPos(), which must already
    // be the cue target (SlipReanchor is a no-op without slip).
    if (d.slip.active()) send(rt::SlipReanchor{ (uint8_t)deck });
    if (c->type == CueType::Loop && loopOut > *target)
        send(rt::SetLoopFrames{ (uint8_t)deck, *target, loopOut });
    // §5 stopped-deck behaviour: JumpPlay (default) starts playback;
    // JumpCue stays cued. HotKind::Cue forces the cued path.
    // §9 HotKind::Marker NEVER auto-plays — it preserves the current
    // transport (playing stays playing, stopped stays stopped).
    if (c->type == CueType::Hot && !playing &&
        c->kind == HotKind::Instant && d.hotMode == HotCueMode::JumpPlay)
        send(rt::DeckTransport{ (uint8_t)deck, 1 }); // play
    // §7 ToLastCue — the cue point follows the last cue action.
    if (d.cueBehavior == CueBehavior::ToLastCue) {
        d.cuePoint.frame = *target;
        d.cuePoint.beat = d.gridCtl.frameToBeat(*target);
        d.cuePoint.createdAt = (int64_t)std::time(nullptr);
        emit(PerfEventType::CuePointChanged, deck, -1,
             (double)d.cuePoint.frame);
    }
    if (c->type == CueType::Memory) d.activeMemCue = slot;
    else                          d.activeHotCue = slot;
    actionTransition(deck, PerfAction::HotCue, ActionState::Armed,
                     slot, (double)*target);
    actionTransition(deck, PerfAction::HotCue, ActionState::Completed,
                     slot, (double)*target);
    diag_.deck(deck).cuesTriggered++;
    emit(PerfEventType::CueTriggered, deck, slot);
    commit();
    return true;
}

// ====================== cue managers + field edits =====================

HotCueManager PerformanceEngine::hot(int deck) {
    static CueEngine empty;
    return HotCueManager(
        deck >= 0 && deck < kMaxDecks ? ctx(deck).cues : empty);
}
MemoryCueManager PerformanceEngine::memory(int deck) {
    static CueEngine empty;
    return MemoryCueManager(
        deck >= 0 && deck < kMaxDecks ? ctx(deck).cues : empty);
}

std::vector<const Cue*> PerformanceEngine::hotCues(int deck) const {
    std::vector<const Cue*> out;
    if (deck < 0 || deck >= kMaxDecks) return out;
    for (auto& c : ctx(deck).cues.cues())
        if (c.type != CueType::Memory) out.push_back(&c);
    return out;
}
std::vector<const Cue*> PerformanceEngine::memoryCues(int deck) const {
    std::vector<const Cue*> out;
    if (deck < 0 || deck >= kMaxDecks) return out;
    for (auto& c : ctx(deck).cues.cues())
        if (c.type == CueType::Memory) out.push_back(&c);
    return out;
}

bool PerformanceEngine::moveCue(int deck, int slot, int64_t frame) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    if (!d.cues.moveCue(slot, frame, grid(deck))) {
        reject(deck, PerfError::EmptySlot, slot); return false; }
    if (const Cue* c = d.cues.get(slot)) persistCue(d, *c);
    emit(PerfEventType::CueUpdated, deck, slot);
    return true;
}
bool PerformanceEngine::renameCue(int deck, int slot,
                                  const std::string& label) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    const Cue* c = d.cues.get(slot);
    if (!c) { reject(deck, PerfError::EmptySlot, slot); return false; }
    if (!d.cues.updateCue(slot, c->argb, label)) return false;
    if ((c = d.cues.get(slot))) persistCue(d, *c);
    emit(PerfEventType::CueUpdated, deck, slot);
    return true;
}
bool PerformanceEngine::recolorCue(int deck, int slot, uint32_t argb) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    const Cue* c = d.cues.get(slot);
    if (!c) { reject(deck, PerfError::EmptySlot, slot); return false; }
    if (!d.cues.updateCue(slot, argb, c->label)) return false;
    if ((c = d.cues.get(slot))) persistCue(d, *c);
    emit(PerfEventType::CueUpdated, deck, slot);
    return true;
}
bool PerformanceEngine::setCueEnabled(int deck, int slot, bool on) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    if (!d.cues.setEnabled(slot, on)) {
        reject(deck, PerfError::EmptySlot, slot); return false; }
    if (const Cue* c = d.cues.get(slot)) persistCue(d, *c);
    emit(PerfEventType::CueUpdated, deck, slot);
    return true;
}
bool PerformanceEngine::setCueKind(int deck, int slot, HotKind k) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    if (!d.cues.setKind(slot, k)) {
        reject(deck, PerfError::EmptySlot, slot); return false; }
    if (const Cue* c = d.cues.get(slot)) persistCue(d, *c);
    emit(PerfEventType::CueUpdated, deck, slot);
    return true;
}
bool PerformanceEngine::setCueQuantized(int deck, int slot, bool on) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    if (!d.cues.setQuantized(slot, on)) {
        reject(deck, PerfError::EmptySlot, slot); return false; }
    if (const Cue* c = d.cues.get(slot)) persistCue(d, *c);
    emit(PerfEventType::CueUpdated, deck, slot);
    return true;
}
bool PerformanceEngine::setCueComment(int deck, int slot,
                                      std::string comment) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    if (!d.cues.setComment(slot, std::move(comment))) {
        reject(deck, PerfError::EmptySlot, slot); return false; }
    if (const Cue* c = d.cues.get(slot)) persistCue(d, *c);
    emit(PerfEventType::CueUpdated, deck, slot);
    return true;
}

void PerformanceEngine::setHotCueMode(int deck, HotCueMode m) {
    if (deck >= 0 && deck < kMaxDecks) ctx(deck).hotMode = m;
}
HotCueMode PerformanceEngine::hotCueMode(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).hotMode
                                         : HotCueMode::JumpPlay;
}

// ==================== cue point + cue button (§24-26) ==================

void PerformanceEngine::setCuePoint(int deck, int64_t frame) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.cuePoint.frame = frame >= 0 ? frame : curFrame(deck);
    d.cuePoint.beat = d.gridCtl.frameToBeat(d.cuePoint.frame);
    d.cuePoint.createdAt = (int64_t)std::time(nullptr);
    emit(PerfEventType::CuePointChanged, deck, -1,
         (double)d.cuePoint.frame);
}
int64_t PerformanceEngine::cuePoint(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).cuePoint.frame : -1;
}

PrimaryCuePoint PerformanceEngine::primaryCuePoint(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).cuePoint
                                         : PrimaryCuePoint{};
}

void PerformanceEngine::setCueBehavior(int deck, CueBehavior b) {
    if (deck >= 0 && deck < kMaxDecks) {
        ctx(deck).cueBehavior = b;
        if (store_)
            store_->setKv("pref.cueBehavior.deck" + std::to_string(deck),
                          std::to_string((int)b));
    }
}
CueBehavior PerformanceEngine::cueBehavior(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).cueBehavior
                                         : CueBehavior::Hold;
}

void PerformanceEngine::cueButton(int deck, CuePhase ph) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (!hasTrack(deck)) { reject(deck, PerfError::NoTrack); return; }
    const bool playing = d.stats->state.load(std::memory_order_relaxed)
                         == (int)DeckState::Playing;
    if (ph == CuePhase::Press) {
        diag_.deck(deck).cueButtonPress++;
        if (playing) {
            // §7 — all behaviours: CUE while playing returns to the
            // cue point and pauses.
            const int64_t cp = d.cuePoint.frame >= 0 ? d.cuePoint.frame : 0;
            send(rt::DeckSeekFrame{ (uint8_t)deck, cp });
            send(rt::DeckTransport{ (uint8_t)deck, 0 }); // pause
            d.cuePreview = false;
        } else if (d.cueBehavior == CueBehavior::Hold) {
            // CUE while paused = the press DEFINES the cue point and
            // previews from it while held (classic hold behaviour).
            setCuePoint(deck, -1);
            send(rt::DeckTransport{ (uint8_t)deck, 1 }); // preview play
            d.cuePreview = true;
        } else {
            // §7 ToStart / ToLastCue — the cue point is NOT redefined
            // here; the press seeks to it and previews while held.
            const int64_t cp = d.cuePoint.frame >= 0 ? d.cuePoint.frame : 0;
            send(rt::DeckSeekFrame{ (uint8_t)deck, cp });
            send(rt::DeckTransport{ (uint8_t)deck, 1 }); // preview play
            d.cuePreview = true;
        }
    } else { // Release
        if (d.cuePreview) {
            const int64_t cp = d.cuePoint.frame >= 0 ? d.cuePoint.frame : 0;
            send(rt::DeckTransport{ (uint8_t)deck, 0 }); // stop
            send(rt::DeckSeekFrame{ (uint8_t)deck, cp }); // return
            d.cuePreview = false;
        }
    }
    commit();
}

// ============================ undo support =============================

void PerformanceEngine::restoreCue(int deck, const Cue& c) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.cues.setCue(c.slot, c.type, c.frame, grid(deck), c.frame2,
                  c.argb, c.label, c.createdAt);
    d.cues.setKind(c.slot, c.kind);
    d.cues.setEnabled(c.slot, c.enabled);
    d.cues.setQuantized(c.slot, c.quantized);
    d.cues.setComment(c.slot, c.comment);
    if (const Cue* p = d.cues.get(c.slot)) persistCue(d, *p);
    emit(PerfEventType::CueCreated, deck, c.slot);
}

void PerformanceEngine::restoreLoop(int deck, const SavedLoop& l) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    for (auto& e : d.loops) if (e.slot == l.slot) { e = l; goto saved; }
    d.loops.push_back(l);
saved:
    if (store_ && d.gridCtl.trackId >= 0) store_->saveLoop(d.gridCtl.trackId, l);
    emit(PerfEventType::LoopSaved, deck, l.slot);
}

void PerformanceEngine::loopRange(int deck, int64_t& in,
                                  int64_t& out) const {
    in = out = -1;
    if (deck < 0 || deck >= kMaxDecks) return;
    const auto& d = ctx(deck);
    in = d.lastLoopIn; out = d.lastLoopOut;
}
void PerformanceEngine::restoreLoopRange(int deck, int64_t in,
                                         int64_t out, bool on) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (on && out > in) {
        storeLoopRange(d, in, out);
        send(rt::SetLoopFrames{ (uint8_t)deck, in, out });
    } else {
        send(rt::SetLoopOn{ (uint8_t)deck, false });
        d.lastLoopIn = in; d.lastLoopOut = out;
    }
    commit();
}

void PerformanceEngine::instantRestart(int deck) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (!hasTrack(deck)) { reject(deck, PerfError::NoTrack); return; }
    // §26 — restart from cue WITHOUT touching sync/master state.
    const int64_t cp = d.cuePoint.frame >= 0 ? d.cuePoint.frame : 0;
    send(rt::DeckSeekFrame{ (uint8_t)deck, cp });
    if (d.slip.active()) send(rt::SlipReanchor{ (uint8_t)deck });
    send(rt::DeckTransport{ (uint8_t)deck, 1 }); // play
    diag_.deck(deck).instantRestart++;
    emit(PerfEventType::CueTriggered, deck, -1);
    commit();
}

// ============================= beat jump ==============================

void PerformanceEngine::beatJump(int deck, double beats) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g)       { reject(deck, PerfError::NoGrid);  return; }
    if (!hasTrack(deck)) { reject(deck, PerfError::NoTrack); return; }
    // §20 — the target is a beat offset resolved through the grid;
    // playback continues because only a faded seek is posted.
    const int64_t t = BeatJumpEngine::target(*g, curFrame(deck), beats,
                                             d.quantizeOn);
    if (d.quantizeOn) diag_.deck(deck).quantizeSnaps++;
    send(rt::DeckSeekFrame{ (uint8_t)deck, t });
    if (d.slip.active()) send(rt::SlipReanchor{ (uint8_t)deck });
    diag_.deck(deck).beatJumps++;
    emit(PerfEventType::BeatJumped, deck, -1, beats);
    commit();
}

void PerformanceEngine::beatJumpIdx(int deck, int idx, int dir) {
    beatJump(deck, (dir >= 0 ? 1.0 : -1.0) * LoopSizeController::jumpSize(idx));
}
void PerformanceEngine::setJumpSizeIndex(int deck, int idx) {
    if (deck >= 0 && deck < kMaxDecks)
        ctx(deck).jumpSizeIdx =
            LoopSizeController::stepIndex(idx, 0,
                                          LoopSizeController::jumpCount());
}
int PerformanceEngine::jumpSizeIndex(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).jumpSizeIdx : 2;
}
double PerformanceEngine::jumpSizeBeats(int deck) const {
    return LoopSizeController::jumpSize(jumpSizeIndex(deck));
}

// ================================ loops ===============================

void PerformanceEngine::loopIn(int deck) {
    ctx(deck).pendingLoopIn = curFrame(deck);
}

void PerformanceEngine::loopOut(int deck) {
    auto& d = ctx(deck);
    if (d.pendingLoopIn < 0) return;
    int64_t in = d.pendingLoopIn, out = curFrame(deck);
    // §17 — with Quantize ON, loop-in/out align to the deck's
    // configured quantum (1/4…1/32…CUSTOM), not a fixed beat.
    if (const auto* g = grid(deck); g && d.quantizeOn) {
        const double q = quantizeBeatsFor(deck);
        in  = QuantizeEngine::snapToSubdivision(*g, in, q);
        out = QuantizeEngine::snapToSubdivision(*g, out, q);
        diag_.deck(deck).quantizeSnaps += 2;
    }
    d.pendingLoopIn = -1;
    if (out <= in) { reject(deck, PerfError::BadRange); return; }
    storeLoopRange(d, in, out);
    send(rt::SetLoopFrames{ (uint8_t)deck, in, out });
    diag_.deck(deck).loopsEntered++;
    emit(PerfEventType::LoopEntered, deck);
    commit();
}

void PerformanceEngine::loopBeats(int deck, double beats) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g) { reject(deck, PerfError::NoGrid); return; }
    const auto l = SmartLoopEngine::beatLoop(*g, curFrame(deck), beats,
                                             d.quantizeOn);
    if (l.out <= l.in) { reject(deck, PerfError::BadRange); return; }
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
    diag_.deck(deck).loopsEntered++;
    emit(PerfEventType::LoopEntered, deck);
    commit();
}

void PerformanceEngine::loopBeatsIdx(int deck, int idx) {
    const double b = LoopSizeController::loopSize(idx);
    if (deck >= 0 && deck < kMaxDecks)
        ctx(deck).loopSizeIdx =
            LoopSizeController::stepIndex(idx, 0,
                                          LoopSizeController::loopCount());
    loopBeats(deck, b);
}
void PerformanceEngine::setLoopSizeIndex(int deck, int idx) {
    if (deck >= 0 && deck < kMaxDecks)
        ctx(deck).loopSizeIdx =
            LoopSizeController::stepIndex(idx, 0,
                                          LoopSizeController::loopCount());
}
int PerformanceEngine::loopSizeIndex(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).loopSizeIdx : 5;
}
double PerformanceEngine::loopSizeBeats(int deck) const {
    return LoopSizeController::loopSize(loopSizeIndex(deck));
}

void PerformanceEngine::loopToggle(int deck) {
    auto& d = ctx(deck);
    const bool on = d.stats && d.stats->loopOn.load(std::memory_order_relaxed);
    if (on) exitLoop(deck);
    else    reloop(deck);
}

void PerformanceEngine::reloop(int deck) {
    auto& d = ctx(deck);
    if (d.lastLoopOut <= d.lastLoopIn) {
        reject(deck, PerfError::BadRange); return; }
    send(rt::SetLoopFrames{ (uint8_t)deck, d.lastLoopIn, d.lastLoopOut });
    send(rt::DeckSeekFrame{ (uint8_t)deck, d.lastLoopIn });
    diag_.deck(deck).loopsEntered++;
    emit(PerfEventType::LoopEntered, deck);
    commit();
}

void PerformanceEngine::loopHalve(int deck) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || d.lastLoopOut <= d.lastLoopIn) {
        reject(deck, PerfError::BadRange); return; }
    const auto l = SmartLoopEngine::halve(*g, {d.lastLoopIn, d.lastLoopOut});
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
    diag_.deck(deck).loopsResized++;
    commit();
}

void PerformanceEngine::loopDouble(int deck) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || d.lastLoopOut <= d.lastLoopIn) {
        reject(deck, PerfError::BadRange); return; }
    const auto l = SmartLoopEngine::double_(*g, {d.lastLoopIn, d.lastLoopOut});
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
    diag_.deck(deck).loopsResized++;
    commit();
}

void PerformanceEngine::loopMove(int deck, double beats) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g || d.lastLoopOut <= d.lastLoopIn) {
        reject(deck, PerfError::BadRange); return; }
    const auto l = SmartLoopEngine::move(*g, {d.lastLoopIn, d.lastLoopOut},
                                         beats);
    storeLoopRange(d, l.in, l.out);
    send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
    send(rt::DeckSeekFrame{ (uint8_t)deck, l.in });
    diag_.deck(deck).loopsResized++;
    commit();
}

void PerformanceEngine::exitLoop(int deck) {
    auto& d = ctx(deck);
    send(rt::SetLoopOn{ (uint8_t)deck, false });
    // §15/§17 — exiting during a roll releases the ROLL ref only;
    // a user-armed slip keeps its shadow running.
    if (d.roll.armed) {
        d.roll.release();
        actionTransition(deck, PerfAction::LoopRoll,
                         ActionState::Completed);
        if (d.slip.set(SlipEngine::Roll, false))
            send(rt::SlipEnd{ (uint8_t)deck });
    }
    diag_.deck(deck).loopsExited++;
    emit(PerfEventType::LoopExited, deck);
    commit();
}

void PerformanceEngine::loopRoll(int deck, double beats) {
    auto& d = ctx(deck);
    const auto* g = grid(deck);
    if (!g) { reject(deck, PerfError::NoGrid); return; }
    // §43-44 — resolver: a second roll while armed RESIZES the running
    // one (live size change, §18); rolling while scratching is denied.
    switch (PerformanceActionResolver::resolve(d.actions,
                                               PerfAction::LoopRoll)) {
        case PerformanceActionResolver::Decision::Deny:
            reject(deck, PerfError::BadRange);
            return;
        case PerformanceActionResolver::Decision::Resize: {
            const auto l = SmartLoopEngine::resizeBeats(
                *g, {d.roll.inFrame, d.roll.outFrame}, beats);
            if (l.out > l.in) {
                send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
                d.roll.resize(beats, l.out);
            }
            diag_.deck(deck).loopsResized++;
            emit(PerfEventType::LoopRollStarted, deck, -1, beats);
            commit();
            return;
        }
        case PerformanceActionResolver::Decision::Allow:
        default:
            break;
    }
    // §15-16 — the roll holds a slip ref so releasing it resumes the
    // musical position the track "would have reached".
    if (d.slip.set(SlipEngine::Roll, true)) {
        send(rt::SlipBegin{ (uint8_t)deck });
        diag_.deck(deck).slipBegins++;
    }
    const auto l = SmartLoopEngine::beatLoop(*g, curFrame(deck), beats, true);
    if (l.out > l.in) {
        send(rt::SetLoopFrames{ (uint8_t)deck, l.in, l.out });
        d.roll.arm(beats, l.in, l.out);
    }
    actionTransition(deck, PerfAction::LoopRoll, ActionState::Armed);
    actionTransition(deck, PerfAction::LoopRoll, ActionState::Active,
                     -1, beats);
    diag_.deck(deck).loopRolls++;
    emit(PerfEventType::LoopRollStarted, deck, -1, beats);
    commit();
}

void PerformanceEngine::loopRollIdx(int deck, int idx) {
    loopRoll(deck, LoopSizeController::rollSize(idx));
}

void PerformanceEngine::loopRollEnd(int deck) {
    auto& d = ctx(deck);
    if (!d.roll.armed) return;
    actionTransition(deck, PerfAction::LoopRoll, ActionState::Releasing);
    d.roll.release();
    send(rt::SetLoopOn{ (uint8_t)deck, false });
    if (d.slip.set(SlipEngine::Roll, false)) {
        send(rt::SlipEnd{ (uint8_t)deck });
        diag_.deck(deck).slipEnds++;
    }
    actionTransition(deck, PerfAction::LoopRoll, ActionState::Completed);
    emit(PerfEventType::LoopRollEnded, deck);
    emit(PerfEventType::LoopExited, deck);
    commit();
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
    if (store_ && d.gridCtl.trackId >= 0) store_->saveLoop(d.gridCtl.trackId, l);
    emit(PerfEventType::LoopSaved, deck, slot);
}

void PerformanceEngine::triggerSavedLoop(int deck, int slot) {
    auto& d = ctx(deck);
    for (const auto& l : d.loops) {
        if (l.slot != slot) continue;
        storeLoopRange(d, l.inFrame, l.outFrame);
        send(rt::DeckSeekFrame{ (uint8_t)deck, l.inFrame });
        send(rt::SetLoopFrames{ (uint8_t)deck, l.inFrame, l.outFrame });
        diag_.deck(deck).loopsEntered++;
        emit(PerfEventType::LoopEntered, deck, slot);
        commit();
        return;
    }
    reject(deck, PerfError::EmptySlot, slot);
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
    if (d.slip.userOn() == on) return;
    // user-mode toggle = one ref; roll/momentary refs survive —
    // turning slip OFF mid-roll only ends the shadow when the roll
    // ref is the last one standing.
    if (d.slip.set(SlipEngine::UserMode, on)) {
        if (on) { send(rt::SlipBegin{ (uint8_t)deck });
                  diag_.deck(deck).slipBegins++; }
        else    { send(rt::SlipEnd{ (uint8_t)deck });
                  diag_.deck(deck).slipEnds++; }
    }
    actionTransition(deck, PerfAction::Slip,
                     on ? ActionState::Active : ActionState::Completed);
    emit(PerfEventType::SlipModeChanged, deck, -1, on ? 1.0 : 0.0);
    if (store_)
        store_->setKv("pref.slip.deck" + std::to_string(deck),
                      on ? "1" : "0");
    commit();
}

bool PerformanceEngine::slipOn(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).slip.userOn();
}
bool PerformanceEngine::slipActive(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).slip.active();
}

void PerformanceEngine::setReverse(int deck, bool on) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (d.rev.setLatched(on)) {
        // §28 — a direction change under slip only flips the heard
        // output; the shadow keeps advancing on the real timeline.
        send(rt::SetReverse{ (uint8_t)deck, d.rev.effective() });
        diag_.deck(deck).reverses++;
        emit(PerfEventType::ReverseChanged, deck, -1,
             d.rev.effective() ? 1.0 : 0.0);
    }
    commit();
}
void PerformanceEngine::reverseMomentary(int deck, bool held) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (d.rev.setMomentary(held)) {
        send(rt::SetReverse{ (uint8_t)deck, d.rev.effective() });
        actionTransition(deck, PerfAction::Reverse,
                         held ? ActionState::Active
                              : ActionState::Completed);
        diag_.deck(deck).reverses++;
        emit(PerfEventType::ReverseChanged, deck, -1,
             d.rev.effective() ? 1.0 : 0.0);
    }
    commit();
}
bool PerformanceEngine::reverseEffective(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).rev.effective();
}

// ===================== Phase 28 — unified transport (§3) ===============

void PerformanceEngine::transport(int deck, Transport t) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    switch (t) {
        case Transport::Play:
            send(rt::DeckTransport{ (uint8_t)deck, 1 });
            break;
        case Transport::Pause:
            send(rt::DeckTransport{ (uint8_t)deck, 0 });
            break;
        case Transport::Toggle: {
            const bool playing = d.stats &&
                d.stats->state.load(std::memory_order_relaxed)
                    == (int)DeckState::Playing;
            send(rt::DeckTransport{ (uint8_t)deck,
                                    (uint8_t)(playing ? 0 : 1) });
            break;
        }
        case Transport::Stop:
            send(rt::DeckTransport{ (uint8_t)deck, 2 });
            break;
        case Transport::CuePress:
            cueButton(deck, CuePhase::Press);
            return;   // cueButton commits + emits itself
        case Transport::CueRelease:
            cueButton(deck, CuePhase::Release);
            return;
        case Transport::Restart:
            instantRestart(deck);
            return;
        case Transport::Brake:
        case Transport::BrakeOn:
        case Transport::BrakeOff: {
            const int edge = d.transport.apply(t);
            if (edge >= 0)
                send(rt::DeckBrake{ (uint8_t)deck, (uint8_t)edge,
                                    (float)d.transport.brakeSec });
            break;
        }
    }
    emit(PerfEventType::TransportChanged, deck, -1, (double)t);
    commit();
}

bool PerformanceEngine::brakeOn(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).transport.brakeOn;
}

// ===================== Phase 28 — key lock (§29-31) =====================

void PerformanceEngine::setKeyLock(int deck, bool on) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    if (!d.keyLock.set(on)) return;
    send(rt::SetKeyLock{ (uint8_t)deck, on });
    emit(PerfEventType::KeyLockChanged, deck, -1, on ? 1.0 : 0.0);
    if (store_)
        store_->setKv("pref.keylock.deck" + std::to_string(deck),
                      on ? "1" : "0");
    commit();
}
bool PerformanceEngine::keyLockOn(int deck) const {
    return deck >= 0 && deck < kMaxDecks && ctx(deck).keyLock.on;
}
void PerformanceEngine::setKeyLockAlgo(int deck, int algo) {
    if (deck >= 0 && deck < kMaxDecks)
        ctx(deck).keyLock.algo = (KeyLockEngine::Algo)algo;
}

// ===================== Phase 28 — pitch / tempo (§34-37) ===============

void PerformanceEngine::setPitchRange(int deck, float pct) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.pitch.setRange(pct);
    send(rt::SetPitchRange{ (uint8_t)deck, d.pitch.range() });
    if (store_)
        store_->setKv("pref.pitchRange.deck" + std::to_string(deck),
                      std::to_string((double)d.pitch.range()));
    commit();
}
float PerformanceEngine::pitchRange(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).pitch.range()
                                         : 16.0f;
}
float PerformanceEngine::cyclePitchRange(int deck) {
    if (deck < 0 || deck >= kMaxDecks) return 16.0f;
    const float r = ctx(deck).pitch.cycle();
    setPitchRange(deck, r);
    return r;
}
void PerformanceEngine::pitchReset(int deck) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    // §36 — tempo reset is a fader move to 0%, routed through the same
    // sync-aware policy as hardware fader input.
    if (d.mode != SyncMode::Off && deck != master_ &&
        pitchPolicy_ == sync::PitchPolicy::Override)
        return;   // synced deck: reset is a no-op under Override
    send(rt::SetPitch{ (uint8_t)deck, 0.0f });
    emit(PerfEventType::TransportChanged, deck, -1, 0.0);
    commit();
}
void PerformanceEngine::pitchBend(int deck, float pct) {
    if (deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    d.pitch.setBending(pct != 0.0f);
    send(rt::SetPitchBend{ (uint8_t)deck, pct });
    commit();
}

// ===================== Phase 28 — jog / scratch (§38-40) ===============

bool PerformanceEngine::jog(int deck, int mode, double deltaTicks) {
    if (deck < 0 || deck >= kMaxDecks) return false;
    auto& d = ctx(deck);
    const auto m = (DeckJogMode)mode;
    // §43 — the resolver gates scratch vs. held rolls; all other modes
    // pass through.
    if (m == DeckJogMode::Scratch &&
        PerformanceActionResolver::resolve(d.actions,
                                           PerfAction::Scratch)
            == PerformanceActionResolver::Decision::Deny) {
        reject(deck, PerfError::BadRange);
        return false;
    }
    const auto dec = d.jog.tick(m, deltaTicks);
    switch (dec.kind) {
        case JogDecision::Kind::SeekSec: {
            const auto* s = d.stats;
            if (!s) return false;
            const double cur = s->positionSec.load(std::memory_order_relaxed);
            send(rt::DeckSeek{ (uint8_t)deck, cur + dec.deltaSec });
            break;
        }
        case JogDecision::Kind::BendPct:
            send(rt::SetPitchBend{ (uint8_t)deck, dec.bendPct });
            break;
        case JogDecision::Kind::Browse:
        case JogDecision::Kind::None:
        default:
            break;
    }
    // §44 — scratch action bookkeeping.
    if (m == DeckJogMode::Scratch &&
        actionState(deck, PerfAction::Scratch) != ActionState::Active)
        actionTransition(deck, PerfAction::Scratch, ActionState::Active);
    else if (m != DeckJogMode::Scratch &&
             actionState(deck, PerfAction::Scratch) == ActionState::Active)
        actionTransition(deck, PerfAction::Scratch, ActionState::Completed);
    commit();
    return true;
}

void PerformanceEngine::setJogMode(int deck, DeckJogMode m) {
    if (deck >= 0 && deck < kMaxDecks) ctx(deck).jog.mode = m;
}
DeckJogMode PerformanceEngine::jogMode(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).jog.mode
                                         : DeckJogMode::Seek;
}

// ===================== Phase 28 — actions (§42-44) =====================

const PerformanceAction& PerformanceEngine::action(int deck,
                                                   PerfAction t) const {
    static const PerformanceAction empty;
    if (deck < 0 || deck >= kMaxDecks) return empty;
    return ctx(deck).actions[(size_t)t];
}
ActionState PerformanceEngine::actionState(int deck, PerfAction t) const {
    return deck >= 0 && deck < kMaxDecks
         ? ctx(deck).actions[(size_t)t].state : ActionState::Idle;
}

void PerformanceEngine::actionTransition(int deck, PerfAction t,
                                         ActionState s, int slot,
                                         double value) {
    auto& a = ctx(deck).actions[(size_t)t];
    a.type = t; a.state = s; a.slot = slot;
    a.timestampMs = nowMs(); a.value = value;
    if (s == ActionState::Completed || s == ActionState::Idle)
        a.state = ActionState::Idle;   // completed actions retire to idle
    emit(PerfEventType::ActionStateChanged, deck, (int)s, (double)t);
}

// ===================== Phase 28 — quantize custom (§27) ================

void PerformanceEngine::setQuantizeCustom(int deck, double beats) {
    if (deck >= 0 && deck < kMaxDecks) {
        ctx(deck).customQuantum = beats;
        if (store_)
            store_->setKv("pref.quantum.deck" + std::to_string(deck),
                          std::to_string(beats));
    }
}
double PerformanceEngine::quantizeCustom(int deck) const {
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).customQuantum : 1.0;
}

// ===================== Phase 28 — unified state (§2) ===================

DeckPerfState PerformanceEngine::deckState(int deck) const {
    DeckPerfState st;
    if (deck < 0 || deck >= kMaxDecks) return st;
    const auto& d = ctx(deck);
    st.deck = deck;
    st.quantize    = d.quantizeOn;
    st.quantum     = d.quantum;
    st.quantumBeats = quantizeBeatsFor(deck);
    st.slipMode    = d.slip.userOn();
    st.slipActive  = d.slip.active();
    st.reverse     = d.rev.effective();
    st.brake       = d.transport.brakeOn;
    st.syncMode    = d.mode;
    st.syncStatus  = (int)syncStatus(deck);
    st.isMaster    = master_ == deck;
    st.keyLock     = d.keyLock.on;
    st.pitchRange  = d.pitch.range();
    st.cuePointFrame = d.cuePoint.frame;
    st.cuePointBeat  = d.cuePoint.beat;
    st.cueBehavior = (int)d.cueBehavior;
    st.cuePreview  = d.cuePreview;
    st.loopInFrame  = d.lastLoopIn;
    st.loopOutFrame = d.lastLoopOut;
    st.loopSizeBeats = loopSizeBeats(deck);
    st.loopInArmed  = d.pendingLoopIn >= 0;
    st.rollActive   = d.roll.armed;
    st.rollBeats    = d.roll.beats;
    st.activeHotCue = d.activeHotCue;
    st.activeMemCue = d.activeMemCue;
    st.hasGrid     = d.gridCtl.has();
    st.scratching  = actionState(deck, PerfAction::Scratch)
                         == ActionState::Active;
    st.lastError   = (int)d.lastErr;
    for (const auto& c : d.cues.cues()) {
        if (c.slot < 0 || c.slot >= 32) continue;
        if (c.type == CueType::Memory) st.memoryCueMask |= 1 << c.slot;
        else                           st.hotCueMask  |= 1 << c.slot;
    }
    if (const auto* s = d.stats) {
        st.trackState  = s->state.load(std::memory_order_relaxed);
        st.playing     = st.trackState == (int)DeckState::Playing;
        st.paused      = st.trackState == (int)DeckState::Paused;
        st.frame       = s->framesRead.load(std::memory_order_relaxed);
        st.positionSec = s->positionSec.load(std::memory_order_relaxed);
        st.durationSec = s->durationSec.load(std::memory_order_relaxed);
        st.pitchPct    = s->pitchPct.load(std::memory_order_relaxed);
        st.bendPct     = s->bendPct.load(std::memory_order_relaxed);
        st.loopOn      = s->loopOn.load(std::memory_order_relaxed);
        st.loopInSec   = s->loopInSec.load(std::memory_order_relaxed);
        st.loopOutSec  = s->loopOutSec.load(std::memory_order_relaxed);
        st.slipReturnSec = s->slipShadowSec.load(std::memory_order_relaxed);
        st.beatPos     = s->beatPos.load(std::memory_order_relaxed);
        st.barPos      = s->barPos.load(std::memory_order_relaxed);
        st.bpm         = s->bpm.load(std::memory_order_relaxed);
        st.effBpm      = s->effBpm.load(std::memory_order_relaxed);
        // brake echo is the RT truth; the non-RT flag can lag a ramp.
        st.brake       = s->brake.load(std::memory_order_relaxed)
                         || d.transport.brakeOn;
    }
    const auto clk = masterClock();
    st.targetBpm = clk.valid ? clk.bpm : 0.0;
    return st;
}

const BeatgridController& PerformanceEngine::gridCtl(int deck) const {
    static const BeatgridController empty;
    return deck >= 0 && deck < kMaxDecks ? ctx(deck).gridCtl : empty;
}

// ===================== Phase 28 — persisted prefs (§54) ================

void PerformanceEngine::restorePrefs(int deck) {
    if (!store_ || deck < 0 || deck >= kMaxDecks) return;
    auto& d = ctx(deck);
    auto num = [&](const std::string& k, double def) {
        const std::string v = store_->getKv(k, "");
        return v.empty() ? def : std::stod(v);
    };
    d.quantizeOn   = num("pref.quantize.deck" + std::to_string(deck),
                         d.quantizeOn ? 1.0 : 0.0) > 0.5;
    d.quantum      = (Quantize)(int)num("pref.quantumRes.deck" +
                        std::to_string(deck), (double)d.quantum);
    d.customQuantum = num("pref.quantum.deck" + std::to_string(deck),
                          d.customQuantum);
    d.pitch.setRange((float)num("pref.pitchRange.deck" +
                     std::to_string(deck), d.pitch.range()));
    d.cueBehavior  = (CueBehavior)(int)num("pref.cueBehavior.deck" +
                     std::to_string(deck), (double)d.cueBehavior);
    // Modes that carry RT commands restore through the same setters so
    // the deck echoes them — §54 restores durable prefs only, never
    // transient state (scratch/roll/cuePreview are session-ephemeral).
    setQuantize(deck, d.quantizeOn);
    setSlip(deck, num("pref.slip.deck" + std::to_string(deck), 0.0) > 0.5);
    setKeyLock(deck, num("pref.keylock.deck" + std::to_string(deck),
                         0.0) > 0.5);
    send(rt::SetPitchRange{ (uint8_t)deck, d.pitch.range() });
    commit();
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
    if (d.mode == SyncMode::Off || !d.gridCtl.has()) return 0.0f;
    float c = d.gridCtl.confidence;                    // analysis-side quality
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
        commit();
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
    commit();
}

// ================================ poll ================================

void PerformanceEngine::poll() {
    commit();   // §29 — safety flush for anything posted but unflushed
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
        commit();
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
        // §28 brake reconcile — the RT ramp finishing on its own (spin
        // down → paused) is the truth; keep the non-RT flag honest.
        d.transport.brakeOn = s->brake.load(std::memory_order_relaxed);
        // Loop action lifecycle follows the RT loopOn edge — a loop
        // that the RT wrapped/exited also retires the action record.
        if (loop &&
            actionState(i, PerfAction::Loop) != ActionState::Active)
            actionTransition(i, PerfAction::Loop, ActionState::Active);
        else if (!loop &&
                 actionState(i, PerfAction::Loop) == ActionState::Active)
            actionTransition(i, PerfAction::Loop, ActionState::Completed);
    }
}

// ============================ persistence ==============================

void PerformanceEngine::persistCue(const DeckCtx& d, const Cue& c) {
    if (store_ && d.gridCtl.trackId >= 0) store_->saveCue(d.gridCtl.trackId, c);
}

void PerformanceEngine::persistLoopDelete(const DeckCtx& d, int slot) {
    if (store_ && d.gridCtl.trackId >= 0) store_->deleteLoop(d.gridCtl.trackId, slot);
}

} // namespace onda
