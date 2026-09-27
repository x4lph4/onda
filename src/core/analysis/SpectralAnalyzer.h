#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "AnalysisTypes.h"
#include "WaveformData.h"

namespace onda {

/// §43 — spectral character of a track. Two measurements, both over
/// the decoded PCM on the analysis worker:
///
///  1) Band energies — second-order Butterworth splits at 250 Hz /
///     4 kHz give low/mid/high fractions, accumulated per overview
///     bin (feeds WaveformData::bands, §42) and globally (SpectralResult).
///     Filter-bank measurement, not STFT — cheap O(n) for full tracks.
///  2) Spectral centroid — radix-2 FFT over Hann windows sampled
///     across the track; weighted mean frequency of the average
///     magnitude spectrum.
///
/// `confidence` reports how much signal actually had usable spectrum
/// (silence-heavy tracks score low) — it is NOT a perceptual metric.
class SpectralAnalyzer {
public:
    static constexpr double kLoHz = 250.0;
    static constexpr double kHiHz = 4000.0;
    static constexpr int    kFftSize = 2048;
    static constexpr int    kMaxWindows = 16;

    struct Out {
        SpectralResult result;
        /// kOverviewBins × {low,mid,high} fractions (mono).
        std::vector<float> bands;
    };

    Out analyze(const float* samples, int64_t n, int channels,
                double rate, int64_t numBins = WaveformData::kOverviewBins,
                const std::atomic<bool>* cancel = nullptr) const {
        Out o;
        if (samples == nullptr || n <= 0 || channels <= 0 || rate <= 0.0)
            return o;

        // ---- band split: LP(250) + HP(4k), mid = remainder ----
        const Biquad lp = butterLp(kLoHz, rate);
        const Biquad hp = butterHp(kHiHz, rate);
        std::vector<double> lo((size_t)numBins), mi((size_t)numBins),
            hi((size_t)numBins), tot((size_t)numBins);
        BiquadS s1{}, s2{};
        const double binsPerFrame = (double)numBins / (double)n;
        int64_t usedFrames = 0;
        for (int64_t i = 0; i < n; ++i) {
            if (cancel && (i & 0xFFFF) == 0 && cancel->load()) return o;
            float mono = 0.0f;
            for (int c = 0; c < channels; ++c)
                mono += samples[(size_t)i * (size_t)channels + (size_t)c];
            mono /= (float)channels;
            const float l = (float)lp.run(s1, mono);
            const float h = (float)hp.run(s2, mono);
            const float m = mono - l - h;
            const size_t b = std::min((size_t)(i * binsPerFrame),
                                      (size_t)numBins - 1);
            lo[b] += (double)l * l; mi[b] += (double)m * m;
            hi[b] += (double)h * h; tot[b] += (double)mono * mono;
        }
        usedFrames = n;

        // ---- global fractions + per-bin triples ----
        double gl = 0, gm = 0, gh = 0, gt = 0;
        int nonzeroBins = 0;
        o.bands.assign((size_t)numBins * 3, 0.0f);
        for (size_t b = 0; b < (size_t)numBins; ++b) {
            const double t = lo[b] + mi[b] + hi[b];
            gl += lo[b]; gm += mi[b]; gh += hi[b]; gt += t;
            if (tot[b] > 1e-9) ++nonzeroBins;
            if (t > 0.0) {
                o.bands[b * 3 + 0] = (float)(lo[b] / t);
                o.bands[b * 3 + 1] = (float)(mi[b] / t);
                o.bands[b * 3 + 2] = (float)(hi[b] / t);
            } else {
                o.bands[b * 3 + 0] = o.bands[b * 3 + 1] =
                    o.bands[b * 3 + 2] = 1.0f / 3.0f;
            }
        }
        if (gt > 0.0) {
            o.result.low  = (float)(gl / gt);
            o.result.mid  = (float)(gm / gt);
            o.result.high = (float)(gh / gt);
        }
        o.result.confidence =
            (float)std::min(1.0, (double)nonzeroBins / (numBins * 0.5));

        // ---- centroid: windowed FFT over spread positions ----
        o.result.centroidHz =
            centroid(samples, n, channels, rate, cancel, usedFrames);
        return o;
    }

private:
    struct BiquadS { double x1{0}, x2{0}, y1{0}, y2{0}; };
    struct Biquad { double b0, b1, b2, a1, a2;
        double run(BiquadS& s, float x) const {
            const double y = b0 * x + b1 * s.x1 + b2 * s.x2
                           - a1 * s.y1 - a2 * s.y2;
            s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
            return y;
        }
    };
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kSqrt2 = 1.41421356237309504880;
    static Biquad butterLp(double fc, double fs) {
        const double w = std::tan(kPi * fc / fs);
        const double k = w * w, d = 1.0 + kSqrt2 * w + k;
        return { k / d, 2 * k / d, k / d,
                 2 * (k - 1) / d, (1 - kSqrt2 * w + k) / d };
    }
    static Biquad butterHp(double fc, double fs) {
        const double w = std::tan(kPi * fc / fs);
        const double k = w * w, d = 1.0 + kSqrt2 * w + k;
        return { 1 / d, -2 / d, 1 / d,
                 2 * (k - 1) / d, (1 - kSqrt2 * w + k) / d };
    }

    /// Iterative radix-2 FFT (in-place, size must be pow2).
    static void fft(std::vector<std::complex<double>>& a) {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const double ang = -2.0 * kPi / (double)len;
            const std::complex<double> w(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<double> wn(1.0, 0.0);
                for (size_t j = 0; j < len / 2; ++j) {
                    const auto u = a[i + j];
                    const auto v = a[i + j + len / 2] * wn;
                    a[i + j] = u + v; a[i + j + len / 2] = u - v;
                    wn *= w;
                }
            }
        }
    }

    /// Mean-frequency-of-magnitude over Hann windows spread through
    /// the track. -1 when there is too little energy to say anything.
    static float centroid(const float* s, int64_t n, int channels,
                          double rate, const std::atomic<bool>* cancel,
                          int64_t /*used*/) {
        if (n < kFftSize) return -1.0f;
        const int wins = (int)std::min<int64_t>(
            kMaxWindows, n / kFftSize);
        if (wins <= 0) return -1.0f;
        std::vector<double> magAcc((size_t)kFftSize / 2, 0.0);
        std::vector<std::complex<double>> buf((size_t)kFftSize);
        int used = 0;
        for (int w = 0; w < wins; ++w) {
            if (cancel && cancel->load()) return -1.0f;
            const int64_t start = (n - kFftSize) * w /
                                  std::max(1, wins - 1);
            for (int i = 0; i < kFftSize; ++i) {
                float mono = 0.0f;
                for (int c = 0; c < channels; ++c)
                    mono += s[((size_t)start + (size_t)i) *
                              (size_t)channels + (size_t)c];
                mono /= (float)channels;
                const double hann = 0.5 * (1.0 - std::cos(
                    2.0 * kPi * i / (kFftSize - 1)));
                buf[(size_t)i] = { (double)mono * hann, 0.0 };
            }
            fft(buf);
            double wsum = 0.0;
            for (int i = 0; i < kFftSize / 2; ++i) {
                const double m = std::abs(buf[(size_t)i]);
                magAcc[(size_t)i] += m; wsum += m;
            }
            if (wsum > 1e-6) ++used;
        }
        if (used == 0) return -1.0f;
        double num = 0.0, den = 0.0;
        const double binHz = rate / kFftSize;
        for (int i = 1; i < kFftSize / 2; ++i) {
            num += magAcc[(size_t)i] * i * binHz;
            den += magAcc[(size_t)i];
        }
        return den > 0.0 ? (float)(num / den) : -1.0f;
    }
};

} // namespace onda
