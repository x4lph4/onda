// Phase 19 — waveform view engine tests: viewport/zoom/autoscroll,
// playhead smoothing + jump detection, frame building (columns,
// markers, cues, loops, badges, playhead), interaction → commands,
// cache keys, 4-deck layout, and long-track performance.
// Everything under test is the JUCE-free model in src/ui/.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ui/LayoutSpec.h"
#include "ui/WaveformViewEngine.h"
#include "core/model/DeckStats.h"

using namespace onda;
using namespace onda::ui;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// Synthetic pyramid — deterministic envelope, all three levels real
/// (a long track's real data size, so perf numbers are honest).
WaveformData synthWave(double durSec, int ch = 1,
                       double rate = kRate) {
    WaveformData w;
    w.channels   = ch;
    w.numFrames  = (int64_t)(durSec * rate);
    w.sampleRate = rate;
    auto fill = [&](std::vector<WaveformBin>& v, int64_t nBins) {
        if (nBins <= 0) return;
        v.resize((size_t)nBins * (size_t)ch);
        for (int64_t i = 0; i < nBins; ++i)
            for (int c = 0; c < ch; ++c) {
                const double t = (double)i / (double)nBins;
                // pulsing envelope + quiet intro/outro shape
                const float env = (float)(0.5 + 0.45 * std::sin(t * 40.0))
                                * (float)std::min(1.0, t * 20.0);
                v[(size_t)i * (size_t)ch + (size_t)c] =
                    { -env, env, env * 0.7f };
            }
    };
    fill(w.overview, WaveformData::kOverviewBins);
    fill(w.mid,     w.numFrames / WaveformData::kMidFrames);
    fill(w.detail,  w.numFrames / WaveformData::kDetailFrames);
    return w;
}

BeatGrid synthGrid(double bpm, int64_t firstBeat = 0, int bpb = 4,
                   double rate = kRate) {
    BeatGrid g;
    g.firstBeatFrame = firstBeat;
    g.beatsPerBar    = bpb;
    g.segments.push_back({ firstBeat, 0, bpm });
    g.setRate(rate);
    return g;
}

DeckSnapshot snapFor(double posSec, double durSec,
                     DeckState st = DeckState::Playing) {
    DeckSnapshot s;
    s.state       = (int)st;
    s.durationSec = durSec;
    s.positionSec = posSec;
    s.bpm = s.effBpm = 128.0;
    s.path        = "/tmp/t.wav";
    s.title       = "T";
    s.analyzed    = true;
    s.beatPos     = posSec * 128.0 / 60.0;
    s.beatsPerBar = 4;
    return s;
}

struct Rig {
    WaveformData   w   = synthWave(300.0);
    BeatGrid       g   = synthGrid(128.0);
    DeckSnapshot   s   = snapFor(10.0, 300.0);
    WaveformViewEngine e;
    WaveformFrame  f;
    Rig() {
        e.wf = &w; e.grid = &g; e.snap = &s;
        e.detail.setView(w.numFrames / w.sampleRate, 800);
        e.detail.pxPerSec = 160.0;
        e.overview.setView(w.numFrames / w.sampleRate, 800);
        e.overview.fitAll();
        e.tick(0.033);
        e.buildDetail(f);
    }
};

} // namespace

// ==========================================================================
// §3 — pyramid level selection
// ==========================================================================
TEST_CASE("queryLevel: pyramid picks cheapest adequate level",
          "[ui][p19]") {
    WaveformData w = synthWave(300.0);
    const int64_t F = (int64_t)kRate;
    // whole track at 800 px → overview (span huge, budget small)
    REQUIRE(w.queryLevel(0, w.numFrames, 800) == 0);
    // 8-second window at 800 px → detail bins ≈1500 ≤ 3200 → detail
    REQUIRE(w.queryLevel(0, 8 * F, 800) == 2);
    // 60-second window at 800 px → detail ~11k > 3200 → mid (~2812)
    REQUIRE(w.queryLevel(0, 60 * F, 800) == 1);
    // no data → -1
    WaveformData empty;
    REQUIRE(empty.queryLevel(0, 10 * F, 800) == -1);
    REQUIRE(w.queryLevel(10 * F, 5 * F, 800) == -1);   // inverted
}

// ==========================================================================
// §33-34 — zoom anchors
// ==========================================================================
TEST_CASE("WaveformZoom: cursor/playhead/center anchors hold position",
          "[ui][p19]") {
    WaveformZoom z;
    WaveformViewport v;
    v.setView(300.0, 800);
    v.pxPerSec = 160.0;
    v.scrollTo(40.0);

    // cursor anchor: sec under px stays under px
    const double cursorPx = 300.0;
    const double before = v.pxToSec(cursorPx);
    z.in(v, ZoomAnchor::Cursor, cursorPx, 0.0);
    REQUIRE(v.pxToSec(cursorPx) == Approx(before).epsilon(0.001));
    REQUIRE(v.pxPerSec == Approx(200.0));

    // playhead anchor: playhead px preserved
    const double ph = 42.0;
    const double phPx = v.secToPx(ph);
    z.out(v, ZoomAnchor::Playhead, 0.0, ph);
    REQUIRE(v.secToPx(ph) == Approx(phPx).epsilon(0.001));

    // center anchor: mid-point preserved
    const double mid = v.scrollSec + v.visibleSec() * 0.5;
    const double midPx = v.secToPx(mid);
    z.in(v, ZoomAnchor::Center, 0.0, 0.0);
    REQUIRE(v.secToPx(mid) == Approx(midPx).epsilon(0.001));

    // clamps still apply
    for (int i = 0; i < 60; ++i) z.in(v, ZoomAnchor::Center, 0, 0);
    REQUIRE(v.pxPerSec <= WaveformViewport::kMaxPxPerSec);
    for (int i = 0; i < 80; ++i) z.out(v, ZoomAnchor::Center, 0, 0);
    REQUIRE(v.pxPerSec >= WaveformViewport::kMinPxPerSec);

    // reset returns to default zoom and reveals the playhead
    v.scrollTo(0.0);
    z.reset(v, 250.0);
    REQUIRE(v.pxPerSec == Approx(160.0));
    REQUIRE(v.endSec() >= 250.0);
}

// ==========================================================================
// §35 — auto-scroll modes
// ==========================================================================
TEST_CASE("WaveformAutoScroll: Off/Center/Smart", "[ui][p19]") {
    WaveformAutoScroll as;
    WaveformViewport v;
    v.setView(300.0, 800);
    v.pxPerSec = 160.0;                 // 5 s visible
    v.scrollTo(100.0);

    // Off: nothing moves even while playing
    const double s0 = v.scrollSec;
    as.apply(v, 200.0, true, AutoScrollMode::Off, 0.033);
    REQUIRE(v.scrollSec == s0);

    // paused: nothing moves in any mode
    as.apply(v, 200.0, false, AutoScrollMode::Center, 0.033);
    REQUIRE(v.scrollSec == s0);

    // Center: eases toward playhead-centred view
    for (int i = 0; i < 120; ++i)
        as.apply(v, 200.0, true, AutoScrollMode::Center, 0.033);
    const double center = v.scrollSec + v.visibleSec() * 0.5;
    REQUIRE(center == Approx(200.0).margin(0.5));

    // Smart: only scrolls when the playhead nears a margin. Window is
    // [10,15] s; ensureVisible(0.35) keeps pos inside [11.75, 13.25].
    v.scrollTo(10.0);
    as.apply(v, 12.4, true, AutoScrollMode::Smart, 0.033);
    REQUIRE(v.scrollSec == 10.0);        // well inside — no move
    as.apply(v, 14.6, true, AutoScrollMode::Smart, 0.033);
    REQUIRE(v.scrollSec > 10.0);         // past right margin — follows
}

// ==========================================================================
// §10-11 — playhead smoothing + jump detection
// ==========================================================================
TEST_CASE("PlayheadSmoother: visual-only drift + jump events",
          "[ui][p19]") {
    PlayheadSmoother ph;
    // first update primes exactly
    REQUIRE(ph.update(10.0, true, 0.033) == 10.0);
    REQUIRE(ph.primed);
    REQUIRE(ph.jumps == 0);

    // playing: advances between ticks and converges to reported pos
    const double d1 = ph.update(10.05, true, 0.033, 1.0);
    REQUIRE(d1 == Approx(10.05).margin(0.01));
    // paused: exact draw position — but a teleport still counts as a
    // jump (a seek discontinuity is a jump whether playing or not)
    REQUIRE(ph.update(11.0, false, 0.033) == 11.0);
    REQUIRE(ph.jumps == 1);

    // a discontinuity beyond the threshold counts as a jump
    const double d2 = ph.update(60.0, true, 0.033);
    REQUIRE(d2 == 60.0);
    REQUIRE(ph.jumps == 2);
    REQUIRE(ph.lastJumpPosSec == 60.0);

    // small drift is smoothed, NOT treated as a jump
    ph.update(60.10, true, 0.033);
    REQUIRE(ph.jumps == 2);
}

// ==========================================================================
// §2-3,6 — buildDetail: columns + level + playhead
// ==========================================================================
TEST_CASE("buildDetail: columns normalized, level picked, playhead px",
          "[ui][p19]") {
    Rig r;
    REQUIRE(r.f.hasWave);
    REQUIRE(r.f.hasGrid);
    REQUIRE(r.f.cols.size() == 800);
    REQUIRE(r.f.levelUsed == 2);   // 5 s window → 937 detail bins fits
    for (const auto& c : r.f.cols) {
        REQUIRE(c.env >= 0.0f); REQUIRE(c.env <= 1.0f);
        REQUIRE(c.rms >= 0.0f); REQUIRE(c.rms <= 1.0f);
        REQUIRE(c.crest >= 0.0f); REQUIRE(c.crest <= 1.0f);
    }
    REQUIRE(r.f.playheadPx == Approx(r.e.detail.secToPx(10.0)));
    REQUIRE(r.f.f1 > r.f.f0);
}

TEST_CASE("buildDetail: crest factor reflects transient content",
          "[ui][p19]") {
    // a spiky bin (peak≫rms) yields high crest; flat bin yields ~0
    WaveformData w;
    w.channels = 1; w.sampleRate = kRate;
    w.numFrames = 48000;                     // 1 s
    w.overview.resize(WaveformData::kOverviewBins);
    w.mid.resize(w.numFrames / WaveformData::kMidFrames,
                 { -1.0f, 1.0f, 0.3f });     // cresty
    w.detail.resize(w.numFrames / WaveformData::kDetailFrames,
                    { -1.0f, 1.0f, 0.3f });
    DeckSnapshot s = snapFor(0.0, 1.0, DeckState::Paused);
    WaveformViewEngine e;
    e.wf = &w; e.snap = &s;
    e.detail.setView(1.0, 400);
    e.detail.pxPerSec = 400.0;
    e.tick(0.033);
    WaveformFrame f;
    e.buildDetail(f);
    REQUIRE(f.cols.size() == 400);
    // env/rms = 1/0.3 ≈ 3.3 → crest ≈ (3.3-1)/3 ≈ 0.78
    REQUIRE(f.cols[200].crest > 0.5f);
}

// ==========================================================================
// §12-14 — beatgrid markers, bar numbers, density gating
// ==========================================================================
TEST_CASE("buildDetail: grid markers kinds, bar numbers, gating",
          "[ui][p19]") {
    Rig r;
    REQUIRE(!r.f.beats.empty());
    int bars = 0, dbs = 0, beats = 0, numbered = 0;
    for (const auto& m : r.f.beats) {
        if (m.kind == 0) ++beats;
        else if (m.kind == 1) ++bars;
        else ++dbs;
        if (m.barNo > 0) ++numbered;
    }
    REQUIRE(beats > 0); REQUIRE(bars > 0);
    // every 4th bar-start is a downbeat → dbs ≈ bars/4
    REQUIRE(dbs > 0);
    REQUIRE(numbered > 0);          // 320 px/bar ⇒ room for numbers

    // low zoom: beats too dense → markers omitted entirely
    r.e.detail.pxPerSec = 8.0;      // ≈4 px/beat < 6 px gate
    r.e.buildDetail(r.f);
    REQUIRE(r.f.beats.empty());

    // grid edit: markers flagged for emphasis
    r.e.detail.pxPerSec = 160.0;
    r.e.interact.gridEdit = true;
    r.e.buildDetail(r.f);
    REQUIRE(!r.f.beats.empty());
    REQUIRE(r.f.beats.front().gridEdit);
    r.e.interact.gridEdit = false;
}

TEST_CASE("buildDetail: current-beat flag + bar band", "[ui][p19]") {
    Rig r;
    // playhead at 10 s, 128 bpm → beat ≈ 21.33 → beat 21 is current
    bool anyCurrent = false;
    for (const auto& m : r.f.beats)
        if (m.current) anyCurrent = true;
    REQUIRE(anyCurrent);
    REQUIRE(r.f.hasBarBand);
    REQUIRE(r.f.barBand.x1 > r.f.barBand.x0);
}

// ==========================================================================
// §18-21 — cue flags + loop rects
// ==========================================================================
TEST_CASE("buildDetail: cue flags typed + armed delete state",
          "[ui][p19]") {
    Rig r;
    r.e.detail.scrollTo(0.0);   // deterministic [0,5] s window
    // cues must sit inside the visible window ([0,5] s at scroll 0)
    r.e.cues = {
        { 1.0, 0xFFFF0000, "1", 0, 0 },      // hot cue slot 0
        { 3.0, 0xFF00FF00, "m", -1, 1 },     // memory cue
    };
    r.e.interact.armedSlot = 0;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.size() == 2);
    REQUIRE(r.f.cues[0].px ==
            Approx((float)r.e.detail.secToPx(1.0)));
    REQUIRE(r.f.cues[0].slot == 0);
    REQUIRE(r.f.cues[0].type == 0);
    REQUIRE(r.f.cues[0].armedDelete);
    REQUIRE(r.f.cues[1].type == 1);
    REQUIRE(!r.f.cues[1].armedDelete);
}

TEST_CASE("buildDetail: loop rects clamped to view, active/saved",
          "[ui][p19]") {
    Rig r;
    r.e.detail.scrollTo(0.0);
    r.e.loops = {
        { 0.0, 1.0, true, false, -1 },       // active deck loop
        { 100.0, 108.0, false, true, 2 },    // saved loop off-screen
    };
    r.e.buildDetail(r.f);
    REQUIRE(r.f.loops.size() == 1);          // off-screen culled
    REQUIRE(r.f.loops[0].active);
    REQUIRE(r.f.loops[0].x0 >= -4.0f);
    r.e.detail.scrollTo(95.0);
    r.e.buildDetail(r.f);
    REQUIRE(r.f.loops.size() == 1);
    REQUIRE(r.f.loops[0].saved);
    REQUIRE(r.f.loops[0].id == 2);
}

// ==========================================================================
// §38-40 — state badges
// ==========================================================================
TEST_CASE("buildDetail: badges for deck/analysis/grid states",
          "[ui][p19]") {
    auto hasBadge = [](const WaveformFrame& f, const char* t) {
        for (const auto& b : f.badges)
            if (std::string(b.text) == t) return true;
        return false;
    };
    Rig r;
    REQUIRE(!hasBadge(r.f, "PLAYING"));      // playing = no chip
    REQUIRE(hasBadge(r.f, "READY") == false);

    r.s.state = (int)DeckState::Paused;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "PAUSED"));

    r.s.state = (int)DeckState::Loading;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "DECODING"));

    r.s.state = (int)DeckState::Error;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "ERROR"));

    r.s.state = (int)DeckState::Paused;
    r.s.analyzed = false;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "ANALYZING"));

    r.s.analyzed = true; r.e.grid = nullptr;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "NO GRID"));
    r.e.grid = &r.g;

    r.e.interact.gridEdit = true;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "GRID EDIT"));
    r.e.interact.gridEdit = false;

    r.s.isMaster = true;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "MASTER"));
    r.s.isMaster = false;

    r.s.syncMode = 1; r.s.syncConf = 0.3f;
    r.e.buildDetail(r.f);
    REQUIRE(hasBadge(r.f, "SYNC UNCERTAIN"));
}

// ==========================================================================
// §7 — overview frame
// ==========================================================================
TEST_CASE("buildOverview: whole track, cues+loops, real playhead",
          "[ui][p19]") {
    Rig r;
    r.e.cues = { { 150.0, 0xFFFF0000, "1", 0, 0 } };
    r.e.loops = { { 100.0, 120.0, false, true, 0 } };
    WaveformFrame ov;
    r.e.buildOverview(ov);
    REQUIRE(ov.hasWave);
    REQUIRE(ov.cols.size() == 800);
    REQUIRE(ov.beats.empty());               // no grid on overview
    REQUIRE(ov.cues.size() == 1);
    REQUIRE(ov.cues[0].px ==
            Approx((float)r.e.overview.secToPx(150.0)));
    REQUIRE(ov.loops.size() == 1);
    // overview playhead = real position, not the smoothed draw pos
    REQUIRE(ov.playheadPx ==
            Approx(r.e.overview.secToPx(10.0)));
}

// ==========================================================================
// §42 — interaction → commands
// ==========================================================================
TEST_CASE("interaction: hit-test zones and press/drag/release",
          "[ui][p19]") {
    Rig r;
    r.e.detail.scrollTo(0.0);   // deterministic [0,5] s window
    // everything inside the [0,5] s visible window
    r.e.cues = { { 1.0, 0xFFFF0000, "1", 0, 0 } };
    r.e.loops = { { 1.0, 2.0, true, false, -1 } };
    r.e.buildDetail(r.f);
    const double cuePx = r.f.cues[0].px;
    const auto& v = r.e.detail;
    auto& it = r.e.interact;

    // cue flag zone (top strip)
    auto h = WaveformInteraction::hitTest(cuePx, 2.0, r.f, v);
    REQUIRE(h.zone == HitZone::CueFlag);
    REQUIRE(h.index == 0);

    // left-click press+release on flag → trigger command
    auto c = it.press(h, r.f, false, 0.0);
    REQUIRE(c.kind == WaveCmdKind::None);    // deferred to release
    c = it.release(r.f, false, v);
    REQUIRE(c.kind == WaveCmdKind::CueTrigger);
    REQUIRE(c.slot == 0);

    // right-click arms; second within window deletes
    c = it.press(h, r.f, true, 1000.0);
    REQUIRE(c.kind == WaveCmdKind::CueArmDelete);
    c = it.release(r.f, true, v);
    c = it.press(h, r.f, true, 1500.0);
    REQUIRE(c.kind == WaveCmdKind::CueDelete);
    REQUIRE(c.slot == 0);
    // ...but outside the window it only re-arms
    c = it.press(h, r.f, true, 9000.0);
    REQUIRE(c.kind == WaveCmdKind::CueArmDelete);
    it.release(r.f, true, v);

    // dragging a cue emits CueSet at the drop position
    it.press(h, r.f, false, 0.0);
    it.drag(cuePx + 160.0, v);               // +1 s at 160 px/s
    c = it.release(r.f, false, v);
    REQUIRE(c.kind == WaveCmdKind::CueSet);
    REQUIRE(c.slot == 0);
    REQUIRE(c.a == Approx(2.0).margin(0.05));

    // wave zone: press = Seek, drag opens a §53 scrub transaction
    // (SeekBegin → ScrubSeek* → SeekCommit on release)
    h = WaveformInteraction::hitTest(500.0, 40.0, r.f, v);
    REQUIRE(h.zone == HitZone::Wave);
    c = it.press(h, r.f, false, 0.0);
    REQUIRE(c.kind == WaveCmdKind::Seek);
    c = it.drag(600.0, v);
    REQUIRE(c.kind == WaveCmdKind::SeekBegin);
    c = it.drag(640.0, v);
    REQUIRE(c.kind == WaveCmdKind::ScrubSeek);
    REQUIRE(c.a == Approx(v.pxToSec(640.0)));
    c = it.release(r.f, false, v);
    REQUIRE(c.kind == WaveCmdKind::SeekCommit);

    // grid-edit mode: click = set first beat, drag = nudge ms
    it.gridEdit = true;
    c = it.press(h, r.f, false, 0.0);
    REQUIRE(c.kind == WaveCmdKind::GridSetFirstBeat);
    REQUIRE(c.a == Approx(v.pxToSec(500.0)));
    c = it.drag(660.0, v);                   // +160 px = +1 s = 1000 ms
    REQUIRE(c.kind == WaveCmdKind::GridNudgeMs);
    REQUIRE(c.a == Approx(1000.0).margin(1.0));
    it.gridEdit = false;

    // loop edge drag → LoopSet on release (loop = [1.0, 2.0] s)
    const double inPx  = r.f.loops[0].x0;
    const double outPx = r.f.loops[0].x1;
    (void)inPx;
    h = WaveformInteraction::hitTest(outPx, 30.0, r.f, v);
    REQUIRE(h.zone == HitZone::LoopOut);
    it.press(h, r.f, false, 0.0);
    it.drag(outPx + 160.0, v);
    c = it.release(r.f, false, v);
    REQUIRE(c.kind == WaveCmdKind::LoopSet);
    REQUIRE(c.a == Approx(1.0).margin(0.05));
    REQUIRE(c.b == Approx(3.0).margin(0.05));

    // overview: inside window → pan zone, outside → seek zone
    WaveformViewport over;
    over.setView(300.0, 800); over.fitAll();
    auto oh = WaveformInteraction::hitTestOverview(
        over.secToPx(v.scrollSec + 1.0), v, over);
    REQUIRE(oh.zone == HitZone::OverviewWindow);
    oh = WaveformInteraction::hitTestOverview(
        over.secToPx(299.0), v, over);
    REQUIRE(oh.zone == HitZone::Wave);
}

// ==========================================================================
// §43 — cache keys
// ==========================================================================
TEST_CASE("WaveformCache: key stability and sensitivity",
          "[ui][p19]") {
    WaveformViewport v;
    v.setView(300.0, 800);
    v.pxPerSec = 160.0;
    WaveformData w = synthWave(300.0);

    const uint64_t k0 =
        WaveformCache::key(v, 800, &w, w.numFrames, 0, 1.0);
    REQUIRE(WaveformCache::key(v, 800, &w, w.numFrames, 0, 1.0) == k0);
    // every input perturbs the key
    v.scrollSec = 1.0;
    REQUIRE(WaveformCache::key(v, 800, &w, w.numFrames, 0, 1.0) != k0);
    v.scrollSec = 0.0;
    REQUIRE(WaveformCache::key(v, 800, &w, w.numFrames, 1, 1.0) != k0);
    REQUIRE(WaveformCache::key(v, 800, &w, w.numFrames, 0, 2.0) != k0);
    REQUIRE(WaveformCache::key(v, 400, &w, w.numFrames, 0, 1.0) != k0);
    WaveformData w2 = synthWave(60.0);
    REQUIRE(WaveformCache::key(v, 800, &w2, w2.numFrames, 0, 1.0)
            != k0);
}

// ==========================================================================
// §22 — beat-jump flash events (model side)
// ==========================================================================
TEST_CASE("engine tick: position jumps counted as events",
          "[ui][p19]") {
    Rig r;
    const int j0 = r.e.jumpEvents();
    r.s.positionSec += 30.0;                 // a beat jump lands here
    r.e.tick(0.033);
    REQUIRE(r.e.jumpEvents() == j0 + 1);
    // small movement is not a jump
    r.s.positionSec += 0.01;
    r.e.tick(0.033);
    REQUIRE(r.e.jumpEvents() == j0 + 1);
}

// ==========================================================================
// §45 — responsive layout, 4-deck regions
// ==========================================================================
TEST_CASE("LayoutSpec: 4-deck regions split evenly, mixer centered",
          "[ui][p19]") {
    LayoutPrefs p;
    p.deckMode = 4;
    const auto L = LayoutSpec::compute(1600, 900, 34, p);
    REQUIRE(L.deckCount == 4);
    // four equal wave strips across the window
    for (int i = 0; i < 4; ++i) {
        REQUIRE(L.waves[i].w == Approx(400).margin(1));
        REQUIRE(L.waves[i].x == Approx(i * 400).margin(1));
    }
    // four deck panels around the mixer: A C | mixer | D B
    REQUIRE(L.decks[0].x < L.decks[2].x);
    REQUIRE(L.decks[2].x + L.decks[2].w <= L.mixer.x);
    REQUIRE(L.decks[3].x >= L.mixer.x + L.mixer.w);
    REQUIRE(L.decks[1].x >= L.decks[3].x);
    REQUIRE(L.decks[3].x + L.decks[3].w <= L.decks[1].x);
    for (int i = 0; i < 4; ++i) REQUIRE(L.decks[i].w > 100);

    // 2-deck mode unchanged (regression)
    p.deckMode = 2;
    const auto L2 = LayoutSpec::compute(1600, 900, 34, p);
    REQUIRE(L2.deckCount == 2);
    REQUIRE(L2.waves[0].w == Approx(800).margin(1));
    REQUIRE(L2.decks[0].w > L2.decks[3].w - 1);   // A widest
}

// ==========================================================================
// §50-51 — long-track + 4-deck performance
// ==========================================================================
TEST_CASE("perf: frame build on 5min/30min/2h tracks", "[ui][p19]") {
    struct Case { double dur; };
    for (const double dur : { 300.0, 1800.0, 7200.0 }) {
        WaveformData w = synthWave(dur);
        BeatGrid g = synthGrid(128.0);
        DeckSnapshot s = snapFor(dur * 0.4, dur);
        WaveformViewEngine e;
        e.wf = &w; e.grid = &g; e.snap = &s;
        e.cues = { { dur*0.3, 0xFF0000FF, "1", 0, 0 },
                   { dur*0.6, 0xFF00FF00, "2", 1, 0 } };
        e.loops = { { dur*0.35, dur*0.37, true, false, -1 } };
        e.detail.setView(dur, 800);
        e.detail.pxPerSec = 160.0;
        e.detail.scrollTo(dur * 0.35);
        e.tick(0.033);
        WaveformFrame f;
        const int iters = 50;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) e.buildDetail(f);
        const double avgMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count() / iters;
        std::printf("  [wave-perf] %4.0fs track  build %6.2f ms  "
                    "level %d  markers %3d  cues %d  loops %d\n",
                    dur, avgMs, f.levelUsed, (int)f.beats.size(),
                    (int)f.cues.size(), (int)f.loops.size());
        // generous ceiling — a regression signal, not a perf contract
        REQUIRE(avgMs < 25.0);
        REQUIRE(f.cols.size() == 800);       // never thousands of bins
    }
}

TEST_CASE("stress: four decks build frames in parallel budget",
          "[ui][p19][stress]") {
    WaveformData w = synthWave(1800.0);
    BeatGrid g = synthGrid(128.0);
    std::vector<std::unique_ptr<WaveformViewEngine>> engines;
    std::vector<WaveformFrame> frames(4);
    for (int d = 0; d < 4; ++d) {
        engines.push_back(std::make_unique<WaveformViewEngine>());
        auto& e = *engines.back();
        e.wf = &w; e.grid = &g;
        e.snap = nullptr;
        e.cues = { { 100.0, 0xFF0000FF, "1", 0, 0 } };
        e.loops = { { 200.0, 208.0, true, false, -1 } };
        e.detail.setView(1800.0, 800);
        e.detail.pxPerSec = 160.0;
        e.detail.scrollTo(60.0 * d);
    }
    DeckSnapshot s = snapFor(500.0, 1800.0);
    for (auto& e : engines) e->snap = &s;

    const int iters = 30;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        for (int d = 0; d < 4; ++d)
            engines[(size_t)d]->buildDetail(frames[(size_t)d]);
    const double perTickMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count() / iters;
    std::printf("  [wave-stress] 4-deck frame build: %5.2f ms/tick\n",
                perTickMs);
    // four decks must still fit in a 30 Hz UI tick with room to spare
    REQUIRE(perTickMs < 60.0);
    for (auto& e : engines) REQUIRE(e->diag.columns == 800);
}

// ==========================================================================
// § diag — diagnostics counters
// ==========================================================================
TEST_CASE("diagnostics: counters reflect the last frame",
          "[ui][p19]") {
    Rig r;
    r.e.detail.scrollTo(0.0);   // cue at 1 s must be on-screen
    r.e.cues = { { 1.0, 0xFFFF0000, "1", 0, 0 } };
    r.e.buildDetail(r.f);
    REQUIRE(r.e.diag.columns == 800);
    REQUIRE(r.e.diag.markers == (int)r.f.beats.size());
    REQUIRE(r.e.diag.cues == 1);
    REQUIRE(r.e.diag.level == r.f.levelUsed);
    REQUIRE(r.e.diag.queries >= 1);
    REQUIRE(r.e.diag.buildMs >= 0.0);
}
