#pragma once

#include <cmath>
#include <numbers>

namespace onda {

inline constexpr double kPi = std::numbers::pi;

/// Single biquad (Direct Form I), RBJ Audio EQ Cookbook coefficients.
/// Coefficient computation uses transcendentals — call the factory
/// methods only when parameters change, never per-sample.
class Biquad {
public:
    void reset() noexcept { x1_ = x2_ = y1_ = y2_ = 0.0f; }

    void setCoefficients(float b0, float b1, float b2,
                         double a0, double a1, double a2) noexcept {
        const float ia0 = 1.0f / (float)a0;
        b0_ = b0 * ia0; b1_ = b1 * ia0; b2_ = b2 * ia0;
        a1_ = (float)a1 * ia0; a2_ = (float)a2 * ia0;
    }

    float process(float x) noexcept {
        const float y = b0_ * x + b1_ * x1_ + b2_ * x2_
                              - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = x;
        y2_ = y1_; y1_ = y;
        return y;
    }

    static Biquad lowShelf(double rate, double freq, double gainDb) {
        Biquad b; b.setLowShelf(rate, freq, gainDb); return b;
    }
    static Biquad highShelf(double rate, double freq, double gainDb) {
        Biquad b; b.setHighShelf(rate, freq, gainDb); return b;
    }
    static Biquad peaking(double rate, double freq, double gainDb, double q) {
        Biquad b; b.setPeaking(rate, freq, gainDb, q); return b;
    }
    static Biquad lowpass(double rate, double freq, double q) {
        Biquad b; b.setLowpass(rate, freq, q); return b;
    }
    static Biquad highpass(double rate, double freq, double q) {
        Biquad b; b.setHighpass(rate, freq, q); return b;
    }

    void setLowShelf(double rate, double f, double dB) noexcept {
        const double A = std::pow(10.0, dB / 40.0);
        const double w = 2.0 * kPi * f / rate, cs = std::cos(w);
        const double sn = std::sin(w), al = sn / 2.0 * std::sqrt((A + 1.0 / A) * 2.0 + 2.0);
        const double sq = 2.0 * std::sqrt(A) * al;
        setCoefficients(
            (float)(A * ((A + 1) - (A - 1) * cs + sq)),
            (float)(2 * A * ((A - 1) - (A + 1) * cs)),
            (float)(A * ((A + 1) - (A - 1) * cs - sq)),
            (A + 1) + (A - 1) * cs + sq,
            -2 * ((A - 1) + (A + 1) * cs),
            (A + 1) + (A - 1) * cs - sq);
    }

    void setHighShelf(double rate, double f, double dB) noexcept {
        const double A = std::pow(10.0, dB / 40.0);
        const double w = 2.0 * kPi * f / rate, cs = std::cos(w);
        const double sn = std::sin(w), al = sn / 2.0 * std::sqrt((A + 1.0 / A) * 2.0 + 2.0);
        const double sq = 2.0 * std::sqrt(A) * al;
        setCoefficients(
            (float)(A * ((A + 1) + (A - 1) * cs + sq)),
            (float)(-2 * A * ((A - 1) + (A + 1) * cs)),
            (float)(A * ((A + 1) + (A - 1) * cs - sq)),
            (A + 1) - (A - 1) * cs + sq,
            2 * ((A - 1) - (A + 1) * cs),
            (A + 1) - (A - 1) * cs - sq);
    }

    void setPeaking(double rate, double f, double dB, double q) noexcept {
        const double A = std::pow(10.0, dB / 40.0);
        const double w = 2.0 * kPi * f / rate, cs = std::cos(w);
        const double al = std::sin(w) / (2.0 * q);
        setCoefficients(
            (float)(1 + al * A), (float)(-2 * cs), (float)(1 - al * A),
            1 + al / A, -2 * cs, 1 - al / A);
    }

    void setLowpass(double rate, double f, double q) noexcept {
        const double w = 2.0 * kPi * f / rate, cs = std::cos(w);
        const double al = std::sin(w) / (2.0 * q);
        setCoefficients(
            (float)((1 - cs) * 0.5), (float)(1 - cs), (float)((1 - cs) * 0.5),
            1 + al, -2 * cs, 1 - al);
    }

    void setHighpass(double rate, double f, double q) noexcept {
        const double w = 2.0 * kPi * f / rate, cs = std::cos(w);
        const double al = std::sin(w) / (2.0 * q);
        setCoefficients(
            (float)((1 + cs) * 0.5), (float)(-(1 + cs)), (float)((1 + cs) * 0.5),
            1 + al, -2 * cs, 1 - al);
    }

private:
    float b0_ { 1.0f }, b1_ { 0.0f }, b2_ { 0.0f };
    float a1_ { 0.0f }, a2_ { 0.0f };
    float x1_ { 0.0f }, x2_ { 0.0f }, y1_ { 0.0f }, y2_ { 0.0f };
};

} // namespace onda
