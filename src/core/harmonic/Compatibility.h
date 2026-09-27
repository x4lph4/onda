#pragma once

#include <cmath>
#include <string>

#include "CamelotWheel.h"
#include "HarmonicTypes.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §13/§14 — harmonic relationship descriptors. INFORMATIONAL ONLY —
/// the engine describes relations, it never picks a "better" track
/// (§27: no automatic mix decisions).
/// ------------------------------------------------------------------
enum class HarmonicRel : int {
    Unknown = 0,            // missing/insufficient data — NOT "bad"
    SameKey,
    RelativeMajorMinor,     // 8A ↔ 8B
    AdjacentCamelot,        // 8A ↔ 7A/9A  (energy glide)
    EnergyChange,           // 8A ↔ 9B etc — adjacent-number mode jump
    DistantKey,             // known keys, no classic relation
};

/// §13 — one described relationship between two keys.
struct HarmonicRelation {
    HarmonicRel relation  { HarmonicRel::Unknown };
    float       score     { 0.0f };     // relation weight 0..1 (not "quality")
    int         semitoneDistance { 0 }; // pitch-class distance (signed, wrap)
    int         camelotDistance  { -1 };// wheel steps, -1 if n/a
    bool        modeMatch { false };
    bool        confidenceSufficient { false };
    std::string explanation;            // human-readable, UI-ready
};

class HarmonicCompatibilityEngine {
public:
    /// Minimum confidence for a relation to be reported as fact;
    /// below this the result is Unknown-with-explanation (§7).
    static constexpr float kMinConfidence = 0.15f;

    /// Describe current-vs-candidate. Either key unknown →
    /// HarmonicRel::Unknown (never a false "incompatible").
    static HarmonicRelation relate(const MusicalKey& current,
                                   const MusicalKey& candidate) {
        HarmonicRelation r;
        if (!current.valid() || !candidate.valid()) {
            r.explanation = "insufficient key data";
            return r;
        }
        r.confidenceSufficient =
            current.confidence >= kMinConfidence &&
            candidate.confidence >= kMinConfidence;

        const int cPos = CamelotConverter::fromIndex(current.index()).pos();
        const int aPos = CamelotConverter::fromIndex(candidate.index()).pos();
        r.semitoneDistance = wrapPc(candidate.pitchClass - current.pitchClass);
        r.modeMatch = current.mode == candidate.mode;
        r.camelotDistance = (cPos >= 0 && aPos >= 0)
            ? camelotSteps(cPos, aPos) : -1;

        const auto w = CamelotWheel::relationship(cPos, aPos);
        switch (w) {
            case CamelotWheel::Rel::Same:
                r.relation = HarmonicRel::SameKey; r.score = 1.0f;
                r.explanation = "same key"; break;
            case CamelotWheel::Rel::Relative:
                r.relation = HarmonicRel::RelativeMajorMinor; r.score = 0.9f;
                r.explanation = "relative major/minor"; break;
            case CamelotWheel::Rel::Adjacent:
                r.relation = HarmonicRel::AdjacentCamelot; r.score = 0.85f;
                r.explanation = "adjacent Camelot position"; break;
            case CamelotWheel::Rel::RelativeAdjacent:
                r.relation = HarmonicRel::EnergyChange; r.score = 0.5f;
                r.explanation = "adjacent-number mode change (energy shift)";
                break;
            default:
                r.relation = HarmonicRel::DistantKey; r.score = 0.15f;
                r.explanation = "no classic Camelot relation"; break;
        }
        if (!r.confidenceSufficient)
            r.explanation += " (low key confidence)";
        return r;
    }

private:
    static int wrapPc(int d) {
        d %= 12; if (d > 6) d -= 12; if (d < -6) d += 12; return d;
    }
    /// Steps around the wheel: same-number cross-side counts 0,
    /// number±1 same side counts 1.
    static int camelotSteps(int fromPos, int toPos) {
        const int fn = fromPos % 12, tn = toPos % 12;
        const int dn = std::min((tn - fn + 12) % 12, (fn - tn + 12) % 12);
        const int side = (fromPos < 12) != (toPos < 12) ? 1 : 0;
        return dn + side;
    }
};

/// ------------------------------------------------------------------
/// §15/§17 — tempo relationship. Pure description: difference, ratio,
/// half/double-time detection, the pitch% a DJ would need — never
/// applied automatically.
/// ------------------------------------------------------------------
struct TempoRelation {
    bool   valid          { false };
    double bpmDiff        { 0.0 };  // candidate - current
    double ratio          { 0.0 };  // candidate / current
    double pitchPercent   { 0.0 };  // % the CANDIDATE would shift to match
    bool   halfTime       { false };// ratio ≈ 0.5
    bool   doubleTime     { false };// ratio ≈ 2.0
    bool   withinRange    { false };// direct mix range (±pitch limit)
    std::string explanation;
};

class TempoCompatibility {
public:
    /// pitchLimitPct: the deck's usable tempo range for "mixable".
    static TempoRelation relate(double currentBpm, double candidateBpm,
                                double pitchLimitPct = 8.0) {
        TempoRelation r;
        if (currentBpm <= 0.0 || candidateBpm <= 0.0) {
            r.explanation = "missing BPM"; return r;
        }
        r.valid = true;
        r.ratio = candidateBpm / currentBpm;
        r.bpmDiff = candidateBpm - currentBpm;
        // §17 — tempo change needed on the candidate to reach current.
        r.pitchPercent = (currentBpm / candidateBpm - 1.0) * 100.0;
        r.halfTime   = std::fabs(r.ratio - 0.5) < 0.03;
        r.doubleTime = std::fabs(r.ratio - 2.0) < 0.03;
        r.withinRange = std::fabs(r.pitchPercent) <= pitchLimitPct;

        char buf[96];
        if (r.halfTime)
            std::snprintf(buf, sizeof buf,
                          "%+.1f BPM (≈half-time)", r.bpmDiff);
        else if (r.doubleTime)
            std::snprintf(buf, sizeof buf,
                          "%+.1f BPM (≈double-time)", r.bpmDiff);
        else
            std::snprintf(buf, sizeof buf, "%+.1f BPM (%+.1f%% pitch)",
                          r.bpmDiff, r.pitchPercent);
        r.explanation = buf;
        if (!r.withinRange) r.explanation += " — outside mix range";
        return r;
    }
};

/// ------------------------------------------------------------------
/// §16 — combined musical compatibility: tempo + key kept as
/// SEPARATE reported components — descriptive, not a scored verdict.
/// ------------------------------------------------------------------
struct MusicalCompatibility {
    TempoRelation    tempo;
    HarmonicRelation key;
    bool             mixableNow { false }; // both usable, informationally
    std::string      explanation;          // "128 vs 126 BPM (+1.6%), 8A→9A adjacent"
};

class MusicalCompatibilityEngine {
public:
    static MusicalCompatibility relate(
        const MusicalKey& currentKey, double currentBpm,
        const MusicalKey& candidateKey, double candidateBpm,
        double pitchLimitPct = 8.0) {
        MusicalCompatibility m;
        m.tempo = TempoCompatibility::relate(currentBpm, candidateBpm,
                                             pitchLimitPct);
        m.key   = HarmonicCompatibilityEngine::relate(currentKey, candidateKey);
        const bool keyOk = m.key.relation != HarmonicRel::Unknown
                        && m.key.relation != HarmonicRel::DistantKey;
        m.mixableNow = m.tempo.valid && m.tempo.withinRange && keyOk;
        std::string cam;
        const auto cc = CamelotConverter::fromIndex(currentKey.index());
        const auto ac = CamelotConverter::fromIndex(candidateKey.index());
        if (cc.valid() && ac.valid()) cam = " · " + cc.str() + "→" + ac.str();
        m.explanation = m.tempo.explanation + cam + " · " + m.key.explanation;
        return m;
    }
};

} // namespace onda::harmonic
