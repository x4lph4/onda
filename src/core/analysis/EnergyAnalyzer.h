#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "AnalysisTypes.h"
#include "WaveformData.h"
#include "../smart/EnergyAnalysisEngine.h"

namespace onda {

/// §44 — normalized energy score for the pipeline. Reuses the
/// documented Phase-23 heuristic (`smart::EnergyAnalysisEngine`) so
/// the library, smart playlists and the deck read ONE formula — no
/// duplicated DSP. Inputs are measurements the pipeline already made
/// (integrated LUFS, RMS, LRA, waveform overview) — zero extra cost.
///
/// `confidence` reflects input coverage: loudness + waveform both
/// present → high; waveform-only or loudness-only → degraded; the
/// score is still reported because a partial answer beats none.
class EnergyAnalyzer {
public:
    static EnergyResult score(const LoudnessResult& loud,
                              const WaveformData& wf) {
        EnergyResult r;
        const bool hasLoud = loud.valid;
        const bool hasWf   = !wf.overview.empty();
        if (!hasLoud && !hasWf) return r;

        r.score = smart::EnergyAnalysisEngine::energyOf(
            hasLoud ? loud.integratedLufs : -69.0f,
            hasLoud ? loud.rms : 0.0f,
            hasLoud ? loud.loudnessRange : 0.0f);
        if (r.score < 0.0f && hasWf) {
            // loudness invalid but waveform exists: estimate energy
            // from the overview RMS curve alone.
            double acc = 0.0; int cnt = 0;
            for (const auto& b : wf.overview) {
                if (b.rms > 0.0f) { acc += b.rms; ++cnt; }
            }
            if (cnt > 0) {
                const float m = (float)(acc / cnt);
                r.score = std::clamp(m / 0.5f, 0.0f, 1.0f);
                r.confidence = 0.4f;
                return r;
            }
        }
        r.confidence = hasLoud ? (hasWf ? 0.9f : 0.7f) : 0.4f;
        if (r.score < 0.0f) r.score = 0.0f;
        return r;
    }
};

} // namespace onda
