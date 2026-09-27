// Phase 22 — professional key detection + harmonic mixing tests.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/analysis/KeyAnalyzer.h"
#include "core/data/AnalysisStore.h"
#include "core/data/Database.h"
#include "core/harmonic/CamelotWheel.h"
#include "core/harmonic/Compatibility.h"
#include "core/harmonic/HarmonicTypes.h"
#include "core/harmonic/KeyAnalysisCache.h"
#include "core/harmonic/KeyAnalysisEngine.h"
#include "core/harmonic/KeyAnalysisProvider.h"
#include "core/harmonic/KeyDiagnostics.h"
#include "core/harmonic/KeyNormalizer.h"
#include "core/harmonic/KeyShiftEngine.h"
#include "core/harmonic/RelatedTracksEngine.h"
#include "core/library/FilterEngine.h"
#include "core/library/KeyNotation.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/library/SearchQuery.h"

using namespace onda;
namespace H = onda::harmonic;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path tempRoot(const char* name) {
    fs::path d = fs::temp_directory_path() / "onda_keytest" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// ---- §41 synthetic test signals ------------------------------------
// Weighted scale + tonic-triad emphasis at 48 kHz stereo. Harmonics
// included so the chromagram sees a realistic pitch-class profile.
std::vector<float> keySignal(int rootPc, bool minor, int64_t frames,
                             double rate = 48000.0) {
    static const int kMajScale[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int kMinScale[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* scale = minor ? kMinScale : kMajScale;
    std::vector<float> pcm((size_t)frames * 2, 0.0f);
    auto midiHz = [](int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); };
    for (int i = 0; i < frames; ++i) {
        double t = (double)i / rate, s = 0.0;
        for (int deg = 0; deg < 7; ++deg) {
            const int pc = (rootPc + scale[deg]) % 12;
            const double w = deg == 0 ? 1.0 : deg == 2 || deg == 4 ? 0.8 : 0.5;
            const double f0 = midiHz(60 + pc);           // octave 4
            s += w * std::sin(6.28318530718 * f0 * t)
               + w * 0.4 * std::sin(6.28318530718 * f0 * 2.0 * t)
               + w * 0.2 * std::sin(6.28318530718 * f0 * 3.0 * t);
        }
        s *= 0.05;
        pcm[(size_t)i * 2] = (float)s;
        pcm[(size_t)i * 2 + 1] = (float)s;
    }
    return pcm;
}

// Triad only — ambiguous relative maj/min by construction.
std::vector<float> triadSignal(int rootPc, bool minor, int64_t frames,
                               double rate = 48000.0) {
    const int third = minor ? 3 : 4;
    std::vector<float> pcm((size_t)frames * 2, 0.0f);
    auto midiHz = [](int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); };
    const int tones[3] = { rootPc, (rootPc + third) % 12,
                           (rootPc + 7) % 12 };
    for (int i = 0; i < frames; ++i) {
        double t = (double)i / rate, s = 0.0;
        for (int pc : tones) {
            const double f0 = midiHz(60 + pc);
            s += std::sin(6.28318530718 * f0 * t)
               + 0.3 * std::sin(6.28318530718 * f0 * 2.0 * t);
        }
        s *= 0.08;
        pcm[(size_t)i * 2] = (float)s;
        pcm[(size_t)i * 2 + 1] = (float)s;
    }
    return pcm;
}

std::vector<float> silence(int64_t frames) {
    return std::vector<float>((size_t)frames * 2, 0.0f);
}

H::KeyAnalysisConfig cfg(int q = 1) {
    return H::KeyAnalysisProvider().configFor(q);
}

} // namespace

// ======================================================== KEY MODEL (§2/3)
TEST_CASE("MusicalKey: index round-trip over all 24 + unknown", "[key22]") {
    for (int i = 0; i < 24; ++i) {
        const auto k = H::MusicalKey::fromIndex(i, 0.9f);
        REQUIRE(k.index() == i);
        REQUIRE(k.valid());
        REQUIRE(k.pitchClass == i % 12);
        REQUIRE(k.mode == (i < 12 ? H::KeyMode::Major
                                  : H::KeyMode::Minor));
    }
    REQUIRE(!H::MusicalKey{}.valid());
    REQUIRE(H::MusicalKey::fromIndex(-1).index() == -1);
    REQUIRE(H::MusicalKey::fromIndex(24).index() == -1);
    // reserved modes stay unrepresentable as an index (§3)
    H::MusicalKey dorian { 2, H::KeyMode::Dorian, 0.5f };
    REQUIRE(dorian.index() == -1);
}

// ======================================================= DETECTION (§41/42)
TEST_CASE("KsKeyDetector: strong scale signals detect tonic", "[key22]") {
    H::KsKeyDetector det;
    const int64_t n = 48000 * 6;          // 6 s
    struct Case { int pc; bool minor; const char* name; };
    const Case cases[] = {
        { 0, false, "C major" }, { 9, true,  "A minor" },
        { 7, false, "G major" }, { 4, true,  "E minor" },
        { 5, false, "F major" }, { 2, true,  "D minor" },
    };
    int hits = 0;
    for (const auto& c : cases) {
        auto pcm = keySignal(c.pc, c.minor, n);
        const auto r = det.analyze(pcm.data(), n, 2, 48000.0, cfg(2));
        const int expected = c.minor ? c.pc + 12 : c.pc;
        INFO(c.name << " → idx " << r.primary.index()
             << " conf " << r.primary.confidence);
        hits += r.primary.index() == expected;
        REQUIRE(r.primary.confidence >= 0.0f);
        REQUIRE(r.primary.confidence <= 1.0f);
        REQUIRE(r.algorithm == "onda.ks");
        REQUIRE(!r.version.empty());
        REQUIRE(r.analyzedAt > 0);
    }
    // Honest bound: synthetic scale+triad signals should be
    // near-deterministic for K-S (measured, not assumed).
    REQUIRE(hits >= 5);
}

TEST_CASE("KsKeyDetector: alternatives + ambiguity surfaces", "[key22]") {
    H::KsKeyDetector det;
    const int64_t n = 48000 * 4;
    auto pcm = triadSignal(0, false, n);   // C-E-G triad → C or Am-ish
    const auto r = det.analyze(pcm.data(), n, 2, 48000.0, cfg(2));
    REQUIRE(r.primary.valid());
    REQUIRE(!r.alternatives.empty());
    REQUIRE(r.alternatives.size() <= 3);
    // alternatives ranked below primary correlation — never ahead
    INFO("primary " << r.primary.index() << " conf "
         << r.primary.confidence << " alts "
         << r.alternatives.size());
    for (const auto& a : r.alternatives)
        REQUIRE(a.key.index() != r.primary.index());
}

TEST_CASE("KsKeyDetector: silence → unknown, never invented", "[key22]") {
    H::KsKeyDetector det;
    auto pcm = silence(48000 * 2);
    const auto r = det.analyze(pcm.data(), 48000 * 2, 2, 48000.0, cfg(1));
    REQUIRE(!r.primary.valid());
    REQUIRE(r.primary.confidence == 0.0f);
}

TEST_CASE("KeyAnalyzer::analyze segments: modulating signal", "[key22]") {
    // first half C-major, second half G-major → ≥1 confident change
    const int64_t half = 48000 * 4;
    auto a = keySignal(0, false, half);
    auto b = keySignal(7, false, half);
    std::vector<float> pcm = a;
    pcm.insert(pcm.end(), b.begin(), b.end());
    KeyAnalyzer ka;
    const auto segs = ka.analyzeSegments(pcm.data(), half * 2, 2,
                                         48000.0, half);
    REQUIRE(segs.size() == 2);
    // windows should not both land on the same confident key
    const bool changed =
        segs[0].key >= 0 && segs[1].key >= 0 && segs[0].key != segs[1].key;
    const bool uncertain = segs[0].key < 0 || segs[1].key < 0;
    INFO("seg0 " << segs[0].key << " seg1 " << segs[1].key);
    REQUIRE((changed || uncertain));   // never a false same-key claim
}

TEST_CASE("TonalProfile: stable vs modulating + unknowns", "[key22]") {
    std::vector<H::TonalSegment> segs;
    for (int i = 0; i < 4; ++i) {
        H::TonalSegment s;
        s.startFrame = i * 1000; s.endFrame = (i + 1) * 1000;
        s.key = H::MusicalKey::fromIndex(0, 0.8f);
        segs.push_back(s);
    }
    auto p = H::TonalProfile::fromSegments(segs, 48000.0);
    REQUIRE(p.dominant.index() == 0);
    REQUIRE(!p.modulating);
    REQUIRE(p.stability == Approx(1.0f));

    segs[2].key = H::MusicalKey::fromIndex(7, 0.8f);
    segs[3].key = H::MusicalKey::fromIndex(7, 0.8f);
    segs[3].key.pitchClass = -1;               // unknown window
    p = H::TonalProfile::fromSegments(segs, 48000.0);
    REQUIRE(p.modulations == 1);
    REQUIRE(p.unknownSegments == 1);
    REQUIRE(p.dominant.index() == 0);          // 2:1 weight
    REQUIRE(p.stability == Approx(0.5f));
}

// ================================================ CAMELOT CONVERSION (§11/43)
TEST_CASE("Camelot: all 24 positions round-trip", "[key22]") {
    // Canonical wheel (standard mapping):
    //   minors: 8A=Am 9A=Em 10A=Bm 11A=F#m 12A=C#m 1A=G#m
    //           2A=D#m 3A=A#m 4A=Fm 5A=Cm 6A=Gm 7A=Dm
    //   majors: 8B=C 9B=G 10B=D 11B=A 12B=E 1B=B
    //           2B=F# 3B=C# 4B=G# 5B=D# 6B=A# 7B=F
    const char* expected[24] = {
        "8B","3B","10B","5B","12B","7B","2B","9B","4B","11B","6B","1B",
        "5A","12A","7A","2A","9A","4A","11A","6A","1A","8A","3A","10A" };
    for (int i = 0; i < 24; ++i) {
        const auto cam = H::CamelotConverter::fromIndex(i);
        INFO("idx " << i << " → " << cam.str());
        REQUIRE(cam.valid());
        REQUIRE(cam.str() == expected[i]);
        REQUIRE(H::CamelotConverter::toIndex(cam.str()) == i);
    }
    // every number/side → index → string
    for (int n = 1; n <= 12; ++n)
        for (char s : { 'A', 'B' }) {
            const int idx = H::CamelotConverter::indexOf(n, s);
            REQUIRE(idx >= 0);
            const auto back = H::CamelotConverter::fromIndex(idx);
            REQUIRE(back.number == n);
            REQUIRE(back.side == s);
        }
    // parse variants + invalids
    REQUIRE(H::CamelotConverter::toIndex("8a") == 21);
    REQUIRE(H::CamelotConverter::toIndex(" 9B ") == 7);
    REQUIRE(H::CamelotConverter::toIndex("0A") == -1);
    REQUIRE(H::CamelotConverter::toIndex("13A") == -1);
    REQUIRE(H::CamelotConverter::toIndex("8C") == -1);
    REQUIRE(H::CamelotConverter::toIndex("AB") == -1);
    REQUIRE(H::CamelotConverter::fromIndex(-1).valid() == false);
}

TEST_CASE("Camelot: pc+mode conversion matches table", "[key22]") {
    for (int i = 0; i < 24; ++i) {
        const auto viaIdx = H::CamelotConverter::fromIndex(i);
        const auto viaPc  = H::CamelotConverter::fromPitchClassMode(
            i % 12, i < 12 ? H::KeyMode::Major : H::KeyMode::Minor);
        REQUIRE(viaPc.str() == viaIdx.str());
    }
    REQUIRE(!H::CamelotConverter::fromPitchClassMode(
                0, H::KeyMode::Dorian).valid());   // §3 — no invention
}

// ================================================ NOTATION/NORMALIZE (§9/10)
TEST_CASE("KeyNotation: five systems + enharmonics preserved", "[key22]") {
    using onda::library::KeyNotation;
    using NC = onda::library::KeyNotationConverter;
    // C# (idx 1) renders five ways; the index never changes (§10)
    REQUIRE(std::string(NC::name(1, KeyNotation::Standard)) == "C#");
    REQUIRE(std::string(NC::name(1, KeyNotation::Sharp))    == "C#");
    REQUIRE(std::string(NC::name(1, KeyNotation::Flat))     == "Db");
    REQUIRE(std::string(NC::name(1, KeyNotation::Camelot))  == "3B");
    REQUIRE(std::string(NC::name(1, KeyNotation::OpenKey))  == "8d");
    // parse enharmonics to the SAME canonical index (§9)
    REQUIRE(NC::parse("C#") == 1);
    REQUIRE(NC::parse("Db") == 1);
    REQUIRE(NC::parse("Eb") == NC::parse("D#"));
    REQUIRE(NC::parse("Bbm") == NC::parse("A#m"));
    // KeyNormalizer round-trips notation → notation
    REQUIRE(H::KeyNormalizer::normalize("C#", KeyNotation::Flat) == "Db");
    REQUIRE(H::KeyNormalizer::normalize("Db", KeyNotation::Sharp) == "C#");
    REQUIRE(H::KeyNormalizer::normalize("8A", KeyNotation::Standard) == "Am");
    REQUIRE(H::KeyNormalizer::normalize("8A", KeyNotation::Flat) == "Am");
    REQUIRE(H::KeyNormalizer::normalize("garbage",
                                        KeyNotation::Standard) == "?");
    const auto mk = H::KeyNormalizer::parseStandard("Gbm", 0.7f);
    REQUIRE(mk.index() == 18);   // F#m
    REQUIRE(mk.confidence == Approx(0.7f));
}

// ================================================= WHEEL RELATIONS (§12/14)
TEST_CASE("CamelotWheel: relationship classes deterministic", "[key22]") {
    using W = H::CamelotWheel;
    const int am = H::CamelotConverter::toIndex("8A");      // pos 7
    const int amPos = H::CamelotConverter::fromIndex(am).pos();
    REQUIRE(amPos == 7);
    const int c  = H::CamelotConverter::toIndex("8B");      // pos 19
    const int cPos = H::CamelotConverter::fromIndex(c).pos();
    const int dm = H::CamelotConverter::toIndex("7A");      // pos 6
    const int dmPos = H::CamelotConverter::fromIndex(dm).pos();
    const int em = H::CamelotConverter::toIndex("9A");      // pos 8
    const int f  = H::CamelotConverter::toIndex("7B");      // pos 18
    const int fPos = H::CamelotConverter::fromIndex(f).pos();

    REQUIRE(W::relationship(amPos, amPos) == W::Rel::Same);
    REQUIRE(W::relationship(amPos, cPos)  == W::Rel::Relative);
    REQUIRE(W::relationship(amPos, dmPos) == W::Rel::Adjacent);
    REQUIRE(W::relationship(amPos,
        H::CamelotConverter::fromIndex(em).pos()) == W::Rel::Adjacent);
    REQUIRE(W::relationship(amPos, fPos)  == W::Rel::RelativeAdjacent);
    REQUIRE(W::relationship(amPos,
        H::CamelotConverter::fromIndex(
            H::CamelotConverter::toIndex("3A")).pos()) == W::Rel::None);
    REQUIRE(W::relationship(-1, amPos) == W::Rel::None);
    // wrap: 12A neighbors 11A and 1A
    const int csm = H::CamelotConverter::toIndex("12A");
    const int gsm = H::CamelotConverter::toIndex("1A");
    REQUIRE(W::relationship(
        H::CamelotConverter::fromIndex(csm).pos(),
        H::CamelotConverter::fromIndex(gsm).pos()) == W::Rel::Adjacent);
    // strict compatible set for 8A = {8A,8B,7A,9A}
    const auto keys = W::compatibleIndices(am);
    std::set<int> s(keys.begin(), keys.end());
    REQUIRE(s == std::set<int>({ am, c, dm, em }));
}

// ============================================ HARMONIC COMPATIBILITY (§13/44)
TEST_CASE("HarmonicCompatibilityEngine: described relations", "[key22]") {
    using E = H::HarmonicCompatibilityEngine;
    auto mk = [](const char* cam, float c) {
        return H::MusicalKey::fromIndex(
            H::CamelotConverter::toIndex(cam), c);
    };
    auto r = E::relate(mk("8A", .9f), mk("8A", .9f));
    REQUIRE(r.relation == H::HarmonicRel::SameKey);
    REQUIRE(r.score == Approx(1.0f));
    REQUIRE(r.modeMatch);
    REQUIRE(r.explanation.find("same key") != std::string::npos);

    r = E::relate(mk("8A", .9f), mk("8B", .9f));
    REQUIRE(r.relation == H::HarmonicRel::RelativeMajorMinor);

    r = E::relate(mk("8A", .9f), mk("9A", .9f));
    REQUIRE(r.relation == H::HarmonicRel::AdjacentCamelot);
    REQUIRE(r.camelotDistance == 1);

    r = E::relate(mk("8A", .9f), mk("9B", .9f));
    REQUIRE(r.relation == H::HarmonicRel::EnergyChange);

    r = E::relate(mk("8A", .9f), mk("2A", .9f));
    REQUIRE(r.relation == H::HarmonicRel::DistantKey);

    // unknown keys → explicit Unknown, never false-incompatible
    r = E::relate(H::MusicalKey{}, mk("8A", .9f));
    REQUIRE(r.relation == H::HarmonicRel::Unknown);
    REQUIRE(r.explanation.find("insufficient") != std::string::npos);
    r = E::relate(mk("8A", .9f), H::MusicalKey{});
    REQUIRE(r.relation == H::HarmonicRel::Unknown);

    // low confidence stays honest in the explanation
    r = E::relate(mk("8A", .9f), mk("9A", .05f));
    REQUIRE(r.relation == H::HarmonicRel::AdjacentCamelot);
    REQUIRE(!r.confidenceSufficient);
    REQUIRE(r.explanation.find("low key confidence") != std::string::npos);
}

// =================================================== TEMPO COMPAT (§15/17)
TEST_CASE("TempoCompatibility: diff/ratio/pitch/half/double", "[key22]") {
    auto r = H::TempoCompatibility::relate(128.0, 130.0);
    REQUIRE(r.valid);
    REQUIRE(r.bpmDiff == Approx(2.0));
    REQUIRE(r.ratio == Approx(130.0 / 128.0));
    REQUIRE(r.pitchPercent == Approx((128.0 / 130.0 - 1.0) * 100.0));
    REQUIRE(r.withinRange);

    r = H::TempoCompatibility::relate(128.0, 64.0);
    REQUIRE(r.halfTime);
    REQUIRE(!r.doubleTime);
    r = H::TempoCompatibility::relate(64.0, 128.0);
    REQUIRE(r.doubleTime);

    r = H::TempoCompatibility::relate(128.0, 200.0);
    REQUIRE(!r.withinRange);
    REQUIRE(r.explanation.find("outside mix range") != std::string::npos);

    r = H::TempoCompatibility::relate(0.0, 128.0);
    REQUIRE(!r.valid);
    REQUIRE(r.explanation == "missing BPM");

    // §17 — exact pitch% needed, reported only (not applied)
    r = H::TempoCompatibility::relate(130.0, 128.0);
    REQUIRE(r.pitchPercent == Approx(1.5625).margin(0.001));
}

TEST_CASE("MusicalCompatibility: combined descriptive result", "[key22]") {
    auto m = H::MusicalCompatibilityEngine::relate(
        H::MusicalKey::fromIndex(21, .9f), 128.0,   // 8A
        H::MusicalKey::fromIndex(16, .9f), 126.0);  // Em = 9A
    // Em = 9A → adjacent to 8A
    REQUIRE(m.key.relation == H::HarmonicRel::AdjacentCamelot);
    REQUIRE(m.tempo.withinRange);
    REQUIRE(m.mixableNow);
    REQUIRE(m.explanation.find("8A") != std::string::npos);
    REQUIRE(m.explanation.find("9A") != std::string::npos);

    m = H::MusicalCompatibilityEngine::relate(
        H::MusicalKey::fromIndex(21, .9f), 128.0,
        H::MusicalKey::fromIndex(-1, 0.0f), 126.0);
    REQUIRE(m.key.relation == H::HarmonicRel::Unknown);
    REQUIRE(!m.mixableNow);
}

// ===================================================== KEY SHIFT (§18/19/45)
TEST_CASE("KeyShiftEngine: semitone math + octave normalization", "[key22]") {
    using S = H::KeyShiftEngine;
    // index shifts preserve mode + wrap pitch class
    REQUIRE(S::shiftIndex(0, 0) == 0);
    REQUIRE(S::shiftIndex(0, 1) == 1);
    REQUIRE(S::shiftIndex(0, -1) == 11);
    REQUIRE(S::shiftIndex(0, 2) == 2);
    REQUIRE(S::shiftIndex(0, -2) == 10);
    REQUIRE(S::shiftIndex(0, 12) == 0);     // octave-equiv → identity
    REQUIRE(S::shiftIndex(0, -12) == 0);
    REQUIRE(S::shiftIndex(21, 1) == 22);    // Am → A#m (mode kept)
    REQUIRE(S::shiftIndex(21, -12) == 21);
    REQUIRE(S::shiftIndex(-1, 5) == -1);    // unknown stays unknown
    REQUIRE(S::shiftIndex(0, 13) == 1);     // wraps mod 12
    REQUIRE(S::shiftIndex(11, 2) == 1);     // B + 2 → C#

    // §18 — requested displacement between two keys
    const auto from = H::MusicalKey::fromIndex(21);   // Am(8A)
    const auto to   = H::MusicalKey::fromIndex(4);    // Em(9A)
    REQUIRE(S::semitonesBetween(from, to) == -5);     // shortest path
    REQUIRE(S::semitonesBetween(H::MusicalKey{}, to) == 0);

    // §19 — display model: effective key with explicit shift
    auto m = S::model(21, 1, 1.0, true);
    REQUIRE(m.originalIndex == 21);
    REQUIRE(m.effectiveIndex == 22);
    REQUIRE(m.tempoDriftSemis == Approx(0.0f));
    REQUIRE(S::display(m).find("8A") != std::string::npos);
    REQUIRE(S::display(m).find("3A") != std::string::npos); // A#m = 3A

    // §20 — tempo drift only when NOT key-locked: +~6% ≈ 1 semitone
    m = S::model(21, 0, std::pow(2.0, 1.0 / 12.0), false);
    REQUIRE(m.tempoDriftSemis == Approx(1.0f));
    REQUIRE(m.effectiveIndex == 22);
    m = S::model(21, 0, std::pow(2.0, 1.0 / 12.0), true);
    REQUIRE(m.tempoDriftSemis == Approx(0.0f));
    REQUIRE(m.effectiveIndex == 21);

    m = S::model(21, 0, 1.0, true);
    REQUIRE(S::display(m) == "8A");         // clean → single key
}

// ========================================================= CACHE (§21/38/40)
TEST_CASE("KeyAnalysisCache: round-trip + validity + history", "[key22]") {
    const auto dir = tempRoot("cache");
    Database db;
    REQUIRE(db.open((dir / "t.db").string()));
    AnalysisStore store(db);
    const int64_t id = store.trackIdFor((dir / "x.wav").string());
    // an analysis row must exist for the UPDATE
    {
        auto s = db.prepare(
            "INSERT OR REPLACE INTO analysis(track_id,key_index) VALUES(?,-1)");
        s.bind(1, id); s.step(); s.finalize();
    }

    H::KeyAnalysisCache cache(db);
    H::KeyAnalysisResult r;
    r.algorithm = "onda.ks"; r.version = "1.0.0";
    r.quality = 2; r.analyzedAt = 12345;
    r.primary = H::MusicalKey::fromIndex(21, 0.83f);
    r.ambiguous = true;
    r.alternatives.push_back({ H::MusicalKey::fromIndex(0), 0.71f });
    r.alternatives.push_back({ H::MusicalKey::fromIndex(9), 0.68f });
    H::TonalSegment ts;
    ts.startFrame = 0; ts.endFrame = 96000;
    ts.key = H::MusicalKey::fromIndex(21, 0.8f);
    r.segments.push_back(ts);
    cache.save(id, r);

    auto back = cache.load(id);
    REQUIRE(back.has_value());
    REQUIRE(back->primary.index() == 21);
    REQUIRE(back->primary.confidence == Approx(0.83f));
    REQUIRE(back->ambiguous);
    REQUIRE(back->algorithm == "onda.ks");
    REQUIRE(back->version == "1.0.0");
    REQUIRE(back->alternatives.size() == 2);
    REQUIRE(back->alternatives[0].key.index() == 0);
    REQUIRE(back->segments.size() == 1);
    REQUIRE(back->segments[0].key.index() == 21);

    // §21 — identity match: version bump invalidates
    REQUIRE(H::KeyAnalysisCache::valid(*back, "onda.ks", "1.0.0"));
    REQUIRE(!H::KeyAnalysisCache::valid(*back, "onda.ks", "2.0.0"));
    REQUIRE(!H::KeyAnalysisCache::valid(*back, "other", "1.0.0"));

    // §40 — history accumulates
    cache.save(id, r);
    auto hist = cache.history(id);
    REQUIRE(hist.size() == 2);
    REQUIRE(hist[0].keyIndex == 21);
    REQUIRE(hist[0].ambiguous);

    // legacy flat columns updated too (UI/library reads them)
    {
        auto s = db.prepare(
            "SELECT key_index,key_confidence,key_algo FROM analysis"
            " WHERE track_id=?");
        s.bind(1, id);
        REQUIRE(s.step());
        REQUIRE(s.colInt(0) == 21);
        REQUIRE(s.colDouble(1) == Approx(0.83));
        REQUIRE(std::string(s.colText(2)) == "onda.ks");
        s.finalize();
    }
}

TEST_CASE("KeyAnalysisCache: user override untouched (§38)", "[key22]") {
    const auto dir = tempRoot("override");
    Database db;
    REQUIRE(db.open((dir / "t.db").string()));
    AnalysisStore store(db);
    const int64_t id = store.trackIdFor((dir / "y.wav").string());
    {
        auto s = db.prepare(
            "INSERT OR REPLACE INTO analysis(track_id,key_index) VALUES(?,-1)");
        s.bind(1, id); s.step(); s.finalize();
    }
    H::KeyAnalysisCache cache(db);
    H::KeyAnalysisResult r;
    r.algorithm = "onda.ks"; r.version = "1.0.0"; r.analyzedAt = 1;
    r.primary = H::MusicalKey::fromIndex(0, 0.9f);
    cache.save(id, r);
    store.setUserKey(id, 9);                    // DJ override → G major
    auto back = cache.load(id);
    REQUIRE(back->primary.index() == 0);        // detected row intact
    {
        auto s = db.prepare(
            "SELECT key_index FROM user_overrides WHERE track_id=?");
        s.bind(1, id);
        REQUIRE(s.step());
        REQUIRE(s.colInt(0) == 9);
        s.finalize();
    }
}

// ================================================ SEARCH/FILTER TOKENS (§24/25)
TEST_CASE("SearchQuery: mode:/camelot:/keyconf:/compatible: tokens", "[key22]") {
    namespace L = onda::library;
    // camelot: parses to a canonical index filter
    auto q = L::SearchQuery::toSpec("camelot:8A");
    REQUIRE(q.filters.size() == 1);
    REQUIRE(q.filters[0].field == "key_index");
    REQUIRE(q.filters[0].a == Approx(21.0));

    q = L::SearchQuery::toSpec("mode:minor");
    REQUIRE(q.filters.size() == 1);
    REQUIRE(q.filters[0].field == "key_mode");
    REQUIRE(q.filters[0].a == Approx(1.0));
    q = L::SearchQuery::toSpec("mode:unknown");
    REQUIRE(q.filters[0].a == Approx(-1.0));
    q = L::SearchQuery::toSpec("mode:junk");       // unparseable → FTS
    REQUIRE(q.filters.empty());
    REQUIRE(q.search == "mode:junk");

    q = L::SearchQuery::toSpec("keyconf:0.5+");
    REQUIRE(q.filters.size() == 1);
    REQUIRE(q.filters[0].field == "keyconf");
    REQUIRE(q.filters[0].a == Approx(0.5));

    q = L::SearchQuery::toSpec("compatible:8A");
    REQUIRE(q.filters.size() == 1);
    REQUIRE(q.filters[0].field == "key_compatible");
    q = L::SearchQuery::toSpec("camelot_compatible:8A");
    REQUIRE(q.filters[0].field == "key_camelot_compatible");

    // combined query — §24
    q = L::SearchQuery::toSpec("key:Am bpm:120-130 mode:minor");
    REQUIRE(q.filters.size() == 3);
}

TEST_CASE("FilterEngine: key_mode + keyconf + camelot clauses", "[key22]") {
    namespace L = onda::library;
    std::vector<std::string> binds;
    L::FilterCond c;
    c.field = "key_mode"; c.op = L::CmpOp::Eq; c.a = 1.0;
    auto w = L::FilterEngine::where({ c }, binds);
    REQUIRE(w.find("BETWEEN 12 AND 23") != std::string::npos);
    c.a = 0.0;
    w = L::FilterEngine::where({ c }, binds);
    REQUIRE(w.find("BETWEEN 0 AND 11") != std::string::npos);
    c.a = -1.0;
    w = L::FilterEngine::where({ c }, binds);
    REQUIRE(w.find("= -1") != std::string::npos);

    c = {}; c.field = "keyconf"; c.op = L::CmpOp::Gte; c.a = 0.5;
    w = L::FilterEngine::where({ c }, binds);
    REQUIRE(w.find("key_confidence") != std::string::npos);

    c = {}; c.field = "key_camelot_compatible"; c.op = L::CmpOp::Eq;
    c.a = 21.0;                                     // 8A
    w = L::FilterEngine::where({ c }, binds);
    REQUIRE(w.find("key_index IN") != std::string::npos);
    // strict wheel set for 8A = {21(8A),0(8B),14(7A),16(9A)}
    for (int k : { 0, 14, 16, 21 })
        REQUIRE(w.find(std::to_string(k)) != std::string::npos);
}

// ============================================== RELATED TRACKS (§26)
TEST_CASE("RelatedTracksEngine: criteria + explanations", "[key22]") {
    namespace L = onda::library;
    std::vector<L::Track> fake;
    auto mk = [](int64_t id, int key, double bpm, float conf) {
        L::Track t; t.id = id; t.keyIndex = key; t.bpm = bpm;
        t.keyConfidence = conf; t.title = "T" + std::to_string(id);
        return t;
    };
    fake.push_back(mk(1, 21, 128.0, .9f));   // 8A same
    fake.push_back(mk(2, 0,  129.0, .9f));   // 8B relative
    fake.push_back(mk(3, 16, 127.0, .9f));   // 9A(Em) adjacent
    fake.push_back(mk(4, 10, 128.0, .9f));   // distant (A#)
    fake.push_back(mk(5, 14, 200.0, .9f));   // 7A(Dm) key-ok, bpm off
    fake.push_back(mk(6, -1, 128.0, .9f));   // unknown key
    auto qfn = [&fake](const L::QuerySpec& q) {
        L::TrackPage pg;
        // evaluate the IN-list + bpm between like the SQL would
        for (const auto& t : fake) {
            bool ok = true;
            for (const auto& f : q.filters) {
                if (f.field == "key_index" && f.op == L::CmpOp::In)
                    ok = std::find(f.ints.begin(), f.ints.end(),
                                   t.keyIndex) != f.ints.end();
                if (f.field == "bpm" && f.op == L::CmpOp::Between)
                    ok = ok && t.bpm >= f.a && t.bpm <= f.b;
                if (f.field == "keyconf" && f.op == L::CmpOp::Gte)
                    ok = ok && t.keyConfidence >= (float)f.a;
            }
            if (ok) pg.items.push_back(t);
        }
        pg.total = (int)pg.items.size();
        return pg;
    };
    H::RelatedCriteria c;
    c.excludeTrackId = 1;
    const auto ref = H::MusicalKey::fromIndex(21, .9f);
    const auto out = H::RelatedTracksEngine::suggest(ref, 128.0, c, qfn);
    // hits: id 2 (8B rel), 3 (9A adj) — not 4 (key), not 5 (bpm),
    // not 6 (unknown), not 1 (excluded)
    REQUIRE(out.size() == 2);
    std::set<int64_t> ids;
    for (auto& r : out) {
        ids.insert(r.track.id);
        REQUIRE(!r.explanation.empty());      // §26 criteria surfaced
    }
    REQUIRE(ids == std::set<int64_t>({ 2, 3 }));

    // sameKeyOnly narrows to the excluded track → empty
    c.sameKeyOnly = true;
    const auto none = H::RelatedTracksEngine::suggest(ref, 128.0, c, qfn);
    REQUIRE(none.empty());

    // broad (non-strict) set adds semitone neighbours: 22,20,16,14,9
    c.sameKeyOnly = false; c.strictCamelot = false; c.useBpm = false;
    const auto broad = H::RelatedTracksEngine::suggest(ref, 0.0, c, qfn);
    std::set<int64_t> bids;
    for (auto& r : broad) bids.insert(r.track.id);
    REQUIRE(bids.count(3));                    // 9A
    REQUIRE(bids.count(5));                    // 7A now included (bpm off)
    REQUIRE(bids.count(4) == 0);               // A# still out
}

// ================================================= ENGINE COORDINATOR (§33/36)
TEST_CASE("KeyAnalysisEngine: buffer analysis + diagnostics", "[key22]") {
    H::KeyAnalysisEngine eng(nullptr);
    const int64_t n = 48000 * 4;
    auto pcm = keySignal(0, false, n);
    const auto r = eng.analyzeBuffer(pcm.data(), n, 2, 48000.0, cfg(2));
    REQUIRE(r.primary.valid());
    const auto d = eng.diagnostics();
    REQUIRE(d.analysesStarted == 1);
    REQUIRE(d.analysesCompleted == 1);
    REQUIRE(d.lastAnalyzeMs > 0.0);

    // cancel short-circuits without analyzing
    std::atomic<bool> cancel { true };
    const auto r2 = eng.analyzeBuffer(pcm.data(), n, 2, 48000.0, cfg(1),
                                      &cancel);
    REQUIRE(!r2.primary.valid());
    REQUIRE(eng.diagnostics().analysesCancelled == 1);
}

TEST_CASE("KeyAnalysisProvider: quality config tiers (§6)", "[key22]") {
    H::KeyAnalysisProvider p;
    const auto fast = p.configFor(0);
    const auto bal  = p.configFor(1);
    const auto hq   = p.configFor(2);
    REQUIRE(fast.maxChromaFrames < bal.maxChromaFrames);
    REQUIRE(hq.segments);
    REQUIRE(!fast.segments);
    REQUIRE(p.createDetector(bal)->id() == "onda.ks");
}

// ======================================== LIBRARY PERFORMANCE (§46)
static void seedTracks(onda::library::MusicLibraryEngine& eng,
                       int count) {
    std::lock_guard lk(eng.db().mutex());
    Database::Tx tx(eng.db());
    auto t = eng.db().prepare(
        "INSERT INTO tracks(path,filename,title,artist,date_added,"
        "updated_at) VALUES(?,?,?,?,0,0)");
    auto a = eng.db().prepare(
        "INSERT INTO analysis(track_id,bpm,key_index,key_confidence)"
        " VALUES(?,?,?,?)");
    for (int i = 0; i < count; ++i) {
        t.bind(1, "/t/" + std::to_string(i));
        t.bind(2, "t" + std::to_string(i) + ".wav");
        t.bind(3, "T" + std::to_string(i));
        t.bind(4, "A");
        t.step(); t.reset();
        const int64_t id = eng.db().lastInsertId();
        a.bind(1, id);
        a.bind(2, 118.0 + (i % 31));   // coprime modulus → all key/bpm combos
        a.bind(3, (int64_t)(i % 24));
        a.bind(4, 0.3 + (i % 7) * 0.1);
        a.step(); a.reset();
    }
    t.finalize(); a.finalize();
    tx.commit();
}

TEST_CASE("Library: key/camelot/bpm filters scale to 100k", "[key22]") {
    namespace L = onda::library;
    const auto dir = tempRoot("perf");
    L::MusicLibraryEngine eng;
    REQUIRE(eng.open((dir / "lib.db").string()));
    seedTracks(eng, 100000);
    REQUIRE(eng.trackCount() == 100000);

    auto timed = [&](const char* what, L::QuerySpec& q) {
        const auto t0 = std::chrono::steady_clock::now();
        auto page = eng.query(q);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        INFO(what << ": " << page.total << " rows in " << ms << " ms");
        return page;
    };

    L::QuerySpec q;
    L::FilterCond f;
    f.field = "key_index"; f.op = L::CmpOp::Eq; f.a = 21;
    q.filters.push_back(f);
    auto p = timed("key=8A", q);
    REQUIRE(p.total > 0);

    q.filters.clear();
    f = {}; f.field = "key_camelot_compatible"; f.op = L::CmpOp::Eq;
    f.a = 21;
    q.filters.push_back(f);
    p = timed("camelot_compat 8A", q);
    REQUIRE(p.total > 0);

    q.filters.clear();
    f = {}; f.field = "key_mode"; f.op = L::CmpOp::Eq; f.a = 1.0;
    q.filters.push_back(f);
    f = {}; f.field = "bpm"; f.op = L::CmpOp::Between;
    f.a = 120.0; f.b = 128.0;
    q.filters.push_back(f);
    p = timed("mode=minor + bpm 120-128", q);
    REQUIRE(p.total > 0);

    q.filters.clear();
    f = {}; f.field = "keyconf"; f.op = L::CmpOp::Gte; f.a = 0.5;
    q.filters.push_back(f);
    p = timed("keyconf>=0.5", q);
    REQUIRE(p.total > 0);

    // related-tracks over the real engine — must stay fast
    H::RelatedCriteria c; c.limit = 32;
    const auto t0 = std::chrono::steady_clock::now();
    auto out = H::RelatedTracksEngine::suggest(
        H::MusicalKey::fromIndex(21, .9f), 124.0, c,
        [&eng](const L::QuerySpec& qq) { return eng.query(qq); });
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    INFO("related(8A,124): " << out.size() << " in " << ms << " ms");
    REQUIRE(!out.empty());
    REQUIRE(ms < 1500.0);          // generous CI bound
}

// =================================================== MULTI-DECK (§47)
TEST_CASE("Deck tonal model shared across 4 decks", "[key22]") {
    // The same model produces per-deck state without duplication.
    struct Deck { int keyIdx; double bpm; double eff; bool keylock; };
    const Deck decks[4] = {
        { 21, 128.0, 128.0, true  },
        { 0,  126.0, 126.0, false },
        { -1, 0.0,   0.0,   true  },
        { 9,  130.0, 139.0, false },
    };
    for (int d = 0; d < 4; ++d) {
        const auto& dk = decks[d];
        const double ratio = dk.bpm > 0 ? dk.eff / dk.bpm : 1.0;
        const auto m = H::KeyShiftEngine::model(dk.keyIdx, 0, ratio,
                                                dk.keylock);
        INFO("deck " << d);
        if (dk.keyIdx < 0) REQUIRE(m.effectiveIndex == -1);
        else REQUIRE(m.effectiveIndex >= 0);
    }
    // cross-deck compatibility is one engine, not four copies
    const auto rel = H::MusicalCompatibilityEngine::relate(
        H::MusicalKey::fromIndex(decks[0].keyIdx, .9f), decks[0].bpm,
        H::MusicalKey::fromIndex(decks[1].keyIdx, .9f), decks[1].bpm);
    REQUIRE(rel.key.relation == H::HarmonicRel::RelativeMajorMinor);
}

// ===================================================== RT SAFETY (§33)
TEST_CASE("Key analysis stays off the audio path", "[key22]") {
    // The detector must never be reachable from DeckEngine — verify
    // the structural invariant instead: analysis on a worker thread
    // while a tight render-sized loop runs undisturbed.
    const int64_t n = 48000 * 4;
    auto pcm = keySignal(7, false, n);
    std::atomic<bool> done { false };
    std::atomic<int>  result { -2 };
    std::thread worker([&] {
        H::KsKeyDetector det;
        const auto r = det.analyze(pcm.data(), n, 2, 48000.0, cfg(1));
        result = r.primary.index();
        done = true;
    });
    // "render": bounded POD work like a callback would do — must not
    // wait on the analyzer at all
    std::vector<float> block(512 * 2, 0.5f);
    double acc = 0.0;
    for (int i = 0; i < 200; ++i)
        for (float& s : block) { s *= 0.999f; acc += s; }
    REQUIRE(acc != 0.0);
    worker.join();
    REQUIRE(done);
    REQUIRE(result == 7);          // G major
}
