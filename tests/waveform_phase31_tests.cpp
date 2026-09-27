// Phase 31 — deck visualization engine: VisualizationState flags,
// marker collision tiers + virtualization index, seek transaction
// (begin/commit/cancel + ghost preview), primary-cue type, perf
// degrade mode, TimeDisplay formats, GraphicsRenderer seam, and
// long-track / many-marker / zoom / seek stress.
// Everything under test is the JUCE-free model in src/ui/.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "ui/GraphicsRenderer.h"
#include "ui/TimeDisplay.h"
#include "ui/VisualizationState.h"
#include "ui/WaveformViewEngine.h"
#include "core/model/DeckStats.h"

using namespace onda;
using namespace onda::ui;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

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
// §37-38 — TimeDisplay: M:SS, M:SS.ss, H:MM:SS, modes + tags
// ==========================================================================
TEST_CASE("TimeDisplay: M:SS / H:MM:SS / centiseconds", "[ui31]") {
    char b[24];
    TimeDisplay::format(b, sizeof b, 0.0);
    REQUIRE(std::string(b) == "0:00");
    TimeDisplay::format(b, sizeof b, 65.4);        // drops centis
    REQUIRE(std::string(b) == "1:05");
    TimeDisplay::format(b, sizeof b, 599.9);       // rounds → 10:00
    REQUIRE(std::string(b) == "10:00");
    // >= 1 h → H:MM:SS (never M:SS with 3+ digit minutes)
    TimeDisplay::format(b, sizeof b, 3600.0);
    REQUIRE(std::string(b) == "1:00:00");
    TimeDisplay::format(b, sizeof b, 7261.0);      // 2 h 1 m 1 s
    REQUIRE(std::string(b) == "2:01:01");
    // centisecond variant stays M:SS.ss below 1 h
    TimeDisplay::formatCs(b, sizeof b, 65.44);
    REQUIRE(std::string(b) == "1:05.44");
    TimeDisplay::formatCs(b, sizeof b, 3661.5);
    REQUIRE(std::string(b) == "1:01:02");
}

TEST_CASE("TimeDisplay: Elapsed/Remaining/Total modes + sign",
          "[ui31]") {
    char b[24];
    TimeDisplay::formatMode(b, sizeof b,
        TimeDisplay::Mode::Elapsed, 75.0, 300.0);
    REQUIRE(std::string(b) == "1:15");
    TimeDisplay::formatMode(b, sizeof b,
        TimeDisplay::Mode::Remaining, 75.0, 300.0);
    REQUIRE(std::string(b) == "-3:45");
    TimeDisplay::formatMode(b, sizeof b,
        TimeDisplay::Mode::Total, 75.0, 300.0);
    REQUIRE(std::string(b) == "5:00");
    // remaining clamps at 0 (never "-0:xx" past the end)
    TimeDisplay::formatMode(b, sizeof b,
        TimeDisplay::Mode::Remaining, 400.0, 300.0);
    REQUIRE(std::string(b) == "0:00");
    // tags are stable for the UI label column
    REQUIRE(std::string(TimeDisplay::tag(TimeDisplay::Mode::Elapsed))
            == "EL");
    REQUIRE(std::string(TimeDisplay::tag(TimeDisplay::Mode::Remaining))
            == "REM");
    REQUIRE(std::string(TimeDisplay::tag(TimeDisplay::Mode::Total))
            == "TOT");
    // edge: negative/NaN input → 0:00, never garbage
    TimeDisplay::format(b, sizeof b, -5.0);
    REQUIRE(std::string(b) == "0:00");
    TimeDisplay::format(b, sizeof b,
                        std::numeric_limits<double>::quiet_NaN());
    REQUIRE(std::string(b) == "0:00");
}

// ==========================================================================
// §2 — VisualizationState flags gate each layer
// ==========================================================================
TEST_CASE("VisualizationState: layer toggles gate frame content",
          "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(0.0);
    r.e.cues  = { { 1.0, 0xFFFF0000, "1", 0, 0 },
                  { 2.0, 0xFF00FF00, "m", -1, 1 },
                  { 3.0, 0xFF0000FF, "C", -1, 3 } };
    r.e.loops = { { 1.0, 2.0, true, false, -1 } };
    r.e.phrases = { { 1.5, 0 } };
    r.e.vis.showPhrases = true;              // §30 seam opt-in
    ++r.e.markersRev;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.size() == 3);
    REQUIRE(r.f.loops.size() == 1);
    REQUIRE(!r.f.beats.empty());
    REQUIRE(!r.f.phrases.empty());

    r.e.vis.showBeatgrid = false;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.beats.empty());
    REQUIRE(!r.f.hasBarBand);
    r.e.vis.showBeatgrid = true;

    r.e.vis.showHotCues = false;
    r.e.buildDetail(r.f);                    // hot gone, mem+prim stay
    REQUIRE(r.f.cues.size() == 2);
    for (const auto& c : r.f.cues) REQUIRE(c.type != 0);

    r.e.vis.showMemoryCues = false;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.size() == 1);
    REQUIRE(r.f.cues[0].type == 3);          // §25 primary survives

    r.e.vis.showPrimaryCue = false;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.empty());

    r.e.vis.showCues = false;                // master gate
    r.e.vis.showHotCues = r.e.vis.showMemoryCues =
        r.e.vis.showPrimaryCue = true;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.empty());
    r.e.vis.showCues = true;

    r.e.vis.showLoops = false;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.loops.empty());
    r.e.vis.showLoops = true;

    r.e.vis.showPhrases = false;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.phrases.empty());
}

// ==========================================================================
// §45 — performance degrade: bars only, no numbers
// ==========================================================================
TEST_CASE("perfMode: sheds secondary detail, keeps structure",
          "[ui31]") {
    Rig r;
    REQUIRE(!r.f.beats.empty());
    int numbered = 0;
    for (const auto& m : r.f.beats) if (m.barNo > 0) ++numbered;
    REQUIRE(numbered > 0);

    r.e.vis.perfMode = true;
    r.e.buildDetail(r.f);
    int beats0 = 0, bars = 0, nums = 0;
    for (const auto& m : r.f.beats) {
        if (m.kind == 0) ++beats0;
        else ++bars;
        if (m.barNo > 0) ++nums;
    }
    REQUIRE(beats0 == 0);                    // beats shed
    REQUIRE(bars > 0);                       // bars/downbeats kept
    REQUIRE(nums == 0);                      // numbers shed
    REQUIRE(r.f.cols.size() == 800);         // waveform intact
    r.e.vis.perfMode = false;
}

// ==========================================================================
// §33 — marker collision tiers
// ==========================================================================
TEST_CASE("cue flags: collision tiers stack labels, never markers",
          "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(0.0);
    // five hot cues 0.15 s apart = 24 px at 160 px/s (<44 px label)
    for (int i = 0; i < 5; ++i)
        r.e.cues.push_back({ 1.0 + i * 0.15, 0xFFFF0000,
                             std::to_string(i + 1), i, 0 });
    ++r.e.markersRev;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.size() == 5);
    // px positions are exact — collision moves labels only
    for (int i = 0; i < 5; ++i)
        REQUIRE(r.f.cues[(size_t)i].px ==
                Approx((float)r.e.detail.secToPx(1.0 + i * 0.15)));
    // labels alternate tiers: 0,1,0,1,0 (24 px < 44 px label width,
    // 48 px ≥ 44 on the same tier)
    REQUIRE(r.f.cues[0].tier == 0);
    REQUIRE(r.f.cues[1].tier == 1);
    REQUIRE(r.f.cues[2].tier == 0);
    REQUIRE(r.f.cues[3].tier == 1);
    REQUIRE(r.f.cues[4].tier == 0);
    // extreme crowd (>2 tiers deep) keeps the marker, drops the label
    r.e.cues.clear();
    for (int i = 0; i < 6; ++i)
        r.e.cues.push_back({ 1.0 + i * 0.05, 0xFFFF0000,
                             std::to_string(i + 1), i, 0 });
    ++r.e.markersRev;
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.size() == 6);
    int blank = 0;
    for (const auto& c : r.f.cues) if (c.label[0] == '\0') ++blank;
    REQUIRE(blank >= 2);                     // tiers 0,1,2 then hidden
}

// ==========================================================================
// §47-48 — virtualization index: only visible markers emitted
// ==========================================================================
TEST_CASE("cue index: hundreds of markers → visible window only",
          "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(0.0);                // [0,5] s window
    for (int i = 0; i < 400; ++i)
        r.e.cues.push_back({ i * 0.75, 0xFFFF0000,
                             std::to_string(i), i % 8, 0 });
    ++r.e.markersRev;
    r.e.buildDetail(r.f);
    // window covers secs [0,5] → markers at 0,.75,1.5,…,4.5 ≈ 7
    REQUIRE(r.f.cues.size() <= 8);
    REQUIRE(r.f.cues.size() >= 6);
    // a later window picks a different slice — index stays sorted
    r.e.detail.scrollTo(150.0);
    r.e.buildDetail(r.f);
    REQUIRE(r.f.cues.size() <= 8);
    for (const auto& c : r.f.cues) {
        const double s = r.e.detail.pxToSec(c.px);
        REQUIRE(s >= 149.0); REQUIRE(s <= 156.0);
    }
}

TEST_CASE("loop index: long list virtualized, left-edge overlap kept",
          "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(100.0);              // [100,105] s window
    r.e.loops.push_back({ 98.0, 101.0, false, true, 0 });  // crosses L
    for (int i = 0; i < 200; ++i)
        r.e.loops.push_back({ i * 2.0, i * 2.0 + 1.0,
                              false, true, i });
    ++r.e.markersRev;
    r.e.buildDetail(r.f);
    // crossing loop (98-101) + loops at 100-101,102-103,104-105
    REQUIRE(r.f.loops.size() == 4);
    bool crossing = false;
    for (const auto& l : r.f.loops)
        if (l.id == 0) crossing = true;
    REQUIRE(crossing);
}

// ==========================================================================
// §53 — seek transaction: begin → scrub → commit / cancel + ghost
// ==========================================================================
TEST_CASE("seek transaction: drag opens session, release commits",
          "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(0.0);
    r.e.buildDetail(r.f);
    auto& it = r.e.interact;
    const auto& v = r.e.detail;
    auto h = WaveformInteraction::hitTest(400.0, 40.0, r.f, v);
    REQUIRE(h.zone == HitZone::Wave);

    // press = immediate Seek (click semantics preserved)
    auto c = it.press(h, r.f, false, 0.0);
    REQUIRE(c.kind == WaveCmdKind::Seek);
    REQUIRE(!it.seekSession_);

    // first real drag → SeekBegin, ghost appears in the frame
    c = it.drag(500.0, v);
    REQUIRE(c.kind == WaveCmdKind::SeekBegin);
    REQUIRE(it.seekSession_);
    r.e.buildDetail(r.f);
    REQUIRE(r.f.previewPx == Approx(v.secToPx(it.seekPreviewSec)));

    // subsequent drags → ScrubSeek
    c = it.drag(600.0, v);
    REQUIRE(c.kind == WaveCmdKind::ScrubSeek);
    REQUIRE(c.a == Approx(v.pxToSec(600.0)));

    // release → SeekCommit at the last scrub position
    c = it.release(r.f, false, v);
    REQUIRE(c.kind == WaveCmdKind::SeekCommit);
    REQUIRE(c.a == Approx(v.pxToSec(600.0)).margin(0.01));
    REQUIRE(!it.seekSession_);
    r.e.buildDetail(r.f);
    REQUIRE(r.f.previewPx < 0.0);            // ghost gone
}

TEST_CASE("seek transaction: click-without-drag stays a plain Seek",
          "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(0.0);
    r.e.buildDetail(r.f);
    auto& it = r.e.interact;
    auto h = WaveformInteraction::hitTest(400.0, 40.0, r.f, r.e.detail);
    auto c = it.press(h, r.f, false, 0.0);
    REQUIRE(c.kind == WaveCmdKind::Seek);
    c = it.release(r.f, false, r.e.detail);
    REQUIRE(c.kind == WaveCmdKind::None);    // no session → no commit
}

TEST_CASE("seek transaction: cancelSeek aborts + reports", "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(0.0);
    r.e.buildDetail(r.f);
    auto& it = r.e.interact;
    auto h = WaveformInteraction::hitTest(400.0, 40.0, r.f, r.e.detail);
    it.press(h, r.f, false, 0.0);
    it.drag(550.0, r.e.detail);              // session open
    auto c = it.cancelSeek();
    REQUIRE(c.kind == WaveCmdKind::SeekCancel);
    REQUIRE(!it.seekSession_);
    REQUIRE(it.dragZone == HitZone::None);
    // second cancel is a no-op
    c = it.cancelSeek();
    REQUIRE(c.kind == WaveCmdKind::None);
    r.e.buildDetail(r.f);
    REQUIRE(r.f.previewPx < 0.0);
}

// ==========================================================================
// §11-13 — static waveform: playhead moves, view stays put
// ==========================================================================
TEST_CASE("static mode: viewport fixed while playhead traverses",
          "[ui31]") {
    Rig r;
    r.e.detail.scrollTo(0.0);
    r.e.autoScrollMode = AutoScrollMode::Off;   // static waveform
    const double scroll0 = r.e.detail.scrollSec;
    for (int i = 0; i < 30; ++i) {
        r.s.positionSec += 0.05;             // 1.5 s of playback
        r.e.tick(0.033);
        r.e.buildDetail(r.f);
        REQUIRE(r.e.detail.scrollSec == scroll0);
    }
    // playhead px advanced ~1.5 s × 160 px/s ≈ 240 px
    REQUIRE(r.f.playheadPx >
            r.e.detail.secToPx(10.0) + 200.0);
}

// ==========================================================================
// §6 — GraphicsRenderer seam over the frame plan
// ==========================================================================
TEST_CASE("FrameRenderer seam: backend consumes WaveformFrame",
          "[ui31]") {
    Rig r;
    NullFrameRenderer nr;
    REQUIRE(nr.maxFps() == 60);
    REQUIRE(!nr.nativeBatching());
    nr.drawFrame(r.f, nullptr);
    nr.drawFrame(r.f, nullptr);
    REQUIRE(nr.drawn == 2);
    REQUIRE(nr.lastColumns == 800);
}

// ==========================================================================
// §60-63 — long tracks, many markers, zoom + seek stress
// ==========================================================================
TEST_CASE("long track: 120-min build + scroll + zoom stays cheap",
          "[ui31]") {
    WaveformData w = synthWave(7200.0);      // 2 h
    BeatGrid     g = synthGrid(128.0);
    DeckSnapshot s = snapFor(3600.0, 7200.0);
    WaveformViewEngine e;
    e.wf = &w; e.grid = &g; e.snap = &s;
    e.detail.setView(7200.0, 800);
    e.detail.pxPerSec = 160.0;
    e.detail.scrollTo(3598.0);
    e.tick(0.033);

    const auto t0 = std::chrono::steady_clock::now();
    WaveformFrame f;
    for (int i = 0; i < 100; ++i) e.buildDetail(f);
    const double ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    REQUIRE(f.cols.size() == 800);
    REQUIRE(!f.beats.empty());
    // 100 frames must cost well under a second of CPU on this box —
    // that's ≪10 ms/frame even at 120 Hz budget.
    REQUIRE(ms < 800.0);
    INFO("100 frames @120min: " << ms << " ms");
}

TEST_CASE("many markers: 500 cues × long track, frames stay flat",
          "[ui31]") {
    WaveformData w = synthWave(3600.0);      // 1 h
    DeckSnapshot s = snapFor(30.0, 3600.0);
    WaveformViewEngine e;
    e.wf = &w; e.snap = &s;
    e.detail.setView(3600.0, 800);
    e.detail.pxPerSec = 160.0;
    e.detail.scrollTo(0.0);
    for (int i = 0; i < 500; ++i)
        e.cues.push_back({ i * 7.0, 0xFFFF0000,
                           std::to_string(i), i % 8, 0 });
    ++e.markersRev;
    e.tick(0.033);

    const auto t0 = std::chrono::steady_clock::now();
    WaveformFrame f;
    for (int i = 0; i < 100; ++i) {
        e.detail.scrollTo((double)(i % 40) * 60.0);   // roam the hour
        e.buildDetail(f);
        REQUIRE(f.cues.size() <= 4);         // 5 s window, 7 s spacing
    }
    const double ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    // index pays off: ~1 marker per frame from a 500-marker list
    REQUIRE(ms < 500.0);
    INFO("100 frames @1h×500cues: " << ms << " ms");
}

TEST_CASE("zoom stress: 400 rapid in/out steps, invariants hold",
          "[ui31]") {
    Rig r;
    for (int i = 0; i < 400; ++i) {
        if (i % 2)
            r.e.zoom.in(r.e.detail, ZoomAnchor::Playhead, 0.0,
                        r.s.positionSec);
        else
            r.e.zoom.out(r.e.detail, ZoomAnchor::Cursor, 400.0, 0.0);
        r.e.buildDetail(r.f);
        REQUIRE(r.f.cols.size() == 800);
        REQUIRE(std::isfinite(r.e.detail.scrollSec));
        REQUIRE(r.e.detail.scrollSec >= -1e-9);
        REQUIRE(r.e.detail.pxPerSec >=
                WaveformViewport::kMinPxPerSec);
        REQUIRE(r.e.detail.pxPerSec <=
                WaveformViewport::kMaxPxPerSec);
    }
}

TEST_CASE("seek stress: rapid snaps → jump counter, state coherent",
          "[ui31]") {
    Rig r;
    for (int i = 0; i < 60; ++i) {
        r.s.positionSec = std::fmod(r.s.positionSec + 37.0, 300.0);
        r.e.tick(0.033);
        r.e.buildDetail(r.f);
        REQUIRE(std::isfinite(r.f.playheadPx));
        // smoothed draw position always lands on the real position
        // after a jump (visual-only, transport untouched)
        REQUIRE(r.e.playhead.drawSec ==
                Approx(r.s.positionSec).margin(0.001));
    }
    REQUIRE(r.e.playhead.jumps >= 50);       // every 37 s snap counts
}

// ==========================================================================
// §50-51 — four decks share track data; frames independent
// ==========================================================================
TEST_CASE("four decks: independent frames share one WaveformData",
          "[ui31]") {
    WaveformData w = synthWave(300.0);       // one track object…
    WaveformViewEngine es[4];
    WaveformFrame fs[4];
    const double pos[4] = { 10.0, 60.0, 120.0, 240.0 };
    DeckSnapshot snaps[4];
    for (int d = 0; d < 4; ++d) {
        snaps[d] = snapFor(pos[d], 300.0);
        es[d].wf = &w;                       // …pointed at by all four
        es[d].snap = &snaps[d];
        es[d].detail.setView(300.0, 800);
        es[d].detail.pxPerSec = 160.0;
        es[d].detail.scrollTo(pos[d] - 2.5);
        es[d].tick(0.033);
        es[d].buildDetail(fs[d]);
        REQUIRE(fs[d].cols.size() == 800);
        REQUIRE(fs[d].playheadPx ==
                Approx(es[d].detail.secToPx(pos[d])).margin(0.5));
    }
    // distinct viewports — no bleed between decks (each is scrolled
    // to its own position; identical playheadPx is by design: each
    // view frames its own position 2.5 s into the window)
    REQUIRE(es[0].detail.scrollSec != es[2].detail.scrollSec);
}
