#pragma once

#include <algorithm>
#include <cmath>

#include "FXProcessor.h"

namespace onda::fx {

/// Stereo-linked compressor — peak detector with independent
/// attack/release, soft-knee gain computer, makeup. Detector takes
/// max(|l|,|r|) so the stereo image never wanders.
///
/// Params:  0 Threshold dB | 1 Ratio | 2 Attack ms | 3 Release ms
///          4 Knee dB | 5 Makeup dB
class CompressorFX : public FXBase<6> {
public:
    FXKind      kind() const noexcept override { return FXKind::Compressor; }
    const char* name() const noexcept override { return "Compressor"; }
    bool supportsSync() const noexcept override { return false; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Threshold", -60.0f, 0.0f,   -18.0f,  "dB", 0.0f },
            { "Ratio",      1.0f,  20.0f,    4.0f,  ":1", 0.0f },
            { "Attack",     0.1f,  80.0f,   10.0f,  "ms", 0.0f },
            { "Release",   10.0f, 500.0f,  120.0f,  "ms", 0.0f },
            { "Knee",       0.0f,  12.0f,    6.0f,  "dB", 0.0f },
            { "Makeup",     0.0f,  24.0f,    0.0f,  "dB", 0.0f },
        };
        return k[i % 6];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 3.0f);   // param ramps; env uses att/rel
        envDb_ = -120.0f;
    }

    void reset() noexcept override { envDb_ = -120.0f; }

    void process(float* l, float* r, int n) noexcept override {
        const float invSr = 1.0f / (float)rate_;
        for (int i = 0; i < n; ++i) {
            const float thr   = tick(0);
            const float ratio = tick(1);
            const float attMs = tick(2);
            const float relMs = tick(3);
            const float knee  = tick(4);
            const float mkDb  = tick(5);

            // detector: stereo-linked peak → dB envelope
            const float det = std::max(std::fabs(l[i]), std::fabs(r[i]));
            const float detDb = det > 1e-6f
                ? 20.0f * std::log10(det) : -120.0f;
            // attack when rising, release when falling — in dB domain
            const float coef = detDb > envDb_
                ? std::exp(-invSr / (attMs * 0.001f))
                : std::exp(-invSr / (relMs * 0.001f));
            envDb_ = detDb + (envDb_ - detDb) * coef;

            // soft-knee gain computer
            const float over = envDb_ - thr;
            float gr = 0.0f;
            const float hk = knee * 0.5f;
            if (over > hk)            gr = over * (1.0f - 1.0f / ratio);
            else if (over > -hk && knee > 1e-3f)
                gr = (over + hk) * (over + hk) / (2.0f * knee)
                   * (1.0f - 1.0f / ratio);
            if (gr < 0.0f) gr = 0.0f;
            const float g = std::pow(10.0f, (mkDb - gr) / 20.0f);

            l[i] *= g;
            r[i] *= g;
        }
    }

    /// Detector envelope read-back for tests/meters (dBFS).
    float envDb() const noexcept { return envDb_; }

private:
    double rate_ { 48000.0 };
    float  envDb_ { -120.0f };
};

} // namespace onda::fx
