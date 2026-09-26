#include "BpmAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace onda {

std::vector<float> BpmAnalyzer::autocorr_(const float* env, int n,
                                        int lagLo, int lagHi) const {
    std::vector<float> ac((size_t)(lagHi - lagLo + 1), 0.0f);
    for (int lag = lagLo; lag <= lagHi; ++lag) {
        double acc = 0.0;
        const int lim = n - lag;
        for (int i = 0; i < lim; ++i)
            acc += (double)env[i] * env[i + lag];
        ac[(size_t)(lag - lagLo)] = (float)(acc / (double)lim);
    }
    return ac;
}

double BpmAnalyzer::bestLagBpm_(const float* env, int n, double hopSec,
                                int* lagOut, float* scoreOut) const {
    const int lagLo = std::max(2, (int)std::ceil(60.0 / (kMaxBpm * hopSec)));
    const int lagHi = std::min(n - 2, (int)std::floor(60.0 / (kMinBpm * hopSec)));
    if (lagHi <= lagLo) { *lagOut = 0; *scoreOut = 0.0f; return 0.0; }

    const auto ac = autocorr_(env, n, lagLo, lagHi);
    int bestLag = lagLo; float best = 0.0f;
    for (int lag = lagLo; lag <= lagHi; ++lag) {
        // Octave-aware weighting: reward lags that also align at 2x/0.5x.
        float w = ac[(size_t)(lag - lagLo)];
        if (lag * 2 <= lagHi) w += 0.35f * ac[(size_t)(lag * 2 - lagLo)];
        if (lag % 2 == 0 && lag / 2 >= lagLo)
            w += 0.35f * ac[(size_t)(lag / 2 - lagLo)];
        if (w > best) { best = w; bestLag = lag; }
    }
    *lagOut = bestLag;
    *scoreOut = best;
    return 60.0 / (hopSec * (double)bestLag);
}

BpmResult BpmAnalyzer::analyze(const OnsetDetector::Result& onset) {
    BpmResult r;
    const int n = (int)onset.envelope.size();
    if (!onset.onsets.empty())
        r.firstOnsetFrame = onset.onsets.front().frame;
    if (n < 32) { r.confidence = 0.0f; return r; }

    const double hopSec = onset.hop / onset.rate;

    // ---- global tempo ----
    int bestLag = 0; float bestScore = 0.0f;
    r.bpm = bestLagBpm_(onset.envelope.data(), n, hopSec, &bestLag, &bestScore);

    // ---- candidate list: local maxima of the autocorrelation ----
    const int lagLo = std::max(2, (int)std::ceil(60.0 / (kMaxBpm * hopSec)));
    const int lagHi = std::min(n - 2, (int)std::floor(60.0 / (kMinBpm * hopSec)));
    if (lagHi > lagLo && bestLag > 0) {
        const auto ac = autocorr_(onset.envelope.data(), n, lagLo, lagHi);
        float mean = 0.0f;
        for (float v : ac) mean += v;
        mean /= (float)ac.size();

        std::vector<BpmCandidate> cands;
        for (int lag = lagLo + 1; lag < lagHi; ++lag) {
            const float v = ac[(size_t)(lag - lagLo)];
            if (v <= ac[(size_t)(lag - lagLo - 1)] ||
                v <  ac[(size_t)(lag - lagLo + 1)] || v <= mean) continue;
            const double b = 60.0 / (hopSec * (double)lag);
            // merge near-duplicates (±1.5%)
            bool merged = false;
            for (auto& c : cands)
                if (std::fabs(c.bpm - b) / b < 0.015) { c.score = std::max(c.score, v); merged = true; }
            if (!merged) cands.push_back({ b, v });
        }
        std::sort(cands.begin(), cands.end(),
                  [](const BpmCandidate& a, const BpmCandidate& b) { return a.score > b.score; });
        const float top = cands.empty() ? 1.0f : cands.front().score;
        for (auto& c : cands) c.score = top > 0.0f ? c.score / top : 0.0f;
        if (cands.size() > 6) cands.resize(6);
        r.candidates = std::move(cands);

        // Confidence: best peak vs. mean level and vs. runner-up.
        const float margin = bestScore > 0.0f
            ? (top - mean) / std::max(bestScore, 1e-9f) : 0.0f;
        const float rivalry = r.candidates.size() > 1 && r.candidates[0].score > 0.0f
            ? 1.0f - r.candidates[1].score : 1.0f;
        r.confidence = std::clamp(
            0.5f * std::clamp(margin * 4.0f, 0.0f, 1.0f) +
            0.5f * std::clamp(rivalry * 2.0f, 0.0f, 1.0f), 0.0f, 1.0f);
    }

    // ---- stability: per-window dominant lag ----
    // Window adapts to track length: 16 s on long tracks, down to 4 s
    // (or quarter-length) on short clips so tempo changes still show.
    const int winN = (int)std::clamp<int64_t>(
        (int64_t)n / 4,
        (int64_t)std::llround(4.0 / hopSec),
        (int64_t)std::llround(16.0 / hopSec));
    const int hopN = std::max(1, winN / 2);
    std::vector<double> windowBpms;
    std::vector<int64_t> windowFrames;
    for (int s = 0; s + winN <= n; s += hopN) {
        int lag; float sc;
        const double b = bestLagBpm_(onset.envelope.data() + s, winN, hopSec, &lag, &sc);
        if (b > 0.0) {
            windowBpms.push_back(b);
            windowFrames.push_back((int64_t)llround(s * onset.hop));
        }
    }
    if (windowBpms.size() >= 2) {
        double mean = std::accumulate(windowBpms.begin(), windowBpms.end(), 0.0)
                      / (double)windowBpms.size();
        double var = 0.0;
        for (double b : windowBpms) var += (b - mean) * (b - mean);
        var /= (double)windowBpms.size();
        const double sd = std::sqrt(var);
        const double spread = sd / std::max(mean, 1.0);
        r.stability = (float)std::clamp(1.0 - spread * 12.0, 0.0, 1.0);

        if (spread > 0.02) {
            // Collapse consecutive similar windows into tempo segments.
            r.variable = true;
            int64_t beatCursor = 0;
            int64_t frameCursor = 0;
            for (size_t i = 0; i < windowBpms.size(); ++i) {
                const double b = windowBpms[i];
                if (r.tempoMap.empty() ||
                    std::fabs(r.tempoMap.back().bpm - b) / b > 0.02) {
                    TempoSegment seg;
                    seg.startFrame = i == 0 ? 0 : windowFrames[i];
                    seg.startBeat  = beatCursor;
                    seg.bpm        = b;
                    r.tempoMap.push_back(seg);
                }
                const int64_t next = i + 1 < windowBpms.size()
                    ? windowFrames[i + 1] : (int64_t)(n * onset.hop);
                const double secs = (next - windowFrames[i]) / onset.rate;
                beatCursor += (int64_t)std::llround(secs * b / 60.0);
            }
            r.confidence *= 0.7f; // coarse map — lower certainty by design
        }
    } else {
        r.stability = 0.5f;
    }
    return r;
}

} // namespace onda
