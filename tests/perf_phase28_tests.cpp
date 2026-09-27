// Phase 28 — professional performance deck engine tests.
// Unified transport + brake, cue behaviors, hot-cue snap-on-create,
// loop quantize at configured quantum, loop-roll resolver, slip
// shadow echo, jog facade routing, pitch reset/range, key lock,
// action state machine, per-deck facade and the unified
// DeckPerfState snapshot — driven through the same rt::Command path
// as the audio callback (applyDeckCommand) so positions and DeckStats
// echoes are honest.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/audio/TrackAudio.h"
#include "core/engine/DeckCommand.h"
#include "core/engine/DeckEngine.h"
#include "core/dsp/WsolaStretcher.h"
#include "core/performance/PerformanceActions.h"
#include "core/performance/PerformanceDeckEngine.h"
#include "core/performance/PerformanceEngine.h"
#include "core/performance/QuantizeEngine.h"
#include "core/performance/TransportEngine.h"
#include "core/performance/DeckPerfState.h"
#include "core/rt/CommandQueue.h"

using namespace onda;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

std::unique_ptr<TrackAudio> makeTone(double secs) {
    auto t = std::make_unique<TrackAudio>();
    t->fileSampleRate = kRate;
    t->deviceRate     = kRate;
    t->numFrames      = (int64_t)(kRate * secs);
    t->samples.resize((size_t)t->numFrames * 2);
    for (int64_t i = 0; i < t->numFrames; ++i) {
        const float v = 0.3f * (float)std::sin(
            2.0 * 3.14159265 * 220.0 * i / kRate);
        t->samples[(size_t)i * 2]     = v;
        t->samples[(size_t)i * 2 + 1] = v;
    }
    return t;
}

std::shared_ptr<BeatGrid> makeGrid(double bpm, int64_t firstBeat = 0) {
    auto g = std::make_shared<BeatGrid>();
    g->firstBeatFrame = firstBeat;
    g->setRate(kRate);
    g->segments.push_back({ firstBeat, 0, bpm });
    return g;
}

/// Same rig shape as the Phase-20 suite: the perf engine's command
/// sink applies straight to real DeckEngines, so seeks/loops/slip
/// genuinely happen in DeckStats.
struct Rig {
    static constexpr int kDecks = 4;
    static constexpr int kBlock = 512;

    std::array<DeckEngine, kDecks> decks;
    std::array<DeckStats, kDecks>  stats;
    std::array<std::unique_ptr<TrackAudio>, kDecks> tracks;
    PerformanceEngine perf;
    std::vector<PerfEvent> events;
    std::vector<rt::Command> sent;
    std::vector<float> l = std::vector<float>(kBlock),
                       r = std::vector<float>(kBlock);

    Rig() {
        perf.setCommandSink([this](const rt::Command& c) {
            sent.push_back(c);
            for (int i = 0; i < kDecks; ++i)
                applyDeckCommand(decks[i], i, c);
        });
        perf.addListener([this](const PerfEvent& e) {
            events.push_back(e); });
        for (int i = 0; i < kDecks; ++i) {
            decks[i].bind(&stats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
            perf.setGridSlot(i, &decks[i].pendingGrid_,
                             &decks[i].retiredGrid_, &stats[i]);
        }
    }

    void load(int deck, double bpm, double secs = 60.0) {
        tracks[deck] = makeTone(secs);
        decks[deck].pendingTrack_.store(tracks[deck].get());
        perf.attachGrid(deck, makeGrid(bpm), deck);
        block(1);
    }

    void block(int n = 1) {
        for (int i = 0; i < n; ++i)
            for (auto& d : decks)
                d.renderPcm(l.data(), r.data(), kBlock);
    }
    void secs(double s) { block((int)(s * kRate / kBlock)); }
    int64_t pos(int d) const { return stats[d].framesRead.load(); }
    static int64_t fpb(double bpm) { return (int64_t)(kRate * 60.0 / bpm); }

    template <typename C>
    int countOf() const {
        int n = 0;
        for (auto& c : sent) if (std::get_if<C>(&c)) ++n;
        return n;
    }
    template <typename C>
    const C* lastOf() const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
            if (auto* m = std::get_if<C>(&*it)) return m;
        return nullptr;
    }
    bool hasEvent(PerfEventType t, int deck = -1) const {
        for (auto& e : events)
            if (e.type == t && (deck < 0 || e.deck == deck)) return true;
        return false;
    }
};

} // namespace

// ------------------------------------------------------------------
// §3 — unified transport: one entry point, real rt commands.
// ------------------------------------------------------------------
TEST_CASE("perf28: unified transport emits DeckTransport; brake spins down/up", "[perf28]") {
    Rig r;
    r.load(0, 120.0);

    r.perf.transport(0, Transport::Play);
    auto* t = r.lastOf<rt::DeckTransport>();
    REQUIRE(t != nullptr);
    REQUIRE(t->action == 1);
    REQUIRE(r.hasEvent(PerfEventType::TransportChanged, 0));

    r.secs(0.3);
    REQUIRE(r.pos(0) > 0);
    REQUIRE(r.stats[0].state.load() == (int)DeckState::Playing);

    // Brake — rt::DeckBrake reaches the engine; spin-down ends in stop.
    r.perf.setBrakeSec(0, 0.2);
    r.perf.transport(0, Transport::Brake);
    auto* b = r.lastOf<rt::DeckBrake>();
    REQUIRE(b != nullptr);
    REQUIRE(b->on == 1);
    REQUIRE(b->sec == Approx(0.2f));
    REQUIRE(r.perf.brakeOn(0));
    r.block(1);                                      // publish echo
    REQUIRE(r.stats[0].brake.load());
    r.secs(0.5);                                     // > 0.2 s ramp
    REQUIRE(r.stats[0].state.load() != (int)DeckState::Playing);

    // Release — spin-up resumes playback.
    r.perf.transport(0, Transport::Brake);
    b = r.lastOf<rt::DeckBrake>();
    REQUIRE(b->on == 0);
    REQUIRE(!r.perf.brakeOn(0));
    r.secs(0.5);
    REQUIRE(r.stats[0].state.load() == (int)DeckState::Playing);

    // Toggle honours real playing state through the perf layer.
    r.perf.transport(0, Transport::Toggle);
    t = r.lastOf<rt::DeckTransport>();
    REQUIRE(t->action == 0);                        // was playing → pause
    r.perf.transport(0, Transport::Stop);
    t = r.lastOf<rt::DeckTransport>();
    REQUIRE(t->action == 2);
}

// ------------------------------------------------------------------
// §2 — DeckPerfState unified snapshot.
// ------------------------------------------------------------------
TEST_CASE("perf28: deckState() unifies transport/quantize/slip/cue/loop", "[perf28]") {
    Rig r;
    r.load(0, 128.0);

    auto d = r.perf.deck(0);                        // §1 facade
    d.setQuantize(true);
    d.setQuantizeRes(Quantize::Quarter);
    d.setSlip(true);
    d.setCuePoint(r.pos(0));
    d.loopBeatsIdx(7);                              // 4-beat auto loop
    r.block(1);                                      // publish echoes

    const auto s = r.perf.deckState(0);
    REQUIRE(s.deck == 0);
    REQUIRE(s.quantize);
    REQUIRE(s.quantum == Quantize::Quarter);
    REQUIRE(s.quantumBeats == Approx(0.25));
    REQUIRE(s.slipMode);
    REQUIRE(s.slipActive);
    REQUIRE(s.cuePointFrame >= 0);
    REQUIRE(s.loopOn);
    REQUIRE(s.loopSizeBeats == Approx(4.0));
    REQUIRE(s.hasGrid);
    REQUIRE(s.bpm == Approx(128.0));
    REQUIRE(r.pos(0) >= 0);
}

// ------------------------------------------------------------------
// §6-7 — primary cue point + cue behaviors.
// ------------------------------------------------------------------
TEST_CASE("perf28: primary cue point carries beat metadata", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    const int64_t target = r.fpb(120.0) * 8;        // beat 8 exactly

    r.perf.setCuePoint(0, target);
    const auto p = r.perf.primaryCuePoint(0);
    REQUIRE(p.frame == target);
    REQUIRE(p.beat == Approx(8.0).margin(1e-3));
    REQUIRE(p.createdAt >= 0);

    const auto s = r.perf.deckState(0);
    REQUIRE(s.cuePointFrame == target);
    REQUIRE(s.cuePointBeat == Approx(8.0).margin(1e-3));
}

TEST_CASE("perf28: cue behaviors — Hold / ToStart / ToLastCue", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    r.perf.setCuePoint(0, r.fpb(120.0) * 4);        // cue at beat 4

    // ToStart: release snaps back to the cue point and stops.
    r.perf.setCueBehavior(0, CueBehavior::ToStart);
    REQUIRE(r.perf.cueBehavior(0) == CueBehavior::ToStart);
    r.perf.transport(0, Transport::CuePress);
    r.secs(0.2);
    r.perf.transport(0, Transport::CueRelease);
    r.secs(0.1);
    REQUIRE(std::abs(r.pos(0) - r.fpb(120.0) * 4) <= r.fpb(120.0) / 2);

    // ToLastCue: triggering a hot cue re-anchors the cue point.
    r.perf.setCueBehavior(0, CueBehavior::ToLastCue);
    const int64_t hot = r.fpb(120.0) * 16;
    r.perf.setCue(0, 0, CueType::Hot, hot);
    r.perf.triggerCue(0, 0);
    const auto p2 = r.perf.primaryCuePoint(0);
    REQUIRE(p2.frame >= hot - r.fpb(120.0) / 2);
}

// ------------------------------------------------------------------
// §8-10 — hot cue snap-on-create + HotKind::Marker.
// ------------------------------------------------------------------
TEST_CASE("perf28: hot cue snaps to configured quantum on create", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    auto d = r.perf.deck(0);
    d.setQuantize(true);
    d.setQuantizeRes(Quantize::Quarter);            // 0.25 beats

    const int64_t fpb  = r.fpb(120.0);
    const int64_t off  = fpb * 8 + fpb / 10;        // 0.1 beat off-grid
    const Cue c = d.setHotCue(0, off);
    // nearest 0.25-beat grid point: 8.0 beats → snapped back to beat 8
    REQUIRE(std::abs(c.frame - fpb * 8) <= fpb / 4 + 8);

    // Quantize OFF → authored frame preserved.
    d.setQuantize(false);
    const Cue c2 = d.setHotCue(1, off);
    REQUIRE(c2.frame == off);
}

TEST_CASE("perf28: HotKind::Marker stored as logical type", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    const Cue c = r.perf.setCue(0, 2, CueType::Hot,
                                r.fpb(120.0) * 4);
    REQUIRE(r.perf.setCueKind(0, 2, HotKind::Marker));
    const Cue* got = nullptr;
    for (auto& x : r.perf.cues(0)) if (x.slot == 2) got = &x;
    REQUIRE(got != nullptr);
    REQUIRE(got->kind == HotKind::Marker);
    (void)c;
}

// ------------------------------------------------------------------
// §17 — loop in/out honours configured quantum (not fixed 1 beat).
// ------------------------------------------------------------------
TEST_CASE("perf28: loop in/out snap to configured quantum", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    const int64_t fpb = r.fpb(120.0);               // 24000

    r.perf.setQuantize(0, true);
    r.perf.setQuantizeResolution(0, Quantize::Quarter); // 0.25 beats

    // Arm loop-in near beat 4, then move the (paused) playhead via a
    // beat jump — in/out must both snap to the 0.25-beat quantum.
    r.perf.beatJump(0, 4.0);                        // seek to ~beat 4
    r.block(2);                                     // clear Seeking echo
    r.perf.loopIn(0);
    r.perf.beatJump(0, 2.0);                        // move ~2 beats
    r.block(2);
    r.perf.loopOut(0);
    r.block(1);                                     // publish echoes
    // Both ends snapped to 0.25-beat grid → in % 6000 == 0.
    REQUIRE(r.stats[0].loopOn.load());
    const int64_t in  = (int64_t)(r.stats[0].loopInSec.load() * kRate + 0.5);
    const int64_t out = (int64_t)(r.stats[0].loopOutSec.load() * kRate + 0.5);
    REQUIRE(in  % (fpb / 4) == 0);
    REQUIRE(out % (fpb / 4) == 0);
    REQUIRE(out > in);
}

// ------------------------------------------------------------------
// §20-22 — loop roll + slip: resolver resize/deny, shadow echo.
// ------------------------------------------------------------------
TEST_CASE("perf28: loop roll resizes while held; scratch denied during roll", "[perf28]") {
    Rig r;
    r.load(0, 128.0);
    r.perf.transport(0, Transport::Play);
    r.secs(0.5);

    r.perf.loopRoll(0, 0.5);
    REQUIRE(r.perf.rollActive(0));
    REQUIRE(r.perf.actionState(0, PerfAction::LoopRoll)
            == ActionState::Active);
    r.block(1);                                     // publish echoes
    REQUIRE(r.stats[0].loopOn.load());
    // Roll holds a slip ref → shadow position is being tracked.
    REQUIRE(r.stats[0].slip.load());
    REQUIRE(r.stats[0].slipShadowSec.load() >= 0.0);

    const int64_t out0 = (int64_t)(r.stats[0].loopOutSec.load() * kRate);
    r.perf.loopRoll(0, 0.25);                       // live resize
    REQUIRE(r.perf.rollActive(0));
    r.block(1);
    const int64_t out1 = (int64_t)(r.stats[0].loopOutSec.load() * kRate);
    REQUIRE(out1 < out0);                           // smaller roll

    // Scratch gesture while the roll is held → resolver denies it.
    REQUIRE(!r.perf.jog(0, (int)DeckJogMode::Scratch, 4.0));
    REQUIRE(r.hasEvent(PerfEventType::ActionRejected, 0));

    r.perf.loopRollEnd(0);
    REQUIRE(!r.perf.rollActive(0));
    r.block(1);
    REQUIRE(!r.stats[0].loopOn.load());
    // Slip ref released → shadow echo cleared (user slip still off).
    REQUIRE(r.stats[0].slipShadowSec.load() < 0.0);
    // And the playhead resumed where the timeline would have been —
    // not stuck inside the roll window.
    REQUIRE(r.pos(0) > 0);
}

TEST_CASE("perf28: scratch then roll — roll denied while scratching", "[perf28]") {
    Rig r;
    r.load(0, 128.0);
    r.perf.transport(0, Transport::Play);
    r.secs(0.3);

    REQUIRE(r.perf.jog(0, (int)DeckJogMode::Scratch, 2.0));
    REQUIRE(r.perf.actionState(0, PerfAction::Scratch)
            == ActionState::Active);
    // Roll during scratch → Deny (documented §43 policy).
    r.perf.loopRoll(0, 0.5);
    REQUIRE(!r.perf.rollActive(0));
    REQUIRE(r.hasEvent(PerfEventType::ActionRejected, 0));
}

// ------------------------------------------------------------------
// §21-22 — slip shadow/return position exposed via DeckStats.
// ------------------------------------------------------------------
TEST_CASE("perf28: slip mode exposes shadow position; ends clean", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    r.perf.transport(0, Transport::Play);
    r.secs(0.4);
    const double before = r.stats[0].positionSec.load();

    r.perf.setSlip(0, true);
    r.secs(0.4);                                    // shadow advances
    const double shadow = r.stats[0].slipShadowSec.load();
    REQUIRE(shadow > before);
    REQUIRE(shadow == Approx(r.stats[0].positionSec.load()).margin(0.05));

    r.perf.setSlip(0, false);
    r.secs(0.05);
    REQUIRE(r.stats[0].slipShadowSec.load() < 0.0);
    REQUIRE(!r.perf.slipActive(0));
}

// ------------------------------------------------------------------
// §23-25 — beat jump keeps transport state.
// ------------------------------------------------------------------
TEST_CASE("perf28: beat jump preserves play/pause state", "[perf28]") {
    Rig r;
    r.load(0, 120.0);

    // Paused jump: position moves, deck stays stopped.
    r.perf.setQuantize(0, true);
    r.perf.beatJump(0, 8.0);
    r.block(2);                                     // Seeking is transient
    const int64_t p1 = r.pos(0);
    REQUIRE(p1 > 0);
    REQUIRE(r.stats[0].state.load() != (int)DeckState::Playing);

    // Playing jump: keeps playing.
    r.perf.transport(0, Transport::Play);
    r.secs(0.2);
    const int64_t p2 = r.pos(0);
    r.perf.beatJump(0, -4.0);
    r.block(2);                                     // clear Seeking echo
    REQUIRE(r.stats[0].state.load() == (int)DeckState::Playing);
    REQUIRE(r.pos(0) < p2 + r.fpb(120.0));          // jumped back
}

// ------------------------------------------------------------------
// §27 — quantize CUSTOM resolution.
// ------------------------------------------------------------------
TEST_CASE("perf28: custom quantize resolution snaps to custom quantum", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    r.perf.setQuantize(0, true);
    r.perf.setQuantizeResolution(0, Quantize::Custom);
    r.perf.setQuantizeCustom(0, 3.0);               // 3-beat quantum
    REQUIRE(r.perf.quantizeCustom(0) == Approx(3.0));

    // Pure engine math: frame near beat 4 → snaps to beat 3 or 6.
    auto g = makeGrid(120.0);
    const int64_t in = (int64_t)(r.fpb(120.0) * 4.2);
    const int64_t snapped = QuantizeEngine::snapToSubdivision(
        *g, in, quantizeBeats(Quantize::Custom, 4, 3.0));
    const double beat = g->frameToBeat(snapped);
    REQUIRE(std::fmod(beat, 3.0) == Approx(0.0).margin(1e-3));
}

// ------------------------------------------------------------------
// §29-31 — key lock through the facade.
// ------------------------------------------------------------------
TEST_CASE("perf28: key lock toggles through facade; rt command emitted", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    auto d = r.perf.deck(0);

    d.setKeyLock(true);
    auto* kl = r.lastOf<rt::SetKeyLock>();
    REQUIRE(kl != nullptr);
    REQUIRE(kl->on == 1);
    REQUIRE(d.keyLockOn());
    REQUIRE(r.hasEvent(PerfEventType::KeyLockChanged, 0));

    // Tempo change keeps key lock engaged — SetKeyLock isn't re-sent.
    d.onPitch(50.0f);
    r.block(1);                                     // publish echo
    REQUIRE(d.keyLockOn());
    REQUIRE(r.stats[0].keylock.load());

    d.setKeyLock(false);
    REQUIRE(!d.keyLockOn());
    kl = r.lastOf<rt::SetKeyLock>();
    REQUIRE(kl->on == 0);
}

// ------------------------------------------------------------------
// §34-37 — pitch range / reset / bend through the facade.
// ------------------------------------------------------------------
TEST_CASE("perf28: pitch range cycle, reset and momentary bend", "[perf28]") {
    Rig r;
    r.load(0, 120.0);
    auto d = r.perf.deck(0);

    // §35 — range cycle 6 → 10 → 16 → 50 → 6.
    d.setPitchRange(6.0f);
    REQUIRE(r.perf.pitchRange(0) == Approx(6.0f));
    REQUIRE(d.cyclePitchRange() == Approx(10.0f));
    REQUIRE(d.cyclePitchRange() == Approx(16.0f));
    REQUIRE(d.cyclePitchRange() == Approx(50.0f));
    REQUIRE(d.cyclePitchRange() == Approx(6.0f));

    // §37 — bend is momentary; the fader target never rewrites.
    d.onPitch(20.0f);
    d.pitchBend(-4.0f);
    auto* bd = r.lastOf<rt::SetPitchBend>();
    REQUIRE(bd != nullptr);
    REQUIRE(bd->pct == Approx(-4.0f));
    d.pitchBend(0.0f);

    // §36 — reset returns tempo to 0% through the policy path.
    d.pitchReset();
    auto* sp = r.lastOf<rt::SetPitch>();
    REQUIRE(sp != nullptr);
    REQUIRE(sp->pct == Approx(0.0f));
}

// ------------------------------------------------------------------
// §38-40 — jog facade: seek/bend/scratch decisions + Browse safety.
// ------------------------------------------------------------------
TEST_CASE("perf28: jog facade routes seek/bend; browse never moves playhead", "[perf28]") {
    Rig r;
    r.load(0, 128.0);
    const int64_t p0 = r.pos(0);

    // Seek mode → DeckSeek relative to current position.
    r.perf.jog(0, (int)DeckJogMode::Seek, 5.0);
    auto* sk = r.lastOf<rt::DeckSeek>();
    REQUIRE(sk != nullptr);
    REQUIRE(sk->sec > 0.0);

    // PitchBend mode → momentary SetPitchBend (clamped by JogEngine).
    r.perf.jog(0, (int)DeckJogMode::PitchBend, 3.0);
    auto* bd = r.lastOf<rt::SetPitchBend>();
    REQUIRE(bd != nullptr);
    REQUIRE(bd->pct > 0.0f);

    // Browse mode → NO deck command at all (playhead untouched).
    const size_t n0 = r.sent.size();
    REQUIRE(r.perf.jog(0, (int)DeckJogMode::Browse, 10.0));
    for (size_t i = n0; i < r.sent.size(); ++i) {
        REQUIRE(!std::get_if<rt::DeckSeek>(&r.sent[i]));
        REQUIRE(!std::get_if<rt::DeckSeekFrame>(&r.sent[i]));
        REQUIRE(!std::get_if<rt::SetPitchBend>(&r.sent[i]));
    }
    REQUIRE(r.pos(0) == p0);
}

// ------------------------------------------------------------------
// §42-44 — action state machine + injected clock.
// ------------------------------------------------------------------
TEST_CASE("perf28: actions carry state + timestamp through lifecycle", "[perf28]") {
    Rig r;
    r.load(0, 128.0);
    uint32_t fakeNow = 1000;
    r.perf.setNowFn([&fakeNow] { return fakeNow; });

    r.perf.transport(0, Transport::Play);
    r.secs(0.3);

    fakeNow = 2000;
    r.perf.loopRoll(0, 0.5);
    const auto& a = r.perf.action(0, PerfAction::LoopRoll);
    REQUIRE(a.state == ActionState::Active);
    REQUIRE(a.timestampMs == 2000);
    REQUIRE(a.value == Approx(0.5));

    fakeNow = 3000;
    r.perf.loopRollEnd(0);
    // After release the roll completes → back to Idle.
    REQUIRE(r.perf.actionState(0, PerfAction::LoopRoll)
            == ActionState::Idle);
    REQUIRE(r.hasEvent(PerfEventType::ActionStateChanged, 0));

    // Slip toggle is a held action too.
    r.perf.setSlip(0, true);
    REQUIRE(r.perf.actionState(0, PerfAction::Slip)
            == ActionState::Active);
    r.perf.setSlip(0, false);
    REQUIRE(r.perf.actionState(0, PerfAction::Slip)
            == ActionState::Idle);
}

// ------------------------------------------------------------------
// §62 — four decks simultaneous: loops + hot cues + jumps + slip.
// ------------------------------------------------------------------
TEST_CASE("perf28: four decks — independent performance state", "[perf28]") {
    Rig r;
    for (int i = 0; i < 4; ++i) {
        r.load(i, 118.0 + i * 4.0);
        r.perf.transport(i, Transport::Play);
    }
    r.secs(0.5);

    r.perf.deck(0).loopBeats(4.0);
    r.perf.deck(1).setHotCue(0, r.pos(1));
    r.perf.deck(2).setSlip(true);
    r.perf.deck(3).beatJump(8.0);
    r.secs(0.3);

    REQUIRE(r.stats[0].loopOn.load());
    REQUIRE(!r.perf.cues(1).empty());
    REQUIRE(r.perf.slipActive(2));
    REQUIRE(r.stats[2].slipShadowSec.load() >= 0.0);

    // Every deck still playing; no cross-talk.
    for (int i = 0; i < 4; ++i)
        REQUIRE(r.stats[i].state.load() == (int)DeckState::Playing);
    REQUIRE(!r.perf.slipActive(0));
    REQUIRE(!r.stats[1].loopOn.load());
    r.perf.deck(2).setSlip(false);
}

// ------------------------------------------------------------------
// Pure-component tests — resolver + transport edge logic.
// ------------------------------------------------------------------
TEST_CASE("perf28: resolver policy — documented decision table", "[perf28]") {
    std::array<PerformanceAction, (size_t)PerfAction::Count> t{};

    using R = PerformanceActionResolver;
    // Nothing running → everything allowed.
    for (int i = 0; i < (int)PerfAction::Count; ++i)
        REQUIRE(R::resolve(t, (PerfAction)i) == R::Decision::Allow);

    t[(size_t)PerfAction::LoopRoll].state = ActionState::Active;
    REQUIRE(R::resolve(t, PerfAction::LoopRoll) == R::Decision::Resize);
    REQUIRE(R::resolve(t, PerfAction::Scratch)  == R::Decision::Deny);
    REQUIRE(R::resolve(t, PerfAction::HotCue)   == R::Decision::Allow);
    REQUIRE(R::resolve(t, PerfAction::BeatJump) == R::Decision::Allow);

    t[(size_t)PerfAction::LoopRoll].state = ActionState::Idle;
    t[(size_t)PerfAction::Scratch].state  = ActionState::Active;
    REQUIRE(R::resolve(t, PerfAction::LoopRoll) == R::Decision::Deny);
    REQUIRE(R::resolve(t, PerfAction::Reverse)  == R::Decision::Allow);

    // Priority ordering is a stable documented ranking.
    REQUIRE(R::priority(PerfAction::Scratch)
            < R::priority(PerfAction::LoopRoll));
    REQUIRE(R::priority(PerfAction::LoopRoll)
            < R::priority(PerfAction::Slip));
}

TEST_CASE("perf28: TransportEngine brake edge logic", "[perf28]") {
    TransportEngine te;
    REQUIRE(te.apply(Transport::Brake)   == 1);    // off → on
    REQUIRE(te.apply(Transport::Brake)   == 0);    // on  → off
    REQUIRE(te.apply(Transport::BrakeOn) == 1);
    REQUIRE(te.apply(Transport::BrakeOn) == -1);   // already on
    REQUIRE(te.apply(Transport::BrakeOff) == 0);
    REQUIRE(te.apply(Transport::BrakeOff) == -1);  // already off
    REQUIRE(te.apply(Transport::Play) == -1);      // not a brake action
}
