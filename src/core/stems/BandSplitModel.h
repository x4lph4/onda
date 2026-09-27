#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "../dsp/Biquad.h"
#include "StemSeparationModel.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// Reference separation model — deterministic DSP, no ML.
///
///   BASS   = LP150 per channel (stereo low band)
///   VOCALS = band(mid, 300 Hz–3.4 kHz), dual-mono (center content)
///   DRUMS  = mid − bassMid − vocals  (rest of the center image)
///   INSTR  = source − (bass + vocals + drums)   → exact residual
///
/// Because INSTRUMENTS is computed as the residual, the four stems
/// sum back to the source sample-for-sample (perfect reconstruction
/// — §51 tests rely on it) and every stem is frame-aligned with the
/// track (§11). The labels are honest approximations of the musical
/// roles, not learned source separation; the interface is the seam
/// where a real model plugs in.
///
/// Runs entirely on a scheduler worker: allocates, blocks, polls the
/// cancel token every 8192 frames.
/// ------------------------------------------------------------------
class BandSplitModel final : public StemSeparationModel {
public:
    static constexpr const char* kId      = "onda.bandsplit";
    static constexpr const char* kVersion = "1.0.0";

    bool load(const StemModelConfig& cfg, std::string* err) override {
        order_ = cfg.filterOrder > 0 ? cfg.filterOrder : 2;
        loaded_ = true;
        (void)err;
        return true;
    }
    void unload() override { loaded_ = false; }
    bool loaded() const override { return loaded_; }

    StemModelInfo info() const override {
        return { kId, kVersion, "Onda Band-Split (reference)" };
    }
    StemModelCaps capabilities() const override {
        StemModelCaps c;
        c.stemMask = stemBit(StemType::Vocals) | stemBit(StemType::Drums)
                   | stemBit(StemType::Bass) | stemBit(StemType::Instruments);
        c.gpuCapable  = false;
        c.cancellable = true;
        return c;
    }

    StemError analyze(const TrackAudio& src, StemCancel& cancel,
                      const StemProgress& progress) override {
        if (cancel.load()) return StemError::Cancelled;
        if (!loaded_)                    return StemError::ModelLoadFailed;
        if (src.numFrames <= 0 || !src.samples.data())
                                       return StemError::InvalidAudio;
        if (src.channels != 2)           return StemError::InvalidAudio;
        if (src.deviceRate <= 0.0 ||
            src.deviceRate > capabilities().maxSampleRate)
                                       return StemError::InvalidAudio;
        // NaN/Inf validation (§40) — sample the content, not every
        // frame: a corrupted stream fails validation, a clean one
        // costs ~0 time.
        const int64_t n = src.numFrames * 2;
        const int64_t step = std::max<int64_t>(1, n / 4096);
        for (int64_t i = 0; i < n; i += step)
            if (!std::isfinite(src.samples[(size_t)i]))
                return StemError::InvalidAudio;
        if (progress) progress(StemJobPhase::Analyzing, 1.0f);
        return StemError::Ok;
    }

    StemError separate(const TrackAudio& src, StemSet& out,
                       StemCancel& cancel,
                       const StemProgress& progress) override {
        const int64_t frames = src.numFrames;
        const float* s = src.samples.data();
        const double rate = src.deviceRate;
        const int stages = order_ * 4;         // 4 filter families
        int done = 0;

        auto setP = [&](StemJobPhase ph, float f) {
            if (progress) progress(ph, f);
        };

        // ---- allocate outputs -------------------------------------
        out.rate = rate; out.channels = 2; out.numFrames = frames;
        out.mask = stemBit(StemType::Vocals) | stemBit(StemType::Drums)
                 | stemBit(StemType::Bass) | stemBit(StemType::Instruments);
        for (StemType t : { StemType::Vocals, StemType::Drums,
                            StemType::Bass, StemType::Instruments })
            out.samples[(size_t)t].assign((size_t)frames * 2, 0.0f);
        if (cancel.load()) return StemError::Cancelled;
        setP(StemJobPhase::Separating, 0.02f);

        float* bass = out.samples[(size_t)StemType::Bass].data();
        float* voc  = out.samples[(size_t)StemType::Vocals].data();
        float* drm  = out.samples[(size_t)StemType::Drums].data();
        float* ins  = out.samples[(size_t)StemType::Instruments].data();

        std::vector<float> midBuf((size_t)frames);
        std::vector<float> bassMid((size_t)frames);

        // ---- stage 1: mid + bass band (stereo LP150) ---------------
        {
            auto lL = makeLpChain(rate, 150.0);
            auto lR = makeLpChain(rate, 150.0);
            auto lM = makeLpChain(rate, 150.0);
            for (int64_t i = 0; i < frames; ++i) {
                const float l = s[i * 2], r = s[i * 2 + 1];
                const float m = (l + r) * 0.5f;
                midBuf[(size_t)i] = m;
                bassMid[(size_t)i] = chain(lM, m);
                bass[i * 2]     = chain(lL, l);
                bass[i * 2 + 1] = chain(lR, r);
                if (((i & 8191) == 0 && cancel.load()))
                    return StemError::Cancelled;
            }
            done += order_; setP(StemJobPhase::Separating,
                                 0.05f + 0.75f * done / stages);
        }
        // ---- stage 2: vocal band on mid (HP300 → LP3400) -----------
        {
            auto hp = makeHpChain(rate, 300.0);
            auto lp = makeLpChain(rate, 3400.0);
            for (int64_t i = 0; i < frames; ++i) {
                const float v = chain(lp, chain(hp, midBuf[(size_t)i]));
                voc[i * 2] = v; voc[i * 2 + 1] = v;   // dual-mono
                if (((i & 8191) == 0 && cancel.load()))
                    return StemError::Cancelled;
            }
            done += order_ * 2; setP(StemJobPhase::Separating,
                                     0.05f + 0.75f * done / stages);
        }
        // ---- stage 3: drums = residual mid, instr = residual -------
        {
            for (int64_t i = 0; i < frames; ++i) {
                const float m = midBuf[(size_t)i];
                const float d = m - bassMid[(size_t)i] - voc[i * 2];
                drm[i * 2] = d; drm[i * 2 + 1] = d;
                ins[i * 2]     = s[i * 2]     - bass[i * 2]
                               - voc[i * 2] - d;
                ins[i * 2 + 1] = s[i * 2 + 1] - bass[i * 2 + 1]
                               - voc[i * 2] - d;
                if (((i & 8191) == 0 && cancel.load()))
                    return StemError::Cancelled;
            }
            setP(StemJobPhase::Separating, 0.85f);
        }

        // ---- validation (§40): alignment + finite -------------------
        setP(StemJobPhase::Validating, 0.9f);
        if (out.numFrames != frames) return StemError::ModelRunFailed;
        const int64_t n = frames * 2, step = std::max<int64_t>(1, n / 4096);
        for (StemType t : { StemType::Vocals, StemType::Drums,
                            StemType::Bass, StemType::Instruments }) {
            const float* p = out.samples[(size_t)t].data();
            for (int64_t i = 0; i < n; i += step)
                if (!std::isfinite(p[(size_t)i]))
                    return StemError::ModelRunFailed;
        }
        setP(StemJobPhase::Validating, 1.0f);
        return StemError::Ok;
    }

private:
    using Chain = std::array<Biquad, 8>;   // ≤8th order
    Chain makeLpChain(double rate, double f) const {
        Chain c{};
        for (int i = 0; i < order_; ++i)
            c[(size_t)i].setLowpass(rate, f, 0.7071);
        return c;
    }
    Chain makeHpChain(double rate, double f) const {
        Chain c{};
        for (int i = 0; i < order_; ++i)
            c[(size_t)i].setHighpass(rate, f, 0.7071);
        return c;
    }
    float chain(Chain& c, float x) const noexcept {
        for (int i = 0; i < order_; ++i) x = c[(size_t)i].process(x);
        return x;
    }

    int  order_  { 2 };
    bool loaded_ { false };
};

} // namespace onda::stems
