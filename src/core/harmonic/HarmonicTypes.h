#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// Phase 22 — key-analysis domain model. Pure data + small helpers;
/// no JUCE, no DSP here.
///
/// The canonical key identity stays the 0..23 index used across the
/// app (0-11 majors C..B, 12-23 minors Cm..Bm, -1 unknown). Every
/// notation (standard/flat/camelot/open-key) is a VIEW of that index,
/// never duplicated data (§10).
/// ------------------------------------------------------------------

/// §3 — detected modes today; the enum reserves richer modes so a
/// future detector can report them without changing the model.
enum class KeyMode : int {
    Major = 0,
    Minor = 1,
    // ---- reserved (§3 — never reported by the K-S detector) ----
    Dorian, Phrygian, Lydian, Mixolydian, Aeolian, Locrian,
    Unknown
};
inline const char* keyModeName(KeyMode m) {
    switch (m) {
        case KeyMode::Major:      return "major";
        case KeyMode::Minor:      return "minor";
        case KeyMode::Dorian:     return "dorian";
        case KeyMode::Phrygian:   return "phrygian";
        case KeyMode::Lydian:     return "lydian";
        case KeyMode::Mixolydian: return "mixolydian";
        case KeyMode::Aeolian:    return "aeolian";
        case KeyMode::Locrian:    return "locrian";
        default:                  return "unknown";
    }
}

/// §2 — one musical key estimate. `pitchClass` is 0..11 (C..B) or
/// -1 for UNKNOWN — an unknown key is data, not an error (§7/§8).
struct MusicalKey {
    int     pitchClass { -1 };          // 0..11, -1 unknown
    KeyMode mode       { KeyMode::Unknown };
    float   confidence { 0.0f };        // 0..1

    bool valid() const { return pitchClass >= 0 && pitchClass < 12; }

    /// Canonical index 0..23 (major first) or -1.
    int index() const {
        if (!valid()) return -1;
        if (mode == KeyMode::Major) return pitchClass;
        if (mode == KeyMode::Minor) return pitchClass + 12;
        return -1;
    }
    static MusicalKey fromIndex(int idx, float conf = 0.0f) {
        if (idx < 0 || idx >= 24) return {};
        return { idx % 12,
                 idx < 12 ? KeyMode::Major : KeyMode::Minor, conf };
    }
};

/// §32 — a contiguous tonal region inside a track. `key` may be
/// invalid (UNKNOWN) where confidence was insufficient (§31 — never
/// invented).
struct TonalSegment {
    int64_t    startFrame { 0 };        // device-rate frames
    int64_t    endFrame   { 0 };
    MusicalKey key;
};

/// §6 — analysis quality tiers. Selects analysis duration, chromagram
/// resolution (window count) and whether the segment pass runs.
enum class KeyQuality : int { Fast = 0, Balanced = 1, HighQuality = 2 };
inline const char* keyQualityName(KeyQuality q) {
    switch (q) {
        case KeyQuality::Fast:        return "fast";
        case KeyQuality::Balanced:    return "balanced";
        case KeyQuality::HighQuality: return "high";
        default:                      return "?";
    }
}

/// Detector runtime knobs (§6) — the provider translates quality into
/// these; tests may set them directly.
struct KeyAnalysisConfig {
    int     quality       { (int)KeyQuality::Balanced };
    int     maxChromaFrames { 1500 };   // whole-track pass cap
    double  maxAnalyzeSec   { 0.0 };    // 0 = whole track
    bool    segments      { false };    // run the segment pass (§31)
    int     segmentSec    { 8 };        // window length, seconds
};

/// §8 — runner-up key (kept whenever the algorithm exposes them).
struct KeyAlternative {
    MusicalKey key;
    float      correlation { 0.0f };    // raw Pearson vs primary
};

/// §37/§40 — full detector output. The rest of the app consumes THIS
/// type; the internal algorithm stays behind the provider (§36).
struct KeyAnalysisResult {
    MusicalKey                primary;
    std::vector<KeyAlternative> alternatives;   // best-first, ≤3
    std::vector<TonalSegment>   segments;       // §32 (opt-in)
    std::string               algorithm;        // e.g. "onda.ks"
    std::string               version;          // algorithm version
    int64_t                   analyzedAt { 0 }; // unix ts (§40)
    int                       quality { (int)KeyQuality::Balanced };
    /// Whole-track 12-bin pitch-class profile (0..1, normalized) —
    /// TonalProfile feeds compatibility displays. Empty when the
    /// detector doesn't expose it.
    std::array<float, 12>     chroma {};
    bool                      ambiguous { false }; // §8 flag
};

} // namespace onda::harmonic
