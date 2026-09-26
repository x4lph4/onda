#pragma once

#include "AnalysisTypes.h"

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
    /// samples: interleaved, `channels` 1|2; worker thread only.
    KeyResult analyze(const float* samples, int64_t numFrames,
                      int channels, double sampleRate);

    static const char* standardName(int key); // "C", "F#m", ...
    static const char* camelotName(int key);  // "8B", "4A", ...
};

} // namespace onda
