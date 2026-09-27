#pragma once

#include <cmath>
#include <string>

#include "CamelotWheel.h"
#include "HarmonicTypes.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §18/§19 — key-shift math + display model. Computes the semitone
/// displacement between keys and the EFFECTIVE key a deck is playing
/// (original analysis + explicit shift + tempo-induced drift when
/// key-lock is off). It never touches audio and never rewrites
/// analysis results — detected key stays detected key.
/// ------------------------------------------------------------------
struct KeyShiftModel {
    int  semitones      { 0 };   // explicit shift (-24..+24)
    int  effectiveIndex { -1 };  // original + semitones (+ tempo drift), -1 unknown
    int  originalIndex  { -1 };
    bool keyLocked      { false };
    float tempoDriftSemis { 0.0f }; // non-key-locked pitch drift (fractional)
};

class KeyShiftEngine {
public:
    /// §18 — semitones needed to move currentKey → targetKey
    /// (shortest path on the wheel: same-side number delta OR
    /// pitch-class delta; we report the musical semitone distance).
    static int semitonesBetween(const MusicalKey& from, const MusicalKey& to) {
        if (!from.valid() || !to.valid()) return 0;
        int d = to.pitchClass - from.pitchClass;
        d %= 12; if (d > 6) d -= 12; if (d < -6) d += 12;
        return d;
    }

    /// Shift a canonical index by `semitones`, preserving mode.
    /// Normalizes octave equivalents: any input works, result stays
    /// 0..23. -1 stays -1 (unknown never invented).
    static int shiftIndex(int keyIndex, int semitones) {
        if (keyIndex < 0 || keyIndex >= 24) return -1;
        const int mode = keyIndex / 12;         // 0 major, 1 minor
        const int pc = ((keyIndex % 12 + semitones) % 12 + 12) % 12;
        return mode * 12 + pc;
    }

    /// Deck display model (§19/§28): original + explicit shift +
    /// tempo drift (when !keyLocked, tempo ±% transposes the key).
    static KeyShiftModel model(int originalIndex, int explicitSemis,
                               double tempoRatio, bool keyLocked) {
        KeyShiftModel m;
        m.originalIndex = originalIndex;
        m.semitones = explicitSemis;
        m.keyLocked = keyLocked;
        // Varispeed transposes by 12·log2(ratio); key-lock stretches
        // without transposing → drift only counts when unlocked.
        m.tempoDriftSemis = keyLocked || tempoRatio <= 0.0
            ? 0.0f
            : (float)(12.0 * std::log2(tempoRatio));
        const int total = explicitSemis + (int)std::lround(m.tempoDriftSemis);
        m.effectiveIndex = shiftIndex(originalIndex, total);
        return m;
    }

    /// §19 — one-line display: "8A → 9A (+1)" / "8A" when clean.
    static std::string display(const KeyShiftModel& m) {
        if (m.originalIndex < 0) return "?";
        if (m.effectiveIndex < 0 || m.effectiveIndex == m.originalIndex)
            return CamelotConverter::fromIndex(m.originalIndex).str();
        const int d = (int)std::lround(m.tempoDriftSemis) + m.semitones;
        return CamelotConverter::fromIndex(m.originalIndex).str() + " → "
             + CamelotConverter::fromIndex(m.effectiveIndex).str()
             + " (" + (d >= 0 ? "+" : "") + std::to_string(d) + ")";
    }
};

} // namespace onda::harmonic
