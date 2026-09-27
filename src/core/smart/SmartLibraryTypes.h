#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onda::smart {

// ============================================================ ENERGY
/// Phase 23 §13-18 — energy is an ANALYSIS FEATURE: a continuous
/// descriptor derived from measured audio data (loudness, RMS,
/// waveform shape). It is descriptive, never an objective judgement
/// of the music.

/// Configurable energy category bounds (§15). Values are normalized
/// 0..1 edges; `levelFor(v)` maps a scalar into a named bucket.
struct EnergyLevels {
    float veryLow  { 0.20f };
    float low      { 0.40f };
    float medium   { 0.60f };
    float high     { 0.80f };   // >= high  → VeryHigh

    enum class Level : int {
        Unknown = -1, VeryLow = 0, Low, Medium, High, VeryHigh
    };

    Level levelFor(float v) const {
        if (v < 0.0f)  return Level::Unknown;
        if (v < veryLow) return Level::VeryLow;
        if (v < low)     return Level::Low;
        if (v < medium)  return Level::Medium;
        if (v < high)    return Level::High;
        return Level::VeryHigh;
    }
    static const char* name(Level l) {
        switch (l) {
            case Level::VeryLow:  return "VERY_LOW";
            case Level::Low:      return "LOW";
            case Level::Medium:   return "MEDIUM";
            case Level::High:     return "HIGH";
            case Level::VeryHigh: return "VERY_HIGH";
            default:              return "UNKNOWN";
        }
    }
};

/// One point of the energy-over-time curve (§14). `start`/`end` are
/// seconds; `value` is normalized 0..1.
struct EnergyPoint {
    double start { 0.0 }, end { 0.0 };
    float  value { 0.0f };
};

using EnergyCurve = std::vector<EnergyPoint>;

/// A segment where energy changes significantly (§16). `delta` is
/// signed (after − before); `confidence` reflects how sharp and how
/// sustained the change is — low-confidence transitions still carry
/// the flag but are marked as such.
struct EnergyTransition {
    double start { 0.0 }, end { 0.0 };
    float  delta { 0.0f };
    float  confidence { 0.0f };
};

/// §17-18 — structural section model. Detection is prepared but
/// honest: a segment is only reported when confidence clears the
/// threshold; kind may be Unknown. UI must distinguish detected from
/// user-defined via `userDefined`.
struct StructureSegment {
    enum class Kind : int {
        Unknown = 0, Intro, Outro, Breakdown, Build, Drop,
        Chorus, Verse
    };
    double start { 0.0 }, end { 0.0 };   // seconds
    Kind   kind { Kind::Unknown };
    float  confidence { 0.0f };
    bool   userDefined { false };        // §18 detected vs user

    static const char* name(Kind k) {
        switch (k) {
            case Kind::Intro:     return "intro";
            case Kind::Outro:     return "outro";
            case Kind::Breakdown: return "breakdown";
            case Kind::Build:     return "build";
            case Kind::Drop:      return "drop";
            case Kind::Chorus:    return "chorus";
            case Kind::Verse:     return "verse";
            default:              return "unknown";
        }
    }
    static Kind kindFor(const std::string& n) {
        if (n == "intro")     return Kind::Intro;
        if (n == "outro")     return Kind::Outro;
        if (n == "breakdown") return Kind::Breakdown;
        if (n == "build")     return Kind::Build;
        if (n == "drop")      return Kind::Drop;
        if (n == "chorus")    return Kind::Chorus;
        if (n == "verse")     return Kind::Verse;
        return Kind::Unknown;
    }
};

/// Full per-track energy descriptor persisted on the analysis row.
struct EnergyResult {
    float            energy { -1.0f };   // -1 = unknown/not analyzed
    EnergyLevels::Level level {
        EnergyLevels::Level::Unknown };
    EnergyCurve               curve;
    std::vector<EnergyTransition> transitions;
    std::vector<StructureSegment> structure;
    float            confidence { 0.0f };
    int              algorithmVersion { 1 };
};

// ========================================================= AUTO TAGS
/// §10-12 — auto tags live in `auto_tags`, NEVER in `tags`/
/// `track_tags`. Status decides lifecycle:
///   Suggested — produced by the engine, shown as suggestion
///   Accepted  — user confirmed (kept, counts as tag for filtering)
///   Rejected  — user denied; re-analysis must NOT re-suggest it
///   Ignored   — user dismissed without verdict (may re-suggest on
///               a *different* confidence only — never silently)
enum class AutoTagStatus : int {
    Suggested = 0, Accepted = 1, Rejected = 2, Ignored = 3
};

struct AutoTag {
    int64_t       id { 0 };
    int64_t       trackId { 0 };
    std::string   name;
    float         confidence { 0.0f };
    AutoTagStatus status { AutoTagStatus::Suggested };
    std::string   source;               // rule that produced it
    int64_t       createdAt { 0 }, updatedAt { 0 };
};

/// A tag the rules engine wants to propose for a track.
struct AutoTagSuggestion {
    std::string name;
    float       confidence { 0.0f };
    std::string source;                 // human-readable rule name
};

// ======================================================== BATCH OPS
/// §39-40 — one reversible batch operation. The inverse is stored as
/// prior values so a single undo restores everything atomically.
struct BatchUndoEntry {
    int64_t              trackId { 0 };
    std::string          tag;           // tag ops: tag name
    int                  intValue { 0 };// rating/color ops: prior value
    bool                 hadTag { false };
};

} // namespace onda::smart
