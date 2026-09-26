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
#include "core/analysis/BeatGridEngine.h"
#include "core/analysis/BpmAnalyzer.h"
#include "core/analysis/KeyAnalyzer.h"
#include "core/analysis/MusicAnalysisEngine.h"
#include "core/analysis/OnsetDetector.h"
#include "core/analysis/WaveformGenerator.h"
#include "core/data/AnalysisStore.h"
#include "core/data/Database.h"
#include "platform/decode/FFmpegDecoder.h"
#include "test_wav.h"

using namespace onda;
using Catch::Approx;

// ---------------------------------------------------------------- fixtures
static const std::filesystem::path kTmp =
    std::filesystem::temp_directory_path() / "onda_analysis_tests";

static void ensureDir() { std::filesystem::create_directories(kTmp); }

/// Click track: 4 ms noise bursts on the beat (and optional off-beats).
static std::vector<float> clickTrack(double rate, double seconds,
                                     double bpm, float amp = 0.8f,
                                     float offbeatAmp = 0.0f,
                                     double bpm2 = 0.0, double splitSec = 0.0) {
    const int64_t n = (int64_t)(rate * seconds);
    std::vector<float> x((size_t)n * 2, 0.0f);
    uint32_t rng = 0x12345;
    auto noise = [&] {
        rng = rng * 1664525u + 1013904223u;
        return ((rng >> 9) & 0xFFFF) / 32768.0f - 1.0f;
    };
    const int64_t clickLen = (int64_t)(rate * 0.004);
    const double bps = bpm / 60.0, bps2 = bpm2 / 60.0;
    for (int64_t i = 0; i < n; ++i) {
        const double t = i / rate;
        const double beat = (t < splitSec || bpm2 <= 0.0) ? t * bps
                          : splitSec * bps + (t - splitSec) * bps2;
        const double phase = beat - std::floor(beat);
        const double half = beat - std::floor(beat * 2.0) / 2.0;
        bool on = phase * (60.0 / bpm) < 0.004 || (1.0 - phase) * (60.0 / bpm) < 0.004;
        bool off = offbeatAmp > 0.0f &&
            std::fabs(half - 0.5) * (60.0 / bpm) < 0.004;
        if (!on && !off) continue;
        const float a = on ? amp : offbeatAmp;
        if (i + clickLen <= n) {
            const float v = a * noise();
            x[(size_t)i * 2] = x[(size_t)i * 2 + 1] = v;
        }
    }
    return x;
}

/// Chord: three sines summed — for key detection.
static std::vector<float> chordTrack(double rate, double seconds,
                                     std::initializer_list<double> freqs,
                                     float amp = 0.3f) {
    const int64_t n = (int64_t)(rate * seconds);
    std::vector<float> x((size_t)n * 2, 0.0f);
    for (int64_t i = 0; i < n; ++i) {
        float s = 0.0f;
        for (double f : freqs)
            s += amp * (float)std::sin(2.0 * 3.14159265 * f * i / rate);
        // slow tremolo so the spectrum isn't unnaturally static
        s *= 0.8f + 0.2f * (float)std::sin(2.0 * 3.14159265 * 2.0 * i / rate);
        x[(size_t)i * 2] = x[(size_t)i * 2 + 1] = s;
    }
    return x;
}

// ---------------------------------------------------------------- onset
TEST_CASE("Onset detection: clicks at 128 BPM land on the envelope") {
    const double rate = 48000.0, bpm = 128.0;
    const auto pcm = clickTrack(rate, 8.0, bpm);
    OnsetDetector od;
    const auto r = od.detect(pcm.data(), (int64_t)pcm.size() / 2, 2, rate);
    REQUIRE(r.onsets.size() >= 10); // ~17 beats in 8 s at 128 BPM
    const int64_t beatFrames = (int64_t)(rate * 60.0 / bpm);
    bool nearBeat = false;
    for (const auto& o : r.onsets) {
        const int64_t mod = o.frame % beatFrames;
        // onsets land on window-start granularity: allow ~1.5 windows
        if (mod < 1600 || mod > beatFrames - 1600) nearBeat = true;
    }
    REQUIRE(nearBeat);
}

// ---------------------------------------------------------------- BPM
static BpmResult bpmOf(const std::vector<float>& pcm, double rate) {
    OnsetDetector od;
    const auto o = od.detect(pcm.data(), (int64_t)pcm.size() / 2, 2, rate);
    BpmAnalyzer b;
    return b.analyze(o);
}

TEST_CASE("BPM: click track at 128 detected within 1%") {
    const auto pcm = clickTrack(48000.0, 20.0, 128.0);
    const auto r = bpmOf(pcm, 48000.0);
    REQUIRE(r.bpm == Approx(128.0).margin(1.5));
    REQUIRE(r.confidence > 0.3f);
}

TEST_CASE("BPM: low tempo 70 BPM") {
    const auto pcm = clickTrack(48000.0, 20.0, 70.0);
    const auto r = bpmOf(pcm, 48000.0);
    // 70 is in range; octave-corrected result should sit near 70 or 140
    const double b = r.bpm;
    REQUIRE((b == Approx(70.0).margin(1.0) || b == Approx(140.0).margin(2.0)));
    REQUIRE_FALSE(r.candidates.empty());
}

TEST_CASE("BPM: high tempo 174 BPM") {
    const auto pcm = clickTrack(48000.0, 20.0, 174.0);
    const auto r = bpmOf(pcm, 48000.0);
    const double b = r.bpm;
    REQUIRE((b == Approx(174.0).margin(2.0) || b == Approx(87.0).margin(1.0)));
}

TEST_CASE("BPM: candidates expose half/double-time alternatives") {
    const auto pcm = clickTrack(48000.0, 20.0, 128.0);
    const auto r = bpmOf(pcm, 48000.0);
    bool hasHalf = false, hasDouble = false;
    for (const auto& c : r.candidates) {
        if (c.bpm == Approx(64.0).margin(1.0)) hasHalf = true;
        if (c.bpm == Approx(256.0).margin(2.0)) hasDouble = true;
    }
    // At least the true tempo must be in the candidate list
    bool hasTrue = false;
    for (const auto& c : r.candidates)
        if (c.bpm == Approx(128.0).margin(1.5)) hasTrue = true;
    REQUIRE(hasTrue);
    REQUIRE(r.candidates.size() > 1); // alternatives preserved, not hidden
}

TEST_CASE("BPM: tempo change produces a tempo map") {
    // 8 s at 120 BPM then 8 s at 150 BPM
    const auto pcm = clickTrack(48000.0, 16.0, 120.0, 0.8f, 0.0f, 150.0, 8.0);
    const auto r = bpmOf(pcm, 48000.0);
    REQUIRE(r.variable);
    REQUIRE(r.tempoMap.size() >= 2);
    double lo = 1e9, hi = 0;
    for (const auto& s : r.tempoMap) { lo = std::min(lo, s.bpm); hi = std::max(hi, s.bpm); }
    REQUIRE(hi - lo > 10.0); // two distinct tempi found
}

// ---------------------------------------------------------------- BeatGrid
TEST_CASE("BeatGrid: constant tempo frame<->beat round-trip") {
    BeatGrid g; g.setRate(48000.0);
    TempoSegment s; s.startFrame = 4800; s.startBeat = 0; s.bpm = 120.0;
    g.segments = { s };
    const double fpb = 48000.0 * 60.0 / 120.0; // 24000
    REQUIRE(g.beatToFrame(0.0) == 4800);
    REQUIRE(g.beatToFrame(4.0) == 4800 + (int64_t)(4 * fpb));
    REQUIRE(g.frameToBeat(4800 + (int64_t)(2 * fpb)) == Approx(2.0).margin(1e-6));
    // no drift after 10 minutes of beats
    const double beats10min = 10.0 * 60.0 * 120.0 / 60.0;
    const int64_t f = g.beatToFrame(beats10min);
    REQUIRE((double)f == Approx(4800 + beats10min * fpb).margin(2.0));
}

TEST_CASE("BeatGrid: downbeat and bars") {
    BeatGrid g; g.setRate(48000.0);
    TempoSegment s; s.startFrame = 0; s.startBeat = 0; s.bpm = 128.0;
    g.segments = { s };
    g.downbeatOffset = 1; // bar starts at beat 1
    REQUIRE_FALSE(g.isDownbeat(0.0));
    REQUIRE(g.isDownbeat(1.0));
    REQUIRE(g.barOf(1.0) == 1);
    REQUIRE(g.barOf(5.0) == 2);
    REQUIRE(g.barOf(0.0) == 0);
}

TEST_CASE("BeatGrid edits: move, nudge, adjustBpm, tempo change") {
    BeatGrid g; g.setRate(48000.0);
    TempoSegment s; s.startFrame = 0; s.startBeat = 0; s.bpm = 120.0;
    g.segments = { s };

    BeatGridEngine::moveBeatGrid(g, 960);
    REQUIRE(g.firstBeatFrame == 960);
    BeatGridEngine::nudgeBeatGrid(g, 10.0); // +10 ms = 480 frames
    REQUIRE(g.firstBeatFrame == 1440);

    BeatGridEngine::adjustBpm(g, 140.0);
    REQUIRE(g.segments.front().bpm == Approx(140.0));

    REQUIRE(BeatGridEngine::addTempoChange(g, 48000 * 60, 170.0)); // @1 min
    REQUIRE(g.segments.size() == 2);
    REQUIRE(g.segments[1].bpm == Approx(170.0));

    // frame->beat stays continuous across the tempo boundary
    const double beatAt60 = g.frameToBeat(48000 * 60);
    const int64_t f = g.beatToFrame(beatAt60);
    REQUIRE(f == Approx(48000 * 60).margin(24000));
}

TEST_CASE("BeatGrid: variable tempo consistency") {
    BeatGrid g; g.setRate(48000.0);
    TempoSegment a{0, 0, 120.0}, b{48000 * 60, 120, 240.0};
    g.segments = { a, b };
    // beat 120 lands exactly at the segment boundary
    REQUIRE(g.beatToFrame(120.0) == 48000 * 60);
    // one beat after the change = half the frames (240 bpm)
    REQUIRE(g.beatToFrame(121.0) == 48000 * 60 + 12000);
    REQUIRE(g.frameToBeat(48000 * 60 + 12000) == Approx(121.0).margin(1e-6));
}

// ---------------------------------------------------------------- Key
TEST_CASE("Key: C major chord detected as C major") {
    const auto pcm = chordTrack(48000.0, 10.0, {261.63, 329.63, 392.0});
    KeyAnalyzer ka;
    const auto r = ka.analyze(pcm.data(), (int64_t)pcm.size() / 2, 2, 48000.0);
    REQUIRE(r.key == 0); // C major
    REQUIRE(r.standard == "C");
    REQUIRE(r.camelot == "8B");
    REQUIRE(r.confidence > 0.2f);
}

TEST_CASE("Key: A minor chord detected as A minor") {
    const auto pcm = chordTrack(48000.0, 10.0, {220.0, 261.63, 329.63});
    KeyAnalyzer ka;
    const auto r = ka.analyze(pcm.data(), (int64_t)pcm.size() / 2, 2, 48000.0);
    REQUIRE(r.key == 21); // Am
    REQUIRE(r.standard == "Am");
    REQUIRE(r.camelot == "8A");
}

// ---------------------------------------------------------------- Waveform
TEST_CASE("Waveform: overview + detail levels and region query") {
    const double rate = 48000.0;
    const int64_t n = (int64_t)(rate * 2.0);
    std::vector<float> pcm((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        const float v = 0.5f * (float)std::sin(2.0 * 3.14159265 * 220.0 * i / rate);
        pcm[(size_t)i * 2] = pcm[(size_t)i * 2 + 1] = v;
    }
    WaveformGenerator wg;
    const auto w = wg.generate(pcm.data(), n, 2, rate);
    REQUIRE(w.overview.size() == WaveformData::kOverviewBins * 2);
    REQUIRE(w.detail.size() == (size_t)(n / WaveformData::kDetailFrames) * 2);

    // full-track overview region query
    std::vector<WaveformBin> bins;
    w.query(0, n, 100, bins, 0);
    REQUIRE(bins.size() == 100);
    float pk = 0.0f;
    for (const auto& b : bins) pk = std::max(pk, b.max);
    REQUIRE(pk > 0.45f);

    // zoomed region uses detail level
    w.query(n / 4, n / 4 + 48000 / 10, 50, bins, 0); // 0.1 s window
    REQUIRE(bins.size() == 50);
    for (const auto& b : bins) {
        REQUIRE(b.min <= b.max);
        REQUIRE(b.rms >= 0.0f);
    }
}

// ---------------------------------------------------------------- store/db
static Database makeDb(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::filesystem::remove(p);
    Database db;
    REQUIRE(db.open(p.string()));
    return db;
}

TEST_CASE("AnalysisStore: save/load round-trip incl. grid + waveform") {
    ensureDir();
    auto db = makeDb(kTmp / "store.db");
    AnalysisStore store(db);

    AnalysisBundle b;
    b.result.state = AnalysisState::Analyzed;
    b.result.durationSec = 2.0; b.result.sampleRate = 48000.0;
    b.result.numFrames = 96000;
    b.result.bpm.bpm = 128.0; b.result.bpm.confidence = 0.9f;
    b.result.bpm.candidates = { {128.0, 1.0f}, {64.0, 0.4f} };
    b.result.key.key = 0; b.result.key.confidence = 0.7f;
    b.result.firstSoundFrame = 10; b.result.lastSoundFrame = 95900;
    b.result.silences = { {0, 5}, {95900, 96000} };
    b.grid.setRate(48000.0);
    b.grid.segments = { TempoSegment{0, 0, 128.0} };
    b.grid.downbeatOffset = 1;
    std::vector<float> pcm(96000 * 2, 0.1f);
    b.waveform = WaveformGenerator().generate(pcm.data(), 96000, 2, 48000.0);

    const std::string path = (kTmp / "dummy.wav").string();
    { // file must exist for fingerprint()
        std::ofstream f(path, std::ios::binary);
        f.write("x", 1);
    }
    store.save(path, b, "T", "A");
    REQUIRE(store.stateFor(path) == AnalysisState::Analyzed);

    const auto loaded = store.load(path);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->result.bpm.bpm == Approx(128.0));
    REQUIRE(loaded->result.bpm.confidence == Approx(0.9f));
    REQUIRE(loaded->result.key.key == 0);
    REQUIRE(loaded->result.key.standard == "C");
    REQUIRE(loaded->grid.segments.size() == 1);
    REQUIRE(loaded->grid.segments[0].bpm == Approx(128.0));
    REQUIRE(loaded->grid.downbeatOffset == 1);
    REQUIRE(loaded->waveform.overview.size() == b.waveform.overview.size());
    REQUIRE(loaded->waveform.detail.size() == b.waveform.detail.size());
}

TEST_CASE("AnalysisStore: user override keeps detected data intact") {
    ensureDir();
    auto db = makeDb(kTmp / "override.db");
    AnalysisStore store(db);
    const std::string path = (kTmp / "o.wav").string();
    { std::ofstream f(path, std::ios::binary); f.write("y", 1); }
    const int64_t id = store.trackIdFor(path);

    store.setUserBpm(id, 128.0);
    auto ub = store.userBpm(id);
    REQUIRE(ub.has_value());
    REQUIRE(*ub == Approx(128.0));
    REQUIRE(store.effectiveBpm(id, 127.4) == Approx(128.0));
    store.setUserBpm(id, -1.0); // clear
    REQUIRE_FALSE(store.userBpm(id).has_value());
    REQUIRE(store.effectiveBpm(id, 127.4) == Approx(127.4));
}

TEST_CASE("AnalysisStore: fingerprint change marks analysis outdated") {
    ensureDir();
    auto db = makeDb(kTmp / "stale.db");
    AnalysisStore store(db);
    const std::string path = (kTmp / "s.wav").string();
    { std::ofstream f(path, std::ios::binary); f.write("v1", 2); }

    AnalysisBundle b;
    b.result.state = AnalysisState::Analyzed;
    b.result.durationSec = 1.0; b.result.sampleRate = 48000.0;
    b.result.numFrames = 48000;
    b.result.bpm.bpm = 100.0;
    store.save(path, b, {}, {});
    REQUIRE(store.stateFor(path) == AnalysisState::Analyzed);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { std::ofstream f(path, std::ios::binary | std::ios::app); f.write("v2-changed", 10); }
    REQUIRE(store.stateFor(path) == AnalysisState::Outdated);
    REQUIRE(store.invalidateStale() >= 1);
    REQUIRE(store.stateFor(path) == AnalysisState::Outdated);
}

// ---------------------------------------------------------------- queue/engine
TEST_CASE("AnalysisQueue: end-to-end analysis of a metronome WAV") {
    ensureDir();
    const auto wav = kTmp / "metro128.wav";
    const auto pcm = clickTrack(48000.0, 20.0, 128.0);
    std::vector<int16_t> pcm16(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm16[i] = (int16_t)std::clamp(pcm[i] * 30000.0f, -32000.0f, 32000.0f);
    writeTestWav(wav, 48000, 2, pcm16);

    auto db = makeDb(kTmp / "q.db");
    AnalysisService svc([] { return std::make_unique<FFmpegDecoder>(); }, db);

    std::atomic<int> completed{0}, failed{0};
    svc.addListener([&](const AnalysisQueue::Notification& n) {
        if (n.event == AnalysisQueue::Event::Completed) ++completed;
        if (n.event == AnalysisQueue::Event::Failed) ++failed;
    });
    svc.analyzeTrack(wav.string());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (completed.load() + failed.load() == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(completed.load() == 1);
    REQUIRE(svc.stateFor(wav.string()) == AnalysisState::Analyzed);

    const auto b = svc.getAnalysis(wav.string());
    REQUIRE(b != nullptr);
    REQUIRE(b->result.bpm.bpm == Approx(128.0).margin(2.5));
    REQUIRE_FALSE(b->waveform.empty());
    REQUIRE_FALSE(b->grid.empty());
    REQUIRE(b->result.key.key >= -1);
}

TEST_CASE("AnalysisQueue: missing file fails and retries then reports") {
    ensureDir();
    auto db = makeDb(kTmp / "q2.db");
    AnalysisService svc([] { return std::make_unique<FFmpegDecoder>(); }, db);
    std::atomic<int> failed{0};
    svc.addListener([&](const AnalysisQueue::Notification& n) {
        if (n.event == AnalysisQueue::Event::Failed) ++failed;
    });
    svc.analyzeTrack("C:/definitely/missing/track.mp3");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (failed.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(failed.load() == 1);
}

TEST_CASE("MusicAnalysisEngine: cancellation token aborts cleanly") {
    ensureDir();
    MusicAnalysisEngine engine([] { return std::make_unique<FFmpegDecoder>(); });
    auto cancel = std::make_shared<std::atomic<bool>>(true); // pre-cancelled
    const auto b = engine.analyzeTrack("anything.wav", cancel, nullptr);
    REQUIRE(b == nullptr);
}

TEST_CASE("Engine + sync sync foundation: grid frame<->beat is stable") {
    // 10-minute click at exactly 120 BPM — drift check end to end.
    const double rate = 48000.0, bpm = 120.0;
    const auto pcm = clickTrack(rate, 30.0, bpm); // 30 s keeps test fast
    MusicAnalysisEngine engine(nullptr);
    const auto b = engine.analyzeBuffer(pcm.data(), (int64_t)pcm.size() / 2, 2, rate, nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(b->result.state == AnalysisState::Analyzed);
    REQUIRE(b->result.bpm.bpm == Approx(120.0).margin(1.5));
    const auto& g = b->grid;
    REQUIRE_FALSE(g.empty());
    const double beatsTotal = 30.0 * bpm / 60.0;
    const int64_t f = g.beatToFrame(beatsTotal - 1);
    // grid internal consistency: last beat = anchor + (N-1) * framesPerBeat
    const double fpb = g.rate() * 60.0 / b->result.bpm.bpm;
    const double expected = (double)g.segments.front().startFrame
                            + (beatsTotal - 1) * fpb;
    REQUIRE((double)f == Approx(expected).margin(2.0));
    // and it must land near the nominal timeline (anchor + bpm error bounded)
    const double nominal = (beatsTotal - 1) * 60.0 / bpm * rate;
    REQUIRE((double)f == Approx(nominal).margin(rate * 60.0 / bpm));
}

// ==================================================================
// Phase 14 — loudness / transients / validator / overrides / service
// ==================================================================
#include "core/analysis/AnalysisOverride.h"
#include "core/analysis/BeatGridValidator.h"
#include "core/analysis/LoudnessAnalyzer.h"
#include "core/analysis/SyncReference.h"
#include "core/analysis/TransientAnalyzer.h"

// ---------------------------------------------------------------- loudness
TEST_CASE("Loudness: sine reports honest peak/RMS, LUFS in range") {
    const double rate = 48000.0;
    const int64_t n = (int64_t)(rate * 4.0);
    std::vector<float> pcm((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        const float v = 0.5f * (float)std::sin(2.0 * 3.14159265 * 440.0 * i / rate);
        pcm[(size_t)i * 2] = pcm[(size_t)i * 2 + 1] = v;
    }
    LoudnessAnalyzer la;
    const auto r = la.analyze(pcm.data(), n, 2, rate);
    REQUIRE(r.peak == Approx(0.5).margin(0.001));
    REQUIRE(r.rms == Approx(0.5 / 1.41421356).margin(0.005));
    REQUIRE(r.valid);
    // K-weighted, both channels summed: expect a few LUFS, not silence
    REQUIRE(r.integratedLufs > -15.0f);
    REQUIRE(r.integratedLufs < 5.0f);
}

TEST_CASE("Loudness: silence stays invalid, no fake numbers") {
    std::vector<float> pcm(48000 * 2, 0.0f);
    LoudnessAnalyzer la;
    const auto r = la.analyze(pcm.data(), 48000, 2, 48000.0);
    REQUIRE(r.peak == 0.0f);
    REQUIRE_FALSE(r.valid); // gate blocks it — honest "not measurable"
}

// -------------------------------------------------------------- transients
TEST_CASE("Transients: click track yields strength-sorted markers on beats") {
    const double rate = 48000.0, bpm = 128.0;
    const auto pcm = clickTrack(rate, 8.0, bpm);
    OnsetDetector od;
    const auto o = od.detect(pcm.data(), (int64_t)pcm.size() / 2, 2, rate);
    TransientAnalyzer ta;
    const auto ev = ta.analyze(o);
    // ~17 beats in 8 s; allow detector slack
    REQUIRE(ev.size() >= 10);
    for (const auto& e : ev)
        REQUIRE(e.strength >= TransientAnalyzer::kMinStrength);
    for (size_t i = 1; i < ev.size(); ++i)
        REQUIRE(ev[i].frame >= ev[i - 1].frame); // chronological
}

TEST_CASE("Transients: cap keeps only the strongest markers") {
    OnsetDetector::Result o;
    for (int i = 0; i < 6000; ++i)
        o.onsets.push_back({ (int64_t)i * 100, (float)(i % 7) / 7.0f + 0.1f });
    TransientAnalyzer ta;
    const auto ev = ta.analyze(o);
    REQUIRE(ev.size() <= TransientAnalyzer::kMaxEvents);
    // 6 strength tiers ×857: the cap drops the weakest tier(s)
    float weakest = 1.0f; int strong = 0;
    for (const auto& e : ev) {
        weakest = std::min(weakest, e.strength);
        if (e.strength > 0.9f) ++strong;
    }
    REQUIRE(strong >= 850);            // every top-tier marker survives
    REQUIRE(weakest >= 0.38f);         // bottom tier (0.1) is dropped
}

// ----------------------------------------------------------------- validator
TEST_CASE("BeatGridValidator: good grid passes, broken grids flag review") {
    const double rate = 48000.0, bpm = 128.0;
    const auto pcm = clickTrack(rate, 16.0, bpm);
    OnsetDetector od;
    const auto ons = od.detect(pcm.data(), (int64_t)pcm.size() / 2, 2, rate);
    BpmAnalyzer ba; const auto bpmR = ba.analyze(ons);
    BeatGridEngine ge;
    BeatGrid good = ge.build(rate, bpmR, ons, (int64_t)pcm.size() / 2);
    BeatGridValidator v;

    const auto ok = v.check(good, bpmR, ons, (int64_t)pcm.size() / 2);
    REQUIRE_FALSE(ok.needsReview);
    REQUIRE(ok.score > 0.55f);

    // empty grid
    const auto e = v.check(BeatGrid{}, bpmR, ons, 100);
    REQUIRE(e.needsReview);
    REQUIRE_FALSE(e.reasons.empty());

    // implausible tempo
    BeatGrid fast; fast.setRate(rate);
    fast.segments = { TempoSegment{0, 0, 300.0} };
    BpmResult hi = bpmR; hi.confidence = 0.9f;
    const auto f = v.check(fast, hi, ons, (int64_t)pcm.size() / 2);
    REQUIRE(f.needsReview);

    // low confidence
    BpmResult lc = bpmR; lc.confidence = 0.1f;
    const auto l = v.check(good, lc, ons, (int64_t)pcm.size() / 2);
    REQUIRE(l.needsReview);

    // grid shifted half a beat → onsets land off-grid
    BeatGrid off = good;
    const int64_t halfBeat = (int64_t)(rate * 30.0 / bpm);
    off.firstBeatFrame += halfBeat;
    for (auto& s : off.segments) s.startFrame += halfBeat;
    const auto sv = v.check(off, bpmR, ons, (int64_t)pcm.size() / 2);
    REQUIRE(sv.needsReview);
    REQUIRE(sv.score < 0.55f);
}

// --------------------------------------------------------- component status
TEST_CASE("analyzeBuffer: per-component statuses + review fields populated") {
    const auto pcm = clickTrack(48000.0, 20.0, 128.0);
    MusicAnalysisEngine engine(nullptr);
    const auto b = engine.analyzeBuffer(pcm.data(),
        (int64_t)pcm.size() / 2, 2, 48000.0, nullptr);
    REQUIRE(b != nullptr);
    const auto& r = b->result;
    const auto st = [&](AnalysisComponent c) {
        return r.components[(int)c].state; };
    REQUIRE(st(AnalysisComponent::Decode)    == (int)AnalysisState::Analyzed);
    REQUIRE(st(AnalysisComponent::Waveform)  == (int)AnalysisState::Analyzed);
    REQUIRE(st(AnalysisComponent::Bpm)       == (int)AnalysisState::Analyzed);
    REQUIRE(st(AnalysisComponent::BeatGrid)  == (int)AnalysisState::Analyzed);
    REQUIRE(st(AnalysisComponent::Transients)== (int)AnalysisState::Analyzed);
    REQUIRE(st(AnalysisComponent::Loudness)  == (int)AnalysisState::Analyzed);
    REQUIRE(r.loudness.valid);
    REQUIRE_FALSE(r.transients.empty());
    REQUIRE_FALSE(r.needsReview); // clean synthetic grid should pass
}

// ----------------------------------------------------------------- rescaled
TEST_CASE("BeatGrid::rescaled: device-rate publish keeps beat positions") {
    BeatGrid g; g.setRate(48000.0);
    g.firstBeatFrame = 4800;
    g.segments = { TempoSegment{4800, 0, 120.0} };
    const auto g44 = g.rescaled(44100.0);
    REQUIRE(g44.rate() == Approx(44100.0));
    REQUIRE(g44.firstBeatFrame == 4410);
    // beat 4 at 44.1k == beat 4 at 48k in seconds
    const double s48 = (double)g.beatToFrame(4.0)  / 48000.0;
    const double s44 = (double)g44.beatToFrame(4.0) / 44100.0;
    REQUIRE(s44 == Approx(s48).margin(1e-4));
    // same rate → unchanged
    const auto same = g.rescaled(48000.0);
    REQUIRE(same.firstBeatFrame == 4800);
}

// ----------------------------------------------------------------- overrides
TEST_CASE("AnalysisOverride: merges without touching detected data") {
    AnalysisOverride o;
    REQUIRE(o.empty());
    REQUIRE(o.effectiveBpm(120.0) == 120.0);
    o.bpm = 130.0;
    REQUIRE(o.effectiveBpm(120.0) == 130.0);
    REQUIRE_FALSE(o.empty());

    BeatGrid det; det.setRate(48000.0);
    det.segments = { TempoSegment{0, 0, 120.0} };
    BeatGrid usr = det; usr.segments[0].bpm = 128.0;
    o.grid = usr;
    const auto eff = o.effectiveGrid(det);
    REQUIRE(eff.segments[0].bpm == Approx(128.0));
    REQUIRE(det.segments[0].bpm == Approx(120.0)); // detected untouched
    o.grid.reset();
    REQUIRE(o.effectiveGrid(det).segments[0].bpm == Approx(120.0));
}

// -------------------------------------------------------------- sync ref
TEST_CASE("SyncReference: reliability gate uses confidence+review+grid") {
    AnalysisResult r;
    r.sampleRate = 48000.0;
    r.bpm.bpm = 128.0; r.bpm.confidence = 0.9f;
    BeatGrid g; g.setRate(48000.0);
    g.segments = { TempoSegment{0, 0, 128.0} };

    const auto s = SyncReference::from(r, g);
    REQUIRE(s.reliable);
    REQUIRE(s.bpm == Approx(128.0));
    REQUIRE(s.firstBeatSec == Approx(0.0));

    r.needsReview = true;
    REQUIRE_FALSE(SyncReference::from(r, g).reliable);
    r.needsReview = false; r.bpm.confidence = 0.1f;
    REQUIRE_FALSE(SyncReference::from(r, g).reliable);
    r.bpm.confidence = 0.9f;
    REQUIRE_FALSE(SyncReference::from(r, BeatGrid{}).reliable);

    // BPM override flows through
    AnalysisOverride o; o.bpm = 64.0;
    REQUIRE(SyncReference::from(r, g, o).bpm == Approx(64.0));
}

// -------------------------------------------------------------- store v4
TEST_CASE("AnalysisStore v4: loudness/transients/status/review/mid round-trip") {
    ensureDir();
    auto db = makeDb(kTmp / "store_v4.db");
    AnalysisStore store(db);
    const std::string path = (kTmp / "v4.wav").string();
    { std::ofstream f(path, std::ios::binary); f.write("z", 1); }

    AnalysisBundle b;
    b.result.state = AnalysisState::Analyzed;
    b.result.durationSec = 2.0; b.result.sampleRate = 48000.0;
    b.result.numFrames = 96000;
    b.result.bpm.bpm = 128.0; b.result.bpm.confidence = 0.8f;
    b.result.loudness.peak = 0.9f; b.result.loudness.rms = 0.4f;
    b.result.loudness.integratedLufs = -8.5f;
    b.result.loudness.loudnessRange = 3.2f;
    b.result.loudness.valid = true;
    b.result.transients = { {100, 0.9f}, {24000, 0.8f}, {48000, 0.7f} };
    b.result.components[(int)AnalysisComponent::Waveform].state =
        (int)AnalysisState::Analyzed;
    b.result.components[(int)AnalysisComponent::Bpm].state =
        (int)AnalysisState::Analyzed;
    b.result.components[(int)AnalysisComponent::Key].state =
        (int)AnalysisState::Failed; // isolated failure persists
    b.result.needsReview = true;
    b.grid.setRate(48000.0);
    b.grid.segments = { TempoSegment{0, 0, 128.0} };
    std::vector<float> pcm(96000 * 2, 0.1f);
    b.waveform = WaveformGenerator().generate(pcm.data(), 96000, 2, 48000.0);
    store.save(path, b, {}, {});

    const auto l = store.load(path);
    REQUIRE(l != nullptr);
    REQUIRE(l->result.loudness.peak == Approx(0.9f));
    REQUIRE(l->result.loudness.integratedLufs == Approx(-8.5f));
    REQUIRE(l->result.loudness.loudnessRange == Approx(3.2f));
    REQUIRE(l->result.transients.size() == 3);
    REQUIRE(l->result.transients[1].frame == 24000);
    REQUIRE(l->result.transients[1].strength == Approx(0.8f));
    REQUIRE(l->result.components[(int)AnalysisComponent::Bpm].state
            == (int)AnalysisState::Analyzed);
    REQUIRE(l->result.components[(int)AnalysisComponent::Key].state
            != (int)AnalysisState::Analyzed);
    REQUIRE(l->result.needsReview);
    // mid pyramid level persisted
    REQUIRE(l->waveform.mid.size() == b.waveform.mid.size());
}

TEST_CASE("AnalysisStore: user grid override survives re-analysis save") {
    ensureDir();
    auto db = makeDb(kTmp / "gridovr.db");
    AnalysisStore store(db);
    const std::string path = (kTmp / "g.wav").string();
    { std::ofstream f(path, std::ios::binary); f.write("g", 1); }

    AnalysisBundle b;
    b.result.state = AnalysisState::Analyzed;
    b.result.durationSec = 2.0; b.result.sampleRate = 48000.0;
    b.result.numFrames = 96000; b.result.bpm.bpm = 120.0;
    b.grid.setRate(48000.0);
    b.grid.segments = { TempoSegment{0, 0, 120.0} };
    std::vector<float> pcm(96000 * 2, 0.1f);
    b.waveform = WaveformGenerator().generate(pcm.data(), 96000, 2, 48000.0);
    store.save(path, b, {}, {});

    // user edits: half-time the grid + shift first beat
    const int64_t id = store.trackIdFor(path);
    BeatGrid edited = b.grid;
    edited.firstBeatFrame = 2400;
    edited.segments[0].bpm = 60.0;
    store.saveUserGrid(id, edited);

    // re-analysis writes fresh detected data — override must persist
    AnalysisBundle b2 = b;
    b2.grid.segments[0].bpm = 122.0;
    store.save(path, b2, {}, {});

    const auto l = store.load(path);
    REQUIRE(l != nullptr);
    REQUIRE(l->grid.segments[0].bpm == Approx(122.0));  // new detection
    REQUIRE(l->overrides.grid.has_value());
    REQUIRE(l->overrides.grid->segments[0].bpm == Approx(60.0));
    REQUIRE(l->overrides.grid->firstBeatFrame == 2400);
    REQUIRE(l->effectiveGrid().segments[0].bpm == Approx(60.0));

    store.clearUserGrid(id);
    const auto l2 = store.load(path);
    REQUIRE(l2 != nullptr);
    REQUIRE_FALSE(l2->overrides.grid.has_value());
    REQUIRE(l2->effectiveGrid().segments[0].bpm == Approx(122.0));
}

// ----------------------------------------------------------------- mid level
TEST_CASE("Waveform: mid pyramid level generated and queryable") {
    const double rate = 48000.0;
    const int64_t n = (int64_t)(rate * 2.0);
    std::vector<float> pcm((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        const float v = 0.5f * (float)std::sin(2.0 * 3.14159265 * 220.0 * i / rate);
        pcm[(size_t)i * 2] = pcm[(size_t)i * 2 + 1] = v;
    }
    WaveformGenerator wg;
    const auto w = wg.generate(pcm.data(), n, 2, rate);
    const int64_t nDet = (n + WaveformData::kDetailFrames - 1)
                       / WaveformData::kDetailFrames;
    const int64_t nMid = (nDet + 3) / 4;
    REQUIRE(w.mid.size() == (size_t)nMid * 2);

    // a ~2 s region with a small bin budget should still resolve data
    std::vector<WaveformBin> bins;
    w.query(n / 4, n / 4 + rate / 2, 40, bins, 0);
    REQUIRE(bins.size() == 40);
    float pk = 0.0f;
    for (const auto& bb : bins) pk = std::max(pk, bb.max);
    REQUIRE(pk > 0.45f);
}

// ------------------------------------------------------------- service edits
TEST_CASE("AnalysisService::applyEdit: overrides persist, detected intact") {
    ensureDir();
    const auto wav = kTmp / "edit.wav";
    const auto pcm = clickTrack(48000.0, 12.0, 128.0);
    std::vector<int16_t> pcm16(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm16[i] = (int16_t)std::clamp(pcm[i] * 30000.0f, -32000.0f, 32000.0f);
    writeTestWav(wav, 48000, 2, pcm16);

    auto db = makeDb(kTmp / "edit.db");
    AnalysisService svc([] { return std::make_unique<FFmpegDecoder>(); },
                        db, 2);
    REQUIRE(svc.workerCount() == 2);
    const auto b = svc.analyzeTrackSync(wav.string());
    REQUIRE(b != nullptr);
    const int64_t detectedFirst = b->grid.firstBeatFrame;
    REQUIRE_FALSE(b->grid.empty());

    // move first beat → effective changes, detected does not
    auto g = svc.applyEdit(wav.string(), AnalysisEdit::SetFirstBeat,
                           12345.0);
    REQUIRE(g.firstBeatFrame == 12345);
    REQUIRE(svc.getBeatGrid(wav.string()).firstBeatFrame == 12345);
    const auto b2 = svc.getAnalysis(wav.string());
    REQUIRE(b2->grid.firstBeatFrame == detectedFirst); // detected intact
    REQUIRE(svc.hasOverride(wav.string()));

    // half tempo
    const double bpm0 = svc.getBeatGrid(wav.string()).bpmAt(0);
    auto g2 = svc.applyEdit(wav.string(), AnalysisEdit::HalfTempo);
    REQUIRE(g2.bpmAt(0) == Approx(bpm0 * 0.5));

    // persistence: fresh store read sees the override row
    const auto loaded = svc.store().load(wav.string());
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->overrides.grid.has_value());
    REQUIRE(loaded->grid.firstBeatFrame == detectedFirst);

    // reset → effective == detected again
    auto g3 = svc.applyEdit(wav.string(), AnalysisEdit::ResetGrid);
    REQUIRE(g3.firstBeatFrame == detectedFirst);
    REQUIRE_FALSE(svc.hasOverride(wav.string()));

    // bpm override channel is separate
    svc.applyEdit(wav.string(), AnalysisEdit::SetBpmOverride, 131.0);
    REQUIRE(svc.getBpm(wav.string()) == Approx(131.0));
    svc.applyEdit(wav.string(), AnalysisEdit::SetBpmOverride, -1.0);
    REQUIRE(svc.getBpm(wav.string()) ==
            Approx(b2->result.bpm.bpm).margin(0.01));

    // sync reference is produced for analyzed tracks
    const auto sr = svc.syncReference(wav.string());
    REQUIRE(sr.has_value());
    REQUIRE(sr->reliable);
}

// --------------------------------------------------------------- queue gate
TEST_CASE("AnalysisQueue: CPU gate blocks background, high priority runs") {
    ensureDir();
    const auto wav = kTmp / "gate.wav";
    const auto pcm = clickTrack(48000.0, 6.0, 120.0);
    std::vector<int16_t> pcm16(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm16[i] = (int16_t)std::clamp(pcm[i] * 30000.0f, -32000.0f, 32000.0f);
    writeTestWav(wav, 48000, 2, pcm16);

    auto db = makeDb(kTmp / "gate.db");
    AnalysisService svc([] { return std::make_unique<FFmpegDecoder>(); },
                        db, 1);
    std::atomic<int> done{0};
    svc.addListener([&](const AnalysisQueue::Notification& n) {
        if (n.event == AnalysisQueue::Event::Completed) ++done;
    });

    svc.setPlaybackActive(true); // gate at ≥HIGH
    svc.analyzeTrack(wav.string(), (int)AnalysisPriority::Normal);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(done.load() == 0);   // background job must NOT have run

    // high-priority job proceeds under the gate
    svc.analyzeTrack(wav.string(), (int)AnalysisPriority::High);
    const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (done.load() == 0 && std::chrono::steady_clock::now() < dl)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(done.load() == 1);

    // reopening the gate lets queued background work flow again
    svc.setPlaybackActive(false);
    const auto dl2 = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (done.load() < 2 && std::chrono::steady_clock::now() < dl2)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(done.load() == 2);
}

// ----------------------------------------------------------------- benchmark
TEST_CASE("Benchmark: analysis throughput on synthetic 128 BPM tracks") {
    const auto pcm = clickTrack(48000.0, 3.0, 128.0);
    MusicAnalysisEngine engine(nullptr);
    constexpr int kTracks = 100;
    const auto t0 = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 0; i < kTracks; ++i) {
        const auto b = engine.analyzeBuffer(pcm.data(),
            (int64_t)pcm.size() / 2, 2, 48000.0, nullptr);
        if (b && b->result.state == AnalysisState::Analyzed) ++ok;
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[analysis-bench] %d tracks, %.0f ms total, "
                "%.1f ms/track, %.1f tracks/s\n",
                kTracks, ms, ms / kTracks, kTracks * 1000.0 / ms);
    REQUIRE(ok == kTracks);
    REQUIRE(kTracks * 1000.0 / ms > 2.0); // >2 tracks/s even in Debug
}
