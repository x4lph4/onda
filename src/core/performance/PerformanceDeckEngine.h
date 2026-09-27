#pragma once

#include "PerformanceEngine.h"

namespace onda {

/// Per-deck controller facade (Phase 28 §1). A lightweight value-type
/// handle over ONE deck of the shared PerformanceEngine — the named
/// subsystems (transport, cues, hot/memory cues, loops, roll, jump,
/// quantize, slip, key lock, reverse, pitch, jog, beatgrid) delegate
/// straight to the engine's per-deck context, so there is exactly one
/// implementation of every action and UI / MIDI / HID / automation
/// share it.
///
///   PerformanceDeckEngine (this handle)
///   ├── transport(Transport)          — §3 single transport impl
///   ├── cueButton / cuePoint          — §5-7 cue engine
///   ├── setCue / triggerCue (hot/mem) — §8-13 cue engines
///   ├── loop* / roll*                 — §14-20 loop engines
///   ├── beatJump / beatJumpIdx        — §23-25
///   ├── quantize*                     — §26-28
///   ├── slip / slipReturn             — §17/21-22
///   ├── keyLock / reverse             — §29-33
///   ├── pitch / pitchRange / bend     — §34-37
///   ├── jog                           — §38-40
///   ├── gridCtl                       — §41 beat-aware authority
///   └── state() → DeckPerfState       — §2 unified snapshot
///
/// Obtain via PerformanceEngine::deck(n). The handle is copyable and
/// cheap — it never outlives the engine by design.
class PerformanceDeckEngine {
public:
    PerformanceDeckEngine() = default;
    PerformanceDeckEngine(PerformanceEngine& e, int deck)
        : e_(&e), deck_(deck) {}

    bool valid() const { return e_ && deck_ >= 0; }
    int  index() const { return deck_; }

    // ---- transport (§3) ----
    void play()       { e_->transport(deck_, Transport::Play); }
    void pause()      { e_->transport(deck_, Transport::Pause); }
    void togglePlay() { e_->transport(deck_, Transport::Toggle); }
    void stop()       { e_->transport(deck_, Transport::Stop); }
    void cuePress()   { e_->transport(deck_, Transport::CuePress); }
    void cueRelease() { e_->transport(deck_, Transport::CueRelease); }
    void restart()    { e_->transport(deck_, Transport::Restart); }
    void brake()      { e_->transport(deck_, Transport::Brake); }
    bool brakeOn()    const { return e_->brakeOn(deck_); }

    // ---- cue (§5-7) ----
    void setCuePoint(int64_t frame = -1) { e_->setCuePoint(deck_, frame); }
    PrimaryCuePoint cuePoint() const { return e_->primaryCuePoint(deck_); }
    void setCueBehavior(CueBehavior b) { e_->setCueBehavior(deck_, b); }
    CueBehavior cueBehavior() const { return e_->cueBehavior(deck_); }

    // ---- hot cues (§8-12) ----
    Cue  setHotCue(int slot, int64_t frame = -1,
                   uint32_t argb = 0xFFFF4444, std::string label = {}) {
        return e_->setCue(deck_, slot, CueType::Hot, frame, -1,
                          argb, std::move(label));
    }
    Cue  setLoopCue(int slot, int64_t in, int64_t out,
                    uint32_t argb = 0xFF4488FF, std::string label = {}) {
        return e_->setCue(deck_, slot, CueType::Loop, in, out,
                          argb, std::move(label));
    }
    bool triggerCue(int slot) { return e_->triggerCue(deck_, slot); }
    bool deleteCue(int slot)  { return e_->deleteCue(deck_, slot); }
    const std::vector<Cue>& cues() const { return e_->cues(deck_); }
    HotCueManager    hot()    { return e_->hot(deck_); }

    // ---- memory cues (§13) ----
    Cue setMemoryCue(int slot, int64_t frame = -1,
                     uint32_t argb = 0xFF33B5E5,
                     std::string label = {}, std::string comment = {}) {
        Cue c = e_->setCue(deck_, slot, CueType::Memory, frame, -1,
                           argb, std::move(label));
        if (!comment.empty()) e_->setCueComment(deck_, slot, comment);
        return c;
    }
    MemoryCueManager memory() { return e_->memory(deck_); }

    // ---- loops (§14-19) ----
    void loopIn()      { e_->loopIn(deck_); }
    void loopOut()     { e_->loopOut(deck_); }
    void loopBeats(double b) { e_->loopBeats(deck_, b); }
    void loopBeatsIdx(int i) { e_->loopBeatsIdx(deck_, i); }
    void loopToggle()  { e_->loopToggle(deck_); }
    void reloop()      { e_->reloop(deck_); }
    void loopHalve()   { e_->loopHalve(deck_); }
    void loopDouble()  { e_->loopDouble(deck_); }
    void loopMove(double beats) { e_->loopMove(deck_, beats); }
    void exitLoop()    { e_->exitLoop(deck_); }

    // ---- loop roll (§20) ----
    void roll(double beats)  { e_->loopRoll(deck_, beats); }
    void rollIdx(int idx)    { e_->loopRollIdx(deck_, idx); }
    void rollEnd()           { e_->loopRollEnd(deck_); }
    bool rollActive() const  { return e_->rollActive(deck_); }

    // ---- beat jump (§23-25) ----
    void beatJump(double beats) { e_->beatJump(deck_, beats); }
    void beatJumpIdx(int idx, int dir = 1) {
        e_->beatJumpIdx(deck_, idx, dir); }

    // ---- quantize (§26-28) ----
    void setQuantize(bool on)        { e_->setQuantize(deck_, on); }
    bool quantizeOn() const          { return e_->quantizeOn(deck_); }
    void setQuantizeRes(Quantize q)  { e_->setQuantizeResolution(deck_, q); }
    void setQuantizeCustom(double b) { e_->setQuantizeCustom(deck_, b); }

    // ---- slip (§17/21-22) ----
    void setSlip(bool on)   { e_->setSlip(deck_, on); }
    bool slipOn() const     { return e_->slipOn(deck_); }
    bool slipActive() const { return e_->slipActive(deck_); }

    // ---- key lock / reverse (§29-33) ----
    void setKeyLock(bool on)   { e_->setKeyLock(deck_, on); }
    bool keyLockOn() const     { return e_->keyLockOn(deck_); }
    void setReverse(bool on)   { e_->setReverse(deck_, on); }
    void reverseHold(bool h)   { e_->reverseMomentary(deck_, h); }
    bool reverseOn() const     { return e_->reverseEffective(deck_); }

    // ---- pitch / tempo (§34-37) ----
    void  onPitch(float pct)   { e_->onUserPitch(deck_, pct); }
    void  setPitchRange(float pct) { e_->setPitchRange(deck_, pct); }
    float cyclePitchRange()    { return e_->cyclePitchRange(deck_); }
    void  pitchReset()         { e_->pitchReset(deck_); }
    void  pitchBend(float pct) { e_->pitchBend(deck_, pct); }

    // ---- jog / scratch (§38-40) ----
    bool jog(DeckJogMode m, double ticks) {
        return e_->jog(deck_, (int)m, ticks); }
    void setJogMode(DeckJogMode m) { e_->setJogMode(deck_, m); }

    // ---- sync ----
    void syncToggle(SyncMode m = SyncMode::Beat) { e_->syncDeck(deck_, m); }

    // ---- beatgrid (§41) ----
    const BeatgridController& gridCtl() const {
        return e_->gridCtl(deck_); }
    const BeatGrid* grid() const { return e_->grid(deck_); }

    // ---- actions + state (§42-44, §2) ----
    ActionState actionState(PerfAction t) const {
        return e_->actionState(deck_, t); }
    DeckPerfState state() const { return e_->deckState(deck_); }

private:
    PerformanceEngine* e_ { nullptr };
    int                deck_ { -1 };
};

inline PerformanceDeckEngine PerformanceEngine::deck(int d) {
    return PerformanceDeckEngine(*this, d);
}

} // namespace onda
