// Phase 35 — performance rhythm engine deltas: quantize Previous /
// at-or-before + multi-beat quanta, spec-shaped LoopState/SlipState/
// PerformanceClock snapshots, CueJumpEngine seam, perf parameter
// registry, end-of-track clamps, slip anchors.
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
#include "core/performance/CueJumpEngine.h"
#include "core/performance/PerformanceEngine.h"
#include "core/performance/PerformanceParams.h"
#include "core/performance/PerformanceState.h"
#include "core/performance/QuantizeEngine.h"
#include "core/performance/SmartLoopEngine.h"
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

std::shared_ptr<BeatGrid> makeGrid(double bpm) {
    auto g = std::make_shared<BeatGrid>();
    g->firstBeatFrame = 0;
    g->setRate(kRate);
    g->segments.push_back({ 0, 0, bpm });
    return g;
}

struct Rig {
    static constexpr int kDecks = 4;
    static constexpr int kBlock = 512;

    std::array<DeckEngine, kDecks> decks;
    std::array<DeckStats, kDecks>  stats;
    std::array<std::unique_ptr<TrackAudio>, kDecks> tracks;
    PerformanceEngine perf;
    std::vector<rt::Command> sent;
    std::vector<float> l = std::vector<float>(kBlock),
                       r = std::vector<float>(kBlock);

    Rig() {
        perf.setCommandSink([this](const rt::Command& c) {
            sent.push_back(c);
            for (int i = 0; i < kDecks; ++i)
                applyDeckCommand(decks[i], i, c);
        });
        for (int i = 0; i < kDecks; ++i) {
            decks[i].bind(&stats[i]);
            decks[i].setDeviceRate(kRate);
            decks[i].setStretcher(std::make_unique<WsolaStretcher>());
            decks[i].prepareDsp(kRate);
            perf.setGridSlot(i, &decks[i].pendingGrid_,
                             &decks[i].retiredGrid_, &stats[i]);
        }
    }

    void load(int deck, double bpm, double secs) {
        tracks[deck] = makeTone(secs);
        decks[deck].pendingTrack_.store(tracks[deck].get());
        perf.attachGrid(deck, makeGrid(bpm), deck);
        block(1);
    }
    void seek(int deck, int64_t frame) {
        applyDeckCommand(decks[deck], deck,
                         rt::DeckSeekFrame{(uint8_t)deck, frame});
        block(1);
    }
    void play(int deck) {
        applyDeckCommand(decks[deck], deck,
                         rt::DeckTransport{(uint8_t)deck, 1});
        block(1);
    }
    void block(int n = 1) {
        for (int i = 0; i < n; ++i)
            for (auto& d : decks)
                d.renderPcm(l.data(), r.data(), kBlock);
    }
    void secs(double s) { block((int)(s * kRate / kBlock)); }
    int64_t pos(int d) const { return stats[d].framesRead.load(); }
    static int64_t fpb(double bpm) { return (int64_t)(kRate*60.0/bpm); }

    template <typename C>
    const C* lastOf() const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
            if (auto* m = std::get_if<C>(&*it)) return m;
        return nullptr;
    }
    template <typename C>
    int countOf() const {
        int n = 0;
        for (auto& c : sent) if (std::get_if<C>(&c)) ++n;
        return n;
    }
};

} // namespace

// ============================ quantize engine ============================

TEST_CASE("perf35: quantize modes — Nearest / Next / Previous",
          "[perf35]") {
    auto g = *makeGrid(120.0);             // 24000 frames/beat
    const int64_t pos = 24000 * 3 + 7000;  // beat 3.29

    // Nearest → beat 3 (7000 < 12000)
    REQUIRE(QuantizeEngine::snap(g, pos, 1.0) == 24000 * 3);
    // Next → beat 4 (strict)
    REQUIRE(QuantizeEngine::nextBoundary(g, pos, 1.0) == 24000 * 4);
    // Previous → beat 3 (strict — before the position)
    REQUIRE(QuantizeEngine::previousBoundary(g, pos, 1.0) == 24000 * 3);
    // At-or-before on an exact boundary stays put (non-strict)
    REQUIRE(QuantizeEngine::boundaryAtOrBefore(g, 24000 * 3, 1.0)
            == 24000 * 3);
    // Previous on an exact boundary moves strictly back
    REQUIRE(QuantizeEngine::previousBoundary(g, 24000 * 3, 1.0)
            == 24000 * 2);
    // Previous never goes negative — beat 0 clamps
    REQUIRE(QuantizeEngine::previousBoundary(g, 500, 1.0) >= 0);
    // Sub-beat quantum honored (1/4 note = 6000 frames)
    REQUIRE(QuantizeEngine::previousBoundary(g, pos, 0.25)
            == 24000 * 3 + 6000);
    // §63 — snap error never exceeds half a quantum
    const int64_t err =
        std::llabs(QuantizeEngine::snap(g, pos, 0.25) - pos);
    REQUIRE((double)err <= 0.25 * 24000 / 2.0 + 1.0);
}

TEST_CASE("perf35: multi-beat quanta 2/4/8 beats (§3)", "[perf35]") {
    // appended after Custom — persisted indices unchanged
    REQUIRE((int)Quantize::Custom == 7);
    REQUIRE((int)Quantize::Beat2 == 8);
    REQUIRE((int)Quantize::Beat4 == 9);
    REQUIRE((int)Quantize::Beat8 == 10);
    REQUIRE(quantizeBeats(Quantize::Beat2) == Approx(2.0));
    REQUIRE(quantizeBeats(Quantize::Beat4) == Approx(4.0));
    REQUIRE(quantizeBeats(Quantize::Beat8) == Approx(8.0));

    // facade: selecting Beat4 quantizes loop-in/out to 4-beat grid
    Rig r;
    r.load(0, 120.0, 60.0);
    r.play(0);
    r.seek(0, 24000 * 6 + 9000);            // beat ~6.4
    r.perf.setQuantize(0, true);
    r.perf.setQuantizeResolution(0, Quantize::Beat4);
    r.perf.loopIn(0);
    r.seek(0, 24000 * 11 + 3000);           // beat ~11.1
    r.perf.loopOut(0);
    auto* lf = r.lastOf<rt::SetLoopFrames>();
    REQUIRE(lf != nullptr);
    // in snaps to beat 8 (nearest 4-beat boundary), out to beat 12
    REQUIRE(lf->inFrame  == 24000 * 8);
    REQUIRE(lf->outFrame == 24000 * 12);
}

// ============================ cue jump engine ============================

TEST_CASE("perf35: CueJumpEngine — verbatim or grid-snapped target",
          "[perf35]") {
    auto g = *makeGrid(120.0);
    // quantize off → verbatim frame
    REQUIRE(CueJumpEngine::target(g, 12345, -1.0, 1.0, false) == 12345);
    // quantize on + stored beat → re-resolves through the beat, so
    // beatgrid edits stay honored; beat 5.3 → snaps to 5
    REQUIRE(CueJumpEngine::target(g, 999, 5.3, 1.0, true)
            == 24000 * 5);
    // no stored beat → resolved from the frame (12345 → beat 0.51 →
    // nearest boundary is beat 1)
    REQUIRE(CueJumpEngine::target(g, 12345, -1.0, 1.0, true)
            == 24000);
    REQUIRE(CueJumpEngine::target(g, 11000, -1.0, 1.0, true) == 0);
    // empty grid → verbatim fallback (fallback contract)
    BeatGrid empty;
    REQUIRE(CueJumpEngine::target(empty, 777, 1.0, true) == 777);
}

// ============================ spec snapshots ============================

TEST_CASE("perf35: LoopState snapshot (§8)", "[perf35]") {
    Rig r;
    r.load(0, 120.0, 60.0);
    r.play(0);
    r.seek(0, 24000 * 4);
    r.perf.setQuantize(0, true);
    r.perf.loopBeats(0, 4.0);

    const LoopState st = r.perf.loopState(0);
    REQUIRE(st.start == 24000 * 4);
    REQUIRE(st.end   == 24000 * 8);
    REQUIRE(st.length == 24000 * 4);
    REQUIRE(st.beatLength == Approx(4.0).margin(0.01));
    REQUIRE(st.quantized);
    REQUIRE(st.mode == 0);                    // auto/sized
    r.block(2);
    REQUIRE(r.perf.loopState(0).active);      // RT echo on

    r.perf.loopRoll(0, 0.5);
    REQUIRE(r.perf.loopState(0).mode == 2);   // roll active
    r.perf.loopRollEnd(0);
}

TEST_CASE("perf35: SlipState snapshot + timeline restore (§23/§64)",
          "[perf35]") {
    Rig r;
    r.load(0, 120.0, 60.0);
    r.play(0);
    r.secs(0.5);
    const int64_t anchor = r.pos(0);

    r.perf.setSlip(0, true);
    const SlipState on = r.perf.slipState(0);
    REQUIRE(on.active);
    REQUIRE(on.originalFrame >= anchor);      // anchored near engage
    REQUIRE(on.originalFrame <= anchor + 4096);
    r.block(2);
    REQUIRE(r.stats[0].slip.load());          // RT echo
    REQUIRE(r.stats[0].slipShadowSec.load() >= 0.0);

    // loop intervention under slip, then exit → timeline returns
    r.perf.loopRoll(0, 1.0);
    r.secs(0.5);
    r.perf.loopRollEnd(0);
    REQUIRE(r.perf.slipState(0).active);      // user ref still held
    const double shadow = r.stats[0].slipShadowSec.load();
    r.perf.setSlip(0, false);
    REQUIRE_FALSE(r.perf.slipState(0).active);
    // §64 — the playhead resumes the shadow's musical position
    r.block(2);
    const double posSec = r.stats[0].positionSec.load();
    REQUIRE(posSec == Approx(shadow).margin(0.05));
}

TEST_CASE("perf35: PerformanceClock mirrors the session BeatClock (§2)",
          "[perf35]") {
    Rig r;
    r.load(0, 120.0, 60.0);
    r.play(0);
    REQUIRE_FALSE(r.perf.perfClock().valid);  // no master yet
    r.perf.setMasterDeck(0);
    r.secs(0.25);
    const PerformanceClock c = r.perf.perfClock();
    REQUIRE(c.valid);
    REQUIRE(c.bpm == Approx(120.0).margin(0.5));
    REQUIRE(c.beatPos > 0.0);
    REQUIRE(c.phase >= 0.0);
    REQUIRE(c.phase < 1.0);
    REQUIRE(c.beatsPerBar == 4);
}

// ============================ parameter registry ============================

TEST_CASE("perf35: perf parameter registry (§46)", "[perf35]") {
    REQUIRE(kPerfParamCount == (int)PerfParam::Count);
    // spec keys exist in order
    const char* want[] = {
        "loop.size", "loop.active", "loop.quantize",
        "beatJump.size", "slip.active", "quantize.mode",
        "beatRoll.size" };
    for (int i = 0; i < kPerfParamCount; ++i)
        REQUIRE(std::string(kPerfParams[i].name) == want[i]);
    // per-deck id layout — 16 slots reserved per deck
    REQUIRE(perfParamId(0, PerfParam::LoopSize) == 0);
    REQUIRE(perfParamId(2, PerfParam::SlipActive) == 2 * 16 + 4);
    // sanitize: toggles quantize to 0/1, sizes clamp to range
    REQUIRE(sanitizePerfParam(PerfParam::SlipActive, 0.7f) == 1.0f);
    REQUIRE(sanitizePerfParam(PerfParam::LoopSize, 99.0f) == 32.0f);
    REQUIRE(sanitizePerfParam(PerfParam::LoopSize, -1.0f) == 0.03125f);
    REQUIRE(sanitizePerfParam(PerfParam::QuantizeMode, 42.0f) == 10.0f);
}

// ============================ end of track (§56) ============================

TEST_CASE("perf35: beat jump clamps at the track end (§56)", "[perf35]") {
    Rig r;
    // 8 s @120bpm = 16 beats exactly
    r.load(0, 120.0, 8.0);
    r.play(0);
    r.seek(0, 24000 * 14);                     // beat 14
    r.perf.beatJump(0, 8.0);                   // would land at beat 22
    auto* s = r.lastOf<rt::DeckSeekFrame>();
    REQUIRE(s != nullptr);
    REQUIRE(s->frame < (int64_t)(8.0 * kRate)); // clamped inside
    REQUIRE(s->frame == (int64_t)(8.0 * kRate) - 1);
}

TEST_CASE("perf35: auto loop clamps out; in past end rejects (§56)",
          "[perf35]") {
    Rig r;
    r.load(0, 120.0, 8.0);                     // 16 beats
    r.play(0);
    r.seek(0, 24000 * 14);                     // beat 14
    r.perf.loopBeats(0, 8.0);                  // out would be beat 22
    auto* lf = r.lastOf<rt::SetLoopFrames>();
    REQUIRE(lf != nullptr);
    REQUIRE(lf->outFrame == (int64_t)(8.0 * kRate));
    REQUIRE(lf->inFrame > 0);
    REQUIRE(lf->outFrame > lf->inFrame);

    // position at/over the very end → deterministic BadRange
    r.seek(0, (int64_t)(8.0 * kRate) - 10);
    r.perf.loopBeats(0, 4.0);
    REQUIRE(r.perf.lastError(0) == PerfError::BadRange);
}

TEST_CASE("perf35: loop move bounded at both ends (§56)", "[perf35]") {
    auto g = *makeGrid(120.0);
    const int64_t durF = (int64_t)(8.0 * kRate);  // 16 beats
    SmartLoopEngine::Loop l { 24000 * 4, 24000 * 8 };  // 4 beats
    // +12 beats would push out to beat 20 → shifts back to [12,16)
    auto m = SmartLoopEngine::boundedMove(g, l, 12.0, durF);
    REQUIRE(m.out == durF);
    REQUIRE(m.out - m.in == 24000 * 4);         // length preserved
    // -8 beats would go negative → clamps to [0,4)
    m = SmartLoopEngine::boundedMove(g, l, -8.0, durF);
    REQUIRE(m.in == 0);
    REQUIRE(m.out - m.in == 24000 * 4);
    // in-range move unchanged
    m = SmartLoopEngine::boundedMove(g, l, 1.0, durF);
    REQUIRE(m.in == 24000 * 5);
    // clampOut: empty when in is past the end
    auto c = SmartLoopEngine::clampOut({durF + 10, durF + 20}, durF);
    REQUIRE(c.out <= c.in);
}

// ==================== performance command vocabulary ====================

TEST_CASE("perf35: PerformanceCommand + PerfCmd vocabulary (§41-42)",
          "[perf35]") {
    REQUIRE((int)PerfCmd::Count == 14);
    PerformanceCommand c { PerfCmd::LoopMove, 1, -4.0, 12345 };
    REQUIRE(c.type == PerfCmd::LoopMove);
    REQUIRE(c.deck == 1);
    REQUIRE(c.value == Approx(-4.0));
    REQUIRE(c.timestampMs == 12345u);
    // every spec verb is named
    static_assert((int)PerfCmd::QuantizeOff > (int)PerfCmd::QuantizeOn);
    static_assert((int)PerfCmd::LoopHalf    > (int)PerfCmd::LoopDouble);
    static_assert((int)PerfCmd::CueJump     > (int)PerfCmd::QuantizeOff);
}

// ==================== combined: jump/loop under slip ====================

TEST_CASE("perf35: beat jump under slip re-anchors the shadow (§34)",
          "[perf35]") {
    Rig r;
    r.load(0, 120.0, 60.0);
    r.play(0);
    r.secs(0.3);
    r.perf.setSlip(0, true);
    const int nb = r.countOf<rt::SlipBegin>();
    r.perf.beatJump(0, 8.0);
    // §34 — jump under slip = permanent move: seek + re-anchor
    REQUIRE(r.lastOf<rt::SlipReanchor>() != nullptr);
    REQUIRE(r.countOf<rt::SlipBegin>() == nb);  // no extra begin
    r.perf.setSlip(0, false);
}

TEST_CASE("perf35: manual loop quantized at configured quantum (§20)",
          "[perf35]") {
    Rig r;
    r.load(0, 120.0, 60.0);
    r.play(0);
    r.perf.setQuantize(0, true);
    r.perf.setQuantizeResolution(0, Quantize::Quarter);
    r.seek(0, 24000 * 2 + 2800);               // between 1/4 boundaries
    r.perf.loopIn(0);
    r.seek(0, 24000 * 3 + 4100);
    r.perf.loopOut(0);
    auto* lf = r.lastOf<rt::SetLoopFrames>();
    REQUIRE(lf != nullptr);
    // quarter note = 6000 frames — both edges on 6000-multiples
    REQUIRE(lf->inFrame  % 6000 == 0);
    REQUIRE(lf->outFrame % 6000 == 0);
    REQUIRE(r.perf.loopState(0).quantized);
}
