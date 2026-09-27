// Phase 30 — Track Analysis Engine: component masks, job lifecycle,
// on-demand/partial analysis, downbeat detection, tap BPM, true peak,
// spectral bands, energy score, failure persistence.
//
// Reuses the same synthetic-signal rig as analysis_tests.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

#include "core/analysis/AnalysisQueue.h"
#include "core/analysis/AnalysisService.h"
#include "core/analysis/DownbeatDetector.h"
#include "core/analysis/EnergyAnalyzer.h"
#include "core/analysis/LoudnessAnalyzer.h"
#include "core/analysis/MusicAnalysisEngine.h"
#include "core/analysis/SpectralAnalyzer.h"
#include "core/analysis/TapBpmTracker.h"
#include "core/data/AnalysisStore.h"
#include "core/data/Database.h"
#include "platform/decode/FFmpegDecoder.h"
#include "test_wav.h"

using namespace onda;
using Catch::Approx;

namespace {

const std::filesystem::path kTmp =
    std::filesystem::temp_directory_path() / "onda_analysis30_tests";

void ensureDir() { std::filesystem::create_directories(kTmp); }

Database makeDb(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::filesystem::remove(p);
    Database db;
    REQUIRE(db.open(p.string()));
    return db;
}

/// Reopen an existing database (no wipe) — for persistence tests.
Database openDb(const std::filesystem::path& p) {
    Database db;
    REQUIRE(db.open(p.string()));
    return db;
}

std::vector<float> clickTrack(double rate, double seconds, double bpm,
                              float amp = 0.8f) {
    const int64_t n = (int64_t)(rate * seconds);
    std::vector<float> x((size_t)n * 2, 0.0f);
    uint32_t rng = 0x5eed;
    auto noise = [&] {
        rng = rng * 1664525u + 1013904223u;
        return ((rng >> 9) & 0xFFFF) / 32768.0f - 1.0f;
    };
    const int64_t clickLen = (int64_t)(rate * 0.004);
    const double bps = bpm / 60.0;
    for (int64_t i = 0; i < n; ++i) {
        const double beat = (i / rate) * bps;
        const double phase = beat - std::floor(beat);
        if (phase * (60.0 / bpm) < 0.004 ||
            (1.0 - phase) * (60.0 / bpm) < 0.004) {
            if (i + clickLen <= n) {
                const float v = amp * noise();
                x[(size_t)i * 2] = x[(size_t)i * 2 + 1] = v;
            }
        }
    }
    return x;
}

std::vector<float> sineTrack(double rate, double seconds, double freq,
                             float amp = 0.5f) {
    const int64_t n = (int64_t)(rate * seconds);
    std::vector<float> x((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        const float s = amp * (float)std::sin(2.0 * 3.14159265 * freq * i / rate);
        x[(size_t)i * 2] = x[(size_t)i * 2 + 1] = s;
    }
    return x;
}

std::filesystem::path makeWav(const char* name, double bpm) {
    ensureDir();
    const auto wav = kTmp / name;
    const auto pcm = clickTrack(48000.0, 12.0, bpm);
    std::vector<int16_t> pcm16(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm16[i] = (int16_t)std::clamp(pcm[i] * 30000.0f,
                                       -32000.0f, 32000.0f);
    writeTestWav(wav, 48000, 2, pcm16);
    return wav;
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs = 45000) {
    const auto dl = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (!pred() && std::chrono::steady_clock::now() < dl)
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    return pred();
}

} // namespace

// ---------------------------------------------------------------- mask
TEST_CASE("AnalysisMask: partial analysis only runs selected components", "[an30]") {
    const auto pcm = clickTrack(48000.0, 12.0, 128.0);
    MusicAnalysisEngine engine(nullptr);

    // Only BPM — everything else must stay NotAnalyzed.
    const auto b = engine.analyzeBuffer(pcm.data(),
        (int64_t)pcm.size() / 2, 2, 48000.0, nullptr,
        AnalysisMask::Bpm);
    REQUIRE(b != nullptr);
    const auto st = [&](AnalysisComponent c) {
        return b->result.components[(int)c].state; };
    REQUIRE(st(AnalysisComponent::Decode)   == (int)AnalysisState::Analyzed);
    REQUIRE(st(AnalysisComponent::Bpm)      == (int)AnalysisState::Analyzed);
    REQUIRE(st(AnalysisComponent::Waveform) == (int)AnalysisState::NotAnalyzed);
    REQUIRE(st(AnalysisComponent::BeatGrid) == (int)AnalysisState::NotAnalyzed);
    REQUIRE(st(AnalysisComponent::Key)      == (int)AnalysisState::NotAnalyzed);
    REQUIRE(st(AnalysisComponent::Loudness) == (int)AnalysisState::NotAnalyzed);
    REQUIRE(st(AnalysisComponent::Energy)   == (int)AnalysisState::NotAnalyzed);
    REQUIRE(st(AnalysisComponent::Spectral) == (int)AnalysisState::NotAnalyzed);
    REQUIRE(st(AnalysisComponent::Transients)
                                        == (int)AnalysisState::NotAnalyzed);
    // Masked-out components never produce phantom results.
    REQUIRE(b->waveform.empty());
    REQUIRE(b->result.loudness.valid == false);
}

TEST_CASE("AnalysisMask: Essential mask yields deck-ready output", "[an30]") {
    const auto pcm = clickTrack(48000.0, 12.0, 128.0);
    MusicAnalysisEngine engine(nullptr);
    const auto b = engine.analyzeBuffer(pcm.data(),
        (int64_t)pcm.size() / 2, 2, 48000.0, nullptr,
        AnalysisMask::Essential);
    REQUIRE(b != nullptr);
    REQUIRE(b->result.bpm.bpm == Approx(128.0).margin(3.0));
    REQUIRE_FALSE(b->waveform.empty());
    // Essentials done; the deferred set is untouched.
    REQUIRE(b->result.components[(int)AnalysisComponent::Key].state
            == (int)AnalysisState::NotAnalyzed);
    REQUIRE(b->result.energy.score < 0.0f);
}

// ------------------------------------------------------ analysis rate (§11)
TEST_CASE("MusicAnalysisEngine: analysis rate is configurable", "[an30]") {
    MusicAnalysisEngine engine(nullptr);
    REQUIRE(engine.analysisRate() == Approx(48000.0));
    engine.setAnalysisRate(44100.0);
    REQUIRE(engine.analysisRate() == Approx(44100.0));
    engine.setAnalysisRate(1000.0);      // below minimum — rejected
    REQUIRE(engine.analysisRate() == Approx(44100.0));
    engine.setAnalysisRate(200000.0);    // above maximum — rejected
    REQUIRE(engine.analysisRate() == Approx(44100.0));

    // A 44.1kHz-rate analysis still detects BPM on a 44.1kHz buffer.
    const auto pcm = clickTrack(44100.0, 10.0, 120.0);
    const auto b = engine.analyzeBuffer(pcm.data(),
        (int64_t)pcm.size() / 2, 2, 44100.0, nullptr,
        AnalysisMask::Bpm);
    REQUIRE(b != nullptr);
    REQUIRE(b->result.bpm.bpm == Approx(120.0).margin(3.0));
}

// ----------------------------------------------------------- downbeat (§29)
TEST_CASE("DownbeatDetector: onsets vote the bar phase", "[an30]") {
    const double rate = 48000.0, bpm = 120.0;
    const double beatFrames = rate * 60.0 / bpm;
    // Put strong onsets on beat 2 of every bar → detector should
    // report offset 1 (0-based) in 4/4.
    BeatGrid g;
    g.setRate(rate);
    g.firstBeatFrame = 0;
    g.segments = { TempoSegment{0, 0, bpm} };
    OnsetDetector::Result on;
    for (int bar = 0; bar < 8; ++bar) {
        Onset o;
        o.frame = (int64_t)((bar * 4 + 1) * beatFrames);
        o.strength = 1.0f;
        on.onsets.push_back(o);
    }
    const auto r = DownbeatDetector::phase(on, g, 4);
    REQUIRE(r.offset == 1);
    REQUIRE(r.confidence == Approx(1.0f));

    // Empty input → safe zero result, no crash.
    OnsetDetector::Result empty;
    const auto r2 = DownbeatDetector::phase(empty, g, 4);
    REQUIRE(r2.confidence == 0.0f);
}

TEST_CASE("BeatGrid: timeSignature reflects beatsPerBar", "[an30]") {
    BeatGrid g;
    g.beatsPerBar = 4;
    const auto ts = g.timeSignature();
    REQUIRE(ts.numerator == 4);
    g.beatsPerBar = 3;
    REQUIRE(g.timeSignature().numerator == 3);
}

// ------------------------------------------------------------- tap bpm (§56)
TEST_CASE("TapBpmTracker: steady 500ms taps → 120 BPM", "[an30]") {
    TapBpmTracker t;
    REQUIRE(t.tap(1000) == 0.0);          // first tap anchors
    t.tap(1500);
    t.tap(2000);
    const double bpm = t.tap(2500);
    REQUIRE(bpm == Approx(120.0).margin(3.0));
}

TEST_CASE("TapBpmTracker: long gap resets measurement", "[an30]") {
    TapBpmTracker t(2000);
    t.tap(0);
    t.tap(500);
    // 3s gap — stale taps dropped, next tap re-anchors.
    REQUIRE(t.tap(4000) == 0.0);
    REQUIRE(t.tap(4500) == Approx(120.0).margin(2.0));
}

TEST_CASE("TapBpmTracker: jitter uses median, not mean", "[an30]") {
    TapBpmTracker t;
    t.tap(0); t.tap(500); t.tap(1000); t.tap(1500);
    t.tap(2400);                          // one sloppy outlier interval
    const double bpm = t.bpm();
    // Median interval of {500,500,500,900} = 500 → 120 BPM.
    REQUIRE(bpm == Approx(120.0).margin(3.0));
}

// ------------------------------------------------------------ loudness (§45)
TEST_CASE("LoudnessAnalyzer: truePeak never below sample peak", "[an30]") {
    const auto pcm = sineTrack(48000.0, 2.0, 997.0, 0.9f);
    LoudnessAnalyzer la;
    const auto r = la.analyze(pcm.data(),
                              (int64_t)pcm.size() / 2, 2, 48000.0);
    REQUIRE(r.valid);
    REQUIRE(r.truePeak >= r.peak - 1e-6f);
    REQUIRE(r.peak == Approx(0.9f).margin(0.02));
}

// ------------------------------------------------------ spectral (§42, §43)
TEST_CASE("SpectralAnalyzer: bass sine lands in the low band", "[an30]") {
    const auto pcm = sineTrack(48000.0, 2.0, 80.0, 0.6f);
    SpectralAnalyzer sa;
    const auto o = sa.analyze(pcm.data(), (int64_t)pcm.size() / 2,
                              2, 48000.0, 256);
    // Butterworth crossover leaks phase error into `mid` (documented
    // approximation) — assert dominance, not a razor margin.
    REQUIRE(o.result.low > 0.70f);
    REQUIRE(o.result.low > o.result.mid);
    REQUIRE(o.result.low > o.result.high);
    REQUIRE(o.result.high < 0.10f);
    REQUIRE(o.result.centroidHz < 300.0f);
    REQUIRE(o.bands.size() == (size_t)256 * 3);
    // First bins (the low-frequency region) carry the energy.
    REQUIRE(o.bands[0] > 0.70f);
}

TEST_CASE("SpectralAnalyzer: bright sine lands in the high band", "[an30]") {
    const auto pcm = sineTrack(48000.0, 2.0, 10000.0, 0.6f);
    SpectralAnalyzer sa;
    const auto o = sa.analyze(pcm.data(), (int64_t)pcm.size() / 2,
                              2, 48000.0, 256);
    REQUIRE(o.result.high > 0.70f);
    REQUIRE(o.result.high > o.result.mid);
    REQUIRE(o.result.low < 0.10f);
    REQUIRE(o.result.centroidHz > 4000.0f);
}

// ---------------------------------------------------------- energy (§44)
TEST_CASE("EnergyAnalyzer: louder track scores higher than quiet", "[an30]") {
    WaveformData wf;
    wf.sampleRate = 48000.0;
    wf.overview.resize(200, {0.5f, 0.6f, 0.4f});

    LoudnessResult loud;
    loud.valid = true;
    loud.integratedLufs = -8.0f;   // club-loud
    const auto hi = EnergyAnalyzer::score(loud, wf);

    loud.integratedLufs = -30.0f;  // quiet
    const auto lo = EnergyAnalyzer::score(loud, wf);

    REQUIRE(hi.score > lo.score);
    REQUIRE(hi.score >= 0.0f);
    REQUIRE(hi.score <= 1.0f);
    REQUIRE(hi.confidence > 0.0f);
}

// -------------------------------------------------------- engine pipeline
TEST_CASE("Full pipeline: spectral + energy results populated", "[an30]") {
    const auto pcm = clickTrack(48000.0, 12.0, 128.0);
    MusicAnalysisEngine engine(nullptr);
    const auto b = engine.analyzeBuffer(pcm.data(),
        (int64_t)pcm.size() / 2, 2, 48000.0, nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(b->result.components[(int)AnalysisComponent::Spectral].state
            == (int)AnalysisState::Analyzed);
    REQUIRE(b->result.components[(int)AnalysisComponent::Energy].state
            == (int)AnalysisState::Analyzed);
    REQUIRE(b->result.energy.score >= 0.0f);
    REQUIRE(b->result.energy.score <= 1.0f);
    REQUIRE(b->result.spectral.centroidHz >= 0.0f);
    REQUIRE_FALSE(b->waveform.bands.empty());
}

// ---------------------------------------------------- queue / job state (§3)
TEST_CASE("AnalysisQueue: snapshot exposes job state + mask + trackId", "[an30]") {
    auto db = makeDb(kTmp / "snap.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);
    svc.pauseQueue();                    // keep jobs in queue, don't run
    const int64_t id = svc.analyzePartial(
        "x.wav", AnalysisMask::Bpm,
        (int)AnalysisPriority::Normal, 42);
    const auto jobs = svc.queue().snapshotJobs();
    svc.resumeQueue();
    REQUIRE(jobs.size() == 1);
    REQUIRE(jobs[0].jobId == id);
    REQUIRE(jobs[0].trackId == 42);
    REQUIRE(jobs[0].requestedAnalyses == AnalysisMask::Bpm);
    REQUIRE(jobs[0].state == (int)JobState::Queued);
    svc.cancel(id);
}

TEST_CASE("AnalysisQueue: min-priority gate holds back low jobs", "[an30]") {
    auto db = makeDb(kTmp / "gate.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);
    svc.pauseQueue();
    const int64_t lo = svc.analyzePartial(
        "low.wav", AnalysisMask::Bpm, (int)AnalysisPriority::Low);
    svc.resumeQueue();
    // Playback active → only HIGH+ runs.
    svc.setPlaybackActive(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto jobs = svc.queue().snapshotJobs();
    bool stillQueued = false;
    for (const auto& j : jobs)
        if (j.jobId == lo &&
            j.state == (int)JobState::Queued) stillQueued = true;
    svc.setPlaybackActive(false);
    svc.cancel(lo);
    REQUIRE(stillQueued);
}

// --------------------------------------------------- on-demand (§52, §53)
TEST_CASE("analyzeForDeck enqueues essentials + background follow-up", "[an30]") {
    auto db = makeDb(kTmp / "deck.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);
    svc.pauseQueue();
    const int64_t job = svc.analyzeForDeck("deck.wav", 7);
    const auto jobs = svc.queue().snapshotJobs();
    REQUIRE(jobs.size() == 2);
    bool sawEssential = false, sawRest = false;
    for (const auto& j : jobs) {
        if (j.requestedAnalyses == AnalysisMask::Essential &&
            j.priority == (int)AnalysisPriority::Realtime &&
            j.trackId == 7 && j.jobId == job)
            sawEssential = true;
        if (j.requestedAnalyses ==
                (AnalysisMask::All & ~AnalysisMask::Essential) &&
            j.priority == (int)AnalysisPriority::Low)
            sawRest = true;
    }
    REQUIRE(sawEssential);
    REQUIRE(sawRest);
    for (const auto& j : jobs) svc.cancel(j.jobId);
    svc.resumeQueue();
}

// ------------------------------------------------- failure persistence (§60)
TEST_CASE("saveFailed leaves Failed state + error, track kept", "[an30]") {
    auto db = makeDb(kTmp / "fail.db");
    AnalysisStore store(db);
    store.saveFailed("broken.mp3", "decoder: unsupported codec");
    REQUIRE(store.stateFor("broken.mp3") == AnalysisState::Failed);
    // load() only serves Analyzed bundles — the error comes via the
    // dedicated side channel.
    REQUIRE(store.load("broken.mp3") == nullptr);
    REQUIRE(store.errorFor("broken.mp3").find("unsupported codec")
            != std::string::npos);
    // Track row itself must survive — no auto-removal.
    REQUIRE(store.trackIdFor("broken.mp3") > 0);
}

// ------------------------------------------------ resume / merge (§8, §51)
TEST_CASE("Partial analysis merges with prior full analysis", "[an30]") {
    const auto wav = makeWav("merge.wav", 128.0);
    auto db = makeDb(kTmp / "merge.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);

    // Full analysis first.
    const auto full = svc.analyzeTrackSync(wav.string());
    REQUIRE(full != nullptr);
    REQUIRE(full->result.components[(int)AnalysisComponent::Waveform].state
            == (int)AnalysisState::Analyzed);
    REQUIRE(full->result.components[(int)AnalysisComponent::Loudness].state
            == (int)AnalysisState::Analyzed);
    REQUIRE_FALSE(full->waveform.empty());
    {
        AnalysisStore check(db);
        const auto prior = check.load(wav.string());
        REQUIRE(prior != nullptr);
        REQUIRE_FALSE(prior->waveform.empty());
    }

    // Re-run only BPM — waveform/loudness must be preserved from before.
    std::atomic<int> done{0};
    svc.addListener([&](const AnalysisQueue::Notification& n) {
        if (n.event == AnalysisQueue::Event::Completed) ++done;
    });
    svc.analyzePartial(wav.string(), AnalysisMask::Bpm);
    REQUIRE(waitFor([&] { return done.load() >= 1; }));

    const auto b = svc.getAnalysis(wav.string());
    REQUIRE(b != nullptr);
    REQUIRE(b->result.components[(int)AnalysisComponent::Waveform].state
            == (int)AnalysisState::Analyzed);
    REQUIRE(b->result.components[(int)AnalysisComponent::Loudness].state
            == (int)AnalysisState::Analyzed);
    REQUIRE_FALSE(b->waveform.empty());
    REQUIRE(b->result.loudness.valid);
}

// ------------------------------------------ persistence round-trip (v11)
TEST_CASE("Store: truePeak/centroid/energy/bands survive save+load", "[an30]") {
    const auto wav = makeWav("persist.wav", 124.0);
    auto db = makeDb(kTmp / "persist.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);
    const auto b = svc.analyzeTrackSync(wav.string());
    REQUIRE(b != nullptr);

    auto db2 = openDb(kTmp / "persist.db");   // reopen same file
    AnalysisStore store(db2);
    const auto loaded = store.load(wav.string());
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->result.loudness.truePeak
            == Approx(b->result.loudness.truePeak).margin(1e-4));
    REQUIRE(loaded->result.spectral.centroidHz
            == Approx(b->result.spectral.centroidHz).margin(1e-3));
    REQUIRE(loaded->result.energy.score
            == Approx(b->result.energy.score).margin(1e-4));
    REQUIRE(loaded->result.energy.confidence
            == Approx(b->result.energy.confidence).margin(1e-4));
    REQUIRE(loaded->waveform.bands.size() == b->waveform.bands.size());
    REQUIRE(loaded->result.components[(int)AnalysisComponent::Energy].state
            == (int)AnalysisState::Analyzed);
}

// ------------------------------------------------------ tap bpm → service
TEST_CASE("applyEdit TapBpm: taps become a BPM override", "[an30]") {
    const auto wav = makeWav("tap.wav", 128.0);
    auto db = makeDb(kTmp / "tap.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);
    REQUIRE(svc.analyzeTrackSync(wav.string()) != nullptr);

    svc.applyEdit(wav.string(), AnalysisEdit::TapBpm, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(480));
    svc.applyEdit(wav.string(), AnalysisEdit::TapBpm, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(480));
    svc.applyEdit(wav.string(), AnalysisEdit::TapBpm, 0.0);
    // ~480ms intervals ≈ 125 BPM — the override (not the detection)
    // must drive the effective value.
    const double bpm = svc.getBpm(wav.string());
    REQUIRE(bpm == Approx(125.0).margin(12.0));
}

// ---------------------------------------------------------- throughput (§68)
TEST_CASE("Queue throughput: 24 tracks complete without stalls", "[an30]") {
    ensureDir();
    auto db = makeDb(kTmp / "through.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);
    std::atomic<int> done{0}, failed{0};
    svc.addListener([&](const AnalysisQueue::Notification& n) {
        if (n.event == AnalysisQueue::Event::Completed) ++done;
        if (n.event == AnalysisQueue::Event::Failed) ++failed;
    });
    const auto wav = makeWav("through.wav", 128.0);
    constexpr int kN = 24;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) svc.analyzeTrack(wav.string());
    REQUIRE(waitFor([&] { return done.load() + failed.load() >= kN; },
                    120000));
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[an30-through] %d jobs in %.1fs (%.2f jobs/s), %d failed\n",
                kN, s, kN / s, failed.load());
    REQUIRE(done.load() >= kN - 1);   // duplicates may coalesce
    REQUIRE(failed.load() == 0);
}

// ------------------------------------------------- isolation (§69, headless)
TEST_CASE("Isolation: paused analysis never touches queue jobs", "[an30]") {
    auto db = makeDb(kTmp / "iso.db");
    AnalysisService svc(
        [] { return std::make_unique<FFmpegDecoder>(); }, db);
    svc.pauseQueue();
    const auto wav = makeWav("iso.wav", 100.0);
    const int64_t job = svc.analyzeTrack(wav.string());
    // While paused, a busy loop stands in for RT load — nothing
    // should steal the worker.
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < end) { /* spin */ }
    auto jobs = svc.queue().snapshotJobs();
    bool queued = false;
    for (const auto& j : jobs)
        if (j.jobId == job && j.state == (int)JobState::Queued)
            queued = true;
    REQUIRE(queued);
    svc.resumeQueue();
    svc.cancel(job);
}
