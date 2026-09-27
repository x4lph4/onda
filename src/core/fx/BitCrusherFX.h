#pragma once

#include <algorithm>
#include <cmath>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Bit crusher — amplitude quantize (1..16 bits) plus sample-and-hold
/// decimation (1..60x). Post low-pass tames the foldover when wanted.
///
/// Params:  0 Bits | 1 Downsample | 2 PostLP Hz | 3 Dither (0/1)
class BitCrusherFX : public FXBase<4> {
public:
    FXKind      kind() const noexcept override { return FXKind::BitCrusher; }
    const char* name() const noexcept override { return "BitCrusher"; }
    bool supportsSync() const noexcept override { return false; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Bits",       1.0f,    16.0f,   8.0f,     "",   1.0f },
            { "Downsample", 1.0f,    60.0f,   1.0f,     "x",  1.0f },
            { "PostLP",     400.0f,  18000.0f,12000.0f, "Hz", 0.0f, FXParamType::Float, FXParamCurve::Log },
            { "Dither",     0.0f,    1.0f,    0.0f,     "",   1.0f },
        };
        return k[i & 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 4.0f);
        lpL_.reset(); lpR_.reset();
        hold_ = 0; holdL_ = holdR_ = 0.0f;
        dit_ = 0x1234567u;
    }

    void reset() noexcept override {
        lpL_.reset(); lpR_.reset();
        hold_ = 0; holdL_ = holdR_ = 0.0f;
    }

    void process(float* l, float* r, int n) noexcept override {
        const int   bits = (int)(target(0) + 0.5f);
        const int   dec  = (int)(target(1) + 0.5f);
        const bool  dith = target(3) > 0.5f;
        const float steps = (float)(1 << (bits - 1));  // ± peak levels
        const float qStep = 1.0f / steps;

        for (int i = 0; i < n; ++i) {
            lpL_.setCutoff(rate_, tick(2));
            lpR_.setCutoff(rate_, tick(2));
            if (hold_ <= 0) {
                holdL_ = crush(l[i], steps, qStep, dith ? dither() : 0.0f);
                holdR_ = crush(r[i], steps, qStep, dith ? dither() : 0.0f);
                hold_ = dec;
            }
            --hold_;
            l[i] = lpL_.tick(holdL_);
            r[i] = lpR_.tick(holdR_);
        }
    }

private:
    static float crush(float x, float steps, float qStep,
                       float d) noexcept {
        // quantize to bit grid; dither is one LSB of noise
        const float v = x + d * qStep;
        const float q = std::floor(v * steps + 0.5f) * qStep;
        return q > 1.0f ? 1.0f : (q < -1.0f ? -1.0f : q);
    }
    /// Tiny xorshift for TPDF-ish dither — deterministic, RT-safe.
    float dither() noexcept {
        dit_ ^= dit_ << 13; dit_ ^= dit_ >> 17; dit_ ^= dit_ << 5;
        return ((float)(dit_ & 0xffff) / 32768.0f - 1.0f) * 0.5f;
    }

    double    rate_ { 48000.0 };
    OnePoleLp lpL_, lpR_;
    int   hold_ { 0 };
    float holdL_ { 0 }, holdR_ { 0 };
    uint32_t dit_ { 1 };
};

} // namespace onda::fx
