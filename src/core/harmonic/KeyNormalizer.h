#pragma once

#include <string>

#include "../analysis/KeyAnalyzer.h"
#include "../library/KeyNotation.h"
#include "HarmonicTypes.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §9 — enharmonic + notation normalization. The canonical value is
/// (pitch class, mode); spelling is derived, never stored — so
/// switching notation can't lose musical information.
/// ------------------------------------------------------------------
class KeyNormalizer {
public:
    using Notation = onda::library::KeyNotation;

    /// Display string for a canonical key index under a notation.
    static std::string display(int keyIndex, Notation n) {
        return onda::library::KeyNotationConverter::name(keyIndex, n);
    }
    static std::string display(const MusicalKey& k, Notation n) {
        return display(k.index(), n);
    }

    /// Sharp vs flat spelling for one pitch class (mode-agnostic).
    static const char* sharpPitchClass(int pc) {
        static const char* names[12] =
            { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return pc >= 0 && pc < 12 ? names[pc] : "?";
    }
    static const char* flatPitchClass(int pc) {
        static const char* names[12] =
            { "C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B" };
        return pc >= 0 && pc < 12 ? names[pc] : "?";
    }

    /// Parse "C#", "Db", "Am", "F#m" → MusicalKey (enharmonics land on
    /// the same pitch class — C# and Db are identical data). Returns
    /// invalid key for unparseable input.
    static MusicalKey parseStandard(const std::string& s, float conf = 0.0f) {
        const int idx = onda::library::KeyNotationConverter::parse(s);
        return MusicalKey::fromIndex(idx, conf);
    }

    /// Normalize an arbitrary display string to a chosen notation —
    /// C#→Db under FLAT, Db→C# under SHARP, etc. Unknown input → "?".
    static std::string normalize(const std::string& s, Notation target) {
        return display(onda::library::KeyNotationConverter::parse(s),
                       target);
    }
};

} // namespace onda::harmonic
