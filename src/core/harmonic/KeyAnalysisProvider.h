#pragma once

#include <algorithm>
#include <ctime>
#include <memory>
#include <string>

#include "../analysis/KeyAnalyzer.h"
#include "HarmonicTypes.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §4/§36 — detector abstraction. The engine and every caller consume
/// KeyAnalysisResult; the algorithm inside the detector is free to
/// change (K-S today, a trained model later) without touching them.
/// Runs on ANALYSIS WORKER threads — never the audio callback (§33).
/// ------------------------------------------------------------------
class KeyDetector {
public:
    virtual ~KeyDetector() = default;
    virtual std::string id() const = 0;            // "onda.ks"
    virtual std::string version() const = 0;       // cache identity
    /// samples: interleaved; cfg selects duration/resolution/segments.
    virtual KeyAnalysisResult analyze(const float* samples,
                                      int64_t numFrames, int channels,
                                      double sampleRate,
                                      const KeyAnalysisConfig& cfg) = 0;
};

/// Default detector — chromagram + Krumhansl-Schmuckler (shared code
/// with the pipeline's KeyAnalyzer; same math, richer result).
class KsKeyDetector final : public KeyDetector {
public:
    static constexpr const char* kId      = "onda.ks";
    static constexpr const char* kVersion = "1.0.0";

    std::string id() const override      { return kId; }
    std::string version() const override { return kVersion; }

    KeyAnalysisResult analyze(const float* samples, int64_t numFrames,
                              int channels, double sampleRate,
                              const KeyAnalysisConfig& cfg) override {
        KeyAnalysisResult r;
        r.algorithm = kId; r.version = kVersion; r.quality = cfg.quality;
        r.analyzedAt = (int64_t)::time(nullptr);

        // §6 quality → duration/resolution budget
        const int64_t limit = cfg.maxAnalyzeSec > 0.0
            ? std::min<int64_t>(numFrames,
                  (int64_t)(cfg.maxAnalyzeSec * sampleRate))
            : numFrames;
        const int maxWin = cfg.quality == (int)KeyQuality::Fast ? 400
                         : cfg.quality == (int)KeyQuality::HighQuality ? 3000
                         : cfg.maxChromaFrames;

        KeyAnalyzer ka;
        const auto d = ka.analyzeDetail(samples, limit, channels,
                                      sampleRate, maxWin);
        r.primary = MusicalKey::fromIndex(d.key, d.confidence);
        for (int i = 0; i < 12; ++i) r.chroma[(size_t)i] = (float)d.chroma[i];

        // §8 — alternatives: top-3 runner-ups (raw correlation kept).
        int idx[24]; for (int i = 0; i < 24; ++i) idx[i] = i;
        std::sort(idx, idx + 24,
                  [&](int a, int b) { return d.corr[a] > d.corr[b]; });
        const double best = d.key >= 0 ? d.corr[d.key] : -2.0;
        for (int i = 1; i < 24 && r.alternatives.size() < 3; ++i) {
            if (d.corr[idx[i]] <= -1.5) break;
            r.alternatives.push_back({
                MusicalKey::fromIndex(idx[i], 0.0f),
                (float)d.corr[idx[i]] });
        }
        // ambiguous: runner-up within 0.05 of best AND mediocre conf
        r.ambiguous = d.key >= 0 && best > 0.0 && !r.alternatives.empty()
            && (best - r.alternatives.front().correlation) < 0.05
            && d.confidence < 0.5f;

        // §31/§32 — segment pass (HighQuality default; opt-in by cfg)
        if (cfg.segments) {
            const int64_t seg = (int64_t)(cfg.segmentSec * sampleRate);
            for (const auto& g : ka.analyzeSegments(
                     samples, numFrames, channels, sampleRate, seg)) {
                TonalSegment ts;
                ts.startFrame = g.startFrame; ts.endFrame = g.endFrame;
                ts.key = MusicalKey::fromIndex(g.key, g.confidence);
                r.segments.push_back(ts);
            }
        }
        return r;
    }
};

/// §36 — swappable provider. `createDetector` returns the best impl
/// for the requested config; callers hold only the interface.
class KeyAnalysisProvider {
public:
    std::unique_ptr<KeyDetector> createDetector(
        const KeyAnalysisConfig& /*cfg*/) const {
        return std::make_unique<KsKeyDetector>();
    }
    /// Quality → runtime knobs (§6): duration analyzed, resolution,
    /// segment pass.
    KeyAnalysisConfig configFor(int quality) const {
        KeyAnalysisConfig c;
        c.quality = quality;
        switch ((KeyQuality)quality) {
            case KeyQuality::Fast:
                c.maxChromaFrames = 400; c.maxAnalyzeSec = 45.0;
                c.segments = false; break;
            case KeyQuality::Balanced:
                c.maxChromaFrames = 1500; c.maxAnalyzeSec = 0.0;
                c.segments = false; break;
            case KeyQuality::HighQuality:
                c.maxChromaFrames = 3000; c.maxAnalyzeSec = 0.0;
                c.segments = true; c.segmentSec = 8; break;
            default: break;
        }
        return c;
    }
};

} // namespace onda::harmonic
