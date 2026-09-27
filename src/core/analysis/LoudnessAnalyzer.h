#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "AnalysisTypes.h"

namespace onda {

/// Loudness measurement (§21) — BS.1770-style: K-weighting
/// approximated by the standard two-filter pre-stage (high-shelf
/// +4 dB @~1.5 kHz then 38 Hz high-pass, coefficients at 48 kHz
/// reference) feeding gated mean-square integrated loudness and a
/// p10–p95 loudness range over 3 s windows.
///
/// Informational only — output feeds library info / optional gain
/// suggestions / diagnostics. The audio is never modified.
///
/// NOTE: coefficients are the fixed 48 kHz set; at other rates the
/// filters are still applied (documented approximation, not a strict
/// BS.1770 certification).
class LoudnessAnalyzer {
public:
    LoudnessResult analyze(const float* samples, int64_t n,
                           int channels, double rate) const {
        LoudnessResult r;
        if (samples == nullptr || n <= 0 || channels <= 0) return r;

        // ---- peak + RMS (unweighted, honest numbers) ----
        float peak = 0.0f; double acc = 0.0;
        for (int64_t i = 0; i < n * channels; ++i) {
            const float s = samples[i];
            peak = std::max(peak, std::fabs(s));
            acc += (double)s * s;
        }
        r.peak = peak;
        r.rms  = (float)std::sqrt(acc / (double)(n * channels));

        // ---- K-weighted channel sum ----
        std::vector<float> k((size_t)n);
        Filter shelf = shelf48k(), hp = hp48k();
        float z1 = 0, z2 = 0, w1 = 0, w2 = 0;
        float hz1 = 0, hz2 = 0, hw1 = 0, hw2 = 0;
        for (int64_t i = 0; i < n; ++i) {
            float mono = 0.0f;
            for (int c = 0; c < channels; ++c)
                mono += samples[(size_t)i * (size_t)channels + (size_t)c];
            float y = shelf.b0 * mono + shelf.b1 * z1 + shelf.b2 * z2
                    - shelf.a1 * w1 - shelf.a2 * w2;
            z2 = z1; z1 = mono; w2 = w1; w1 = y;
            float h = hp.b0 * y + hp.b1 * hz1 + hp.b2 * hz2
                    - hp.a1 * hw1 - hp.a2 * hw2;
            hz2 = hz1; hz1 = y; hw2 = hw1; hw1 = h;
            k[(size_t)i] = h;
        }

        // ---- momentary power in 400 ms blocks → integrated ----
        const int64_t blk = std::max<int64_t>((int64_t)(rate * 0.4), 1);
        std::vector<double> blockPow;
        for (int64_t i = 0; i + blk <= n; i += blk) {
            double a = 0.0;
            for (int64_t j = i; j < i + blk; ++j)
                a += (double)k[(size_t)j] * k[(size_t)j];
            blockPow.push_back(a / (double)blk);
        }
        double gated = 0.0; int gc = 0;
        for (double p : blockPow)
            if (p > 1e-10) { gated += p; ++gc; }  // -70 LUFS abs gate
        if (gc > 0) {
            const double ms = gated / gc;
            r.integratedLufs = (float)(-0.691 + 10.0 * std::log10(ms));
            r.valid = true;
        }

        // ---- LRA: 3 s windows, p10–p95 ----
        const int64_t win = std::max<int64_t>((int64_t)(rate * 3.0), 1);
        std::vector<float> lu;
        for (int64_t i = 0; i + win <= n; i += win / 2) {
            double a = 0.0;
            for (int64_t j = i; j < i + win; ++j)
                a += (double)k[(size_t)j] * k[(size_t)j];
            const double ms = a / (double)win;
            if (ms > 1e-10)
                lu.push_back((float)(-0.691 + 10.0 * std::log10(ms)));
        }
        if (lu.size() >= 4) {
            std::sort(lu.begin(), lu.end());
            r.loudnessRange =
                lu[(size_t)(lu.size() * 0.95) - 1] -
                lu[(size_t)(lu.size() * 0.10)];
        }

        // ---- true-peak estimate (§45) ----
        // 4x Catmull-Rom oversampling: the reconstructed waveform can
        // legitimately exceed the sample peak between samples
        // (linear interpolation never overshoots, so it cannot detect
        // intersample overs). Cubic approximates the sinc
        // reconstruction closely enough to flag clipped-looking overs;
        // documented approximation — not a certification meter.
        {
            float tp = 0.0f;
            for (int c = 0; c < channels; ++c) {
                for (int64_t i = 0; i < n - 1; ++i) {
                    const float p0 = sampleAt(samples, n, channels,
                                              i - 1, c);
                    const float p1 = sampleAt(samples, n, channels, i, c);
                    const float p2 = sampleAt(samples, n, channels,
                                              i + 1, c);
                    const float p3 = sampleAt(samples, n, channels,
                                              i + 2, c);
                    tp = std::max(tp, std::fabs(p1));
                    for (float t : { 0.25f, 0.5f, 0.75f }) {
                        const float v = 0.5f * ((2.0f * p1) +
                            (-p0 + p2) * t +
                            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
                        tp = std::max(tp, std::fabs(v));
                    }
                }
            }
            r.truePeak = std::min(tp, 4.0f);
        }
        return r;
    }

private:
    /// Edge-clamped channel sample access for the oversampler.
    static float sampleAt(const float* s, int64_t n, int ch,
                          int64_t i, int c) {
        i = std::clamp<int64_t>(i, 0, n - 1);
        return s[(size_t)i * (size_t)ch + (size_t)c];
    }

    struct Filter { double b0, b1, b2, a1, a2; };
    // BS.1770 pre-filter stage 1 (high-shelf) & stage 2 (high-pass),
    // bilinear coefficients computed for 48 kHz.
    static Filter shelf48k() { return { 1.53512485958697,
        -2.69169618940638, 1.19839281085285,
        -1.69065929318241, 0.73248077421585 }; }
    static Filter hp48k()    { return { 1.0, -2.0, 1.0,
        -1.99004745483398, 0.99007225036621 }; }
};

} // namespace onda
