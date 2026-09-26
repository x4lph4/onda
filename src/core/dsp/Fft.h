#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "Biquad.h" // kPi

namespace onda {

/// In-place radix-2 FFT for analysis (worker threads only — never RT).
/// Power-of-two sizes, iterative Cooley-Tukey, no allocation after prepare.
class Fft {
public:
    void prepare(int order) noexcept {
        order_ = order;
        size_ = 1 << order;
        const double ang = -2.0 * kPi / (double)size_;
        tw_.assign((size_t)size_ / 2, {});
        for (int i = 0; i < size_ / 2; ++i)
            tw_[(size_t)i] = { (float)std::cos(ang * i),
                               (float)std::sin(ang * i) };
        // bit-reversal table
        rev_.assign((size_t)size_, 0);
        for (int i = 0; i < size_; ++i) {
            int r = 0;
            for (int b = 0; b < order_; ++b)
                r |= ((i >> b) & 1) << (order_ - 1 - b);
            rev_[(size_t)i] = r;
        }
        win_.assign((size_t)size_, 0.0f);
        for (int i = 0; i < size_; ++i) // Hann
            win_[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * (float)kPi * i / (size_ - 1));
        frame_.assign((size_t)size_, {});
    }

    int size() const noexcept { return size_; }
    const std::vector<float>& window() const noexcept { return win_; }

    /// Windowed real FFT of `in` (first size() samples read, rest zero).
    /// Writes magnitudes into `mags` (size()/2 entries). Reuses scratch.
    void magnitudeSpectrum(const float* in, int inCount, float* mags) noexcept {
        for (int i = 0; i < size_; ++i) {
            const float v = i < inCount ? in[i] : 0.0f;
            frame_[(size_t)rev_[(size_t)i]] = { v * win_[(size_t)i], 0.0f };
        }
        for (int len = 2; len <= size_; len <<= 1) {
            const int half = len >> 1;
            const int step = size_ / len;
            for (int base = 0; base < size_; base += len) {
                for (int j = 0; j < half; ++j) {
                    const auto w = tw_[(size_t)(j * step)];
                    const auto a = frame_[(size_t)(base + j)];
                    const auto b = frame_[(size_t)(base + j + half)] * w;
                    frame_[(size_t)(base + j)]         = a + b;
                    frame_[(size_t)(base + j + half)]  = a - b;
                }
            }
        }
        for (int i = 0; i < size_ / 2; ++i)
            mags[i] = std::abs(frame_[(size_t)i]);
    }

private:
    int order_ { 0 }, size_ { 0 };
    std::vector<int> rev_;
    std::vector<std::complex<float>> tw_, frame_;
    std::vector<float> win_;
};

} // namespace onda
