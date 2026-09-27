#pragma once

#include "AnalysisTypes.h"

#include <array>
#include <vector>

namespace onda {

/// Musical key estimation via chromagram + Krumhansl-Schmuckler
/// profiles (major + minor, all 12 rotations).
///
/// Method: STFT the mono mix, fold FFT magnitudes into a 12-bin pitch
/// class histogram, then correlate against the K-S templates; best
/// correlation wins. Confidence = margin between best and runner-up,
/// normalised — kept explicitly so the UI can flag guesses.
///
/// Swap point: the class implements one `analyze()` on mono float
/// audio; a different algorithm (e.g. trained model) can replace the
/// internals without touching callers.
class KeyAnalyzer {
public:
    /// Full-correlation output (Phase 22 §8/§37): every candidate's
    /// Pearson score + the normalized chroma — the KeyAnalysisEngine
    /// turns this into primary/alternatives/segments.
    struct Detail {
        int    key { -1 };
        float  confidence { 0.0f };
        double chroma[12] {};
        /// correlation per key index 0..23 (best-first NOT sorted —
        /// callers rank).
        double corr[24] {};
    };
    /// One window's estimate inside the segment pass (§31/§32).
    struct SegmentGuess {
        int64_t startFrame { 0 }, endFrame { 0 };
        int     key { -1 };             // -1 = UNKNOWN (low conf)
        float   confidence { 0.0f };
    };

    /// samples: interleaved, `channels` 1|2; worker thread only.
    KeyResult analyze(const float* samples, int64_t numFrames,
                      int channels, double sampleRate);

    /// Phase 22 — same pass as analyze() but returns the full
    /// correlation table + normalized chroma. `maxFrames` caps the
    /// number of FFT windows consumed (quality tiers); <= 0 = default.
    Detail analyzeDetail(const float* samples, int64_t numFrames,
                         int channels, double sampleRate,
                         int maxFrames = 0);

    /// §31 — per-window key guesses over the track. `segFrames` is the
    /// window size in frames; low-confidence windows report key=-1
    /// (never invented). Silence-only windows are skipped.
    std::vector<SegmentGuess> analyzeSegments(
        const float* samples, int64_t numFrames, int channels,
        double sampleRate, int64_t segFrames);

    static const char* standardName(int key); // "C", "F#m", ...
    static const char* camelotName(int key);  // "8B", "4A", ...
    /// Flat-variant standard names (§10 FLAT notation) — "Dbm" etc.
    static const char* flatName(int key);
};

} // namespace onda
