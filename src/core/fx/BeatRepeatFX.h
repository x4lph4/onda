#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "../audio/AudioFormat.h"
#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Beat repeat — latches a slice of the incoming audio and loops it
/// while engaged (§21). Slice length = beat division x SizeMult,
/// resolved through the shared clock context (MasterClock/BeatGrid
/// upstream — never recomputed here).
///
/// While disengaged the effect passes audio through untouched and
/// keeps capturing, so the latch always grabs the freshest slice.
/// Slice start is ramped over ~2 ms — no clicks at the repeat edge.
///
/// Params:  0 Engage (momentary) | 1 SizeMult (0.25/0.5/1/2 x div)
///          2 Backstep (slice offset, 0..3 slices) | 3 Pitch (0.5..2)
class BeatRepeatFX : public FXBase<4> {
public:
    static constexpr float kMaxSliceSec = 2.0f;   // 4 bars @ 30 BPM min
    static constexpr int   kFadeFrames  = 96;     // ~2 ms @48k

    FXKind      kind() const noexcept override { return FXKind::BeatRepeat; }
    const char* name() const noexcept override { return "BeatRepeat"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Engage",   0.0f, 1.0f, 0.0f,  "",   1.0f },
            { "SizeMult", 0.25f,2.0f, 1.0f,  "x",  0.0f },
            { "Backstep", 0.0f, 3.0f, 0.0f,  "sl", 1.0f },
            { "Pitch",    0.5f, 2.0f, 1.0f,  "x",  0.0f },
        };
        return k[i & 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 2.0f);
        const int cap = (int)(kMaxSliceSec * sr) + 4;
        capL_.assign((size_t)cap, 0.0f);
        capR_.assign((size_t)cap, 0.0f);
        // slice buffer preallocated at worst case — latch() never
        // allocates on the RT path.
        slcL_.assign((size_t)cap, 0.0f);
        slcR_.assign((size_t)cap, 0.0f);
        w_ = 0; sliceLen_ = 0; engaged_ = false;
    }

    void reset() noexcept override {
        std::fill(capL_.begin(), capL_.end(), 0.0f);
        std::fill(capR_.begin(), capR_.end(), 0.0f);
        w_ = 0; sliceLen_ = 0; engaged_ = false;
    }

    void process(float* l, float* r, int n) noexcept override {
        // Engage is a stepped momentary — one decision per block.
        const bool  eng  = target(0) > 0.5f;
        const float mult = target(1);
        const int   back = (int)(target(2) + 0.5f);
        const float pit  = std::max(0.05f, value(3));

        if (eng && !engaged_) latch(mult, back);
        else if (!eng && engaged_) sliceLen_ = 0;
        engaged_ = eng;

        if (engaged_ && sliceLen_ > 0) {
            const float m = (float)sliceLen_;
            for (int i = 0; i < n; ++i) {
                const float env = fadePos_ < kFadeFrames
                    ? (float)fadePos_ / (float)kFadeFrames : 1.0f;
                ++fadePos_;
                l[i] = readAt(slcL_, rdL_, m) * env;
                r[i] = readAt(slcR_, rdR_, m) * env;
                rdL_ += pit; rdR_ += pit;
                while (rdL_ >= m) rdL_ -= m;
                while (rdR_ >= m) rdR_ -= m;
            }
        } else {
            for (int i = 0; i < n; ++i) {
                capL_[(size_t)w_] = l[i];
                capR_[(size_t)w_] = r[i];
                if (++w_ >= (int)capL_.size()) w_ = 0;
            }
            // passthrough: leave l/r untouched (wet = dry)
        }
    }

private:
    /// Copy the last `len` frames (offset back slices) into the
    /// preallocated slice buffer. No allocation — RT safe.
    void latch(float mult, int back) noexcept {
        const double len = std::min(
            ctx().divisionFrames(rate_) * (double)mult,
            (double)capL_.size() - 4.0);
        sliceLen_ = (int)std::max(16.0, len);
        const int cap = (int)capL_.size();
        int src = w_ - sliceLen_ * (back + 1);
        while (src < 0) src += cap;
        for (int i = 0; i < sliceLen_; ++i) {
            const int idx = src + i >= cap ? src + i - cap : src + i;
            slcL_[(size_t)i] = capL_[(size_t)idx];
            slcR_[(size_t)i] = capR_[(size_t)idx];
        }
        rdL_ = rdR_ = 0.0f;
        fadePos_ = 0;
    }

    static float readAt(const std::vector<float>& s, float pos,
                        float len) noexcept {
        const int i0 = (int)pos;
        const int i1 = i0 + 1 < (int)len ? i0 + 1 : 0;
        const float f = pos - (float)i0;
        return s[(size_t)i0] + f * (s[(size_t)i1] - s[(size_t)i0]);
    }

    double rate_ { 48000.0 };
    std::vector<float> capL_, capR_;   // rolling capture ring
    std::vector<float> slcL_, slcR_;   // preallocated slice buffer
    int   w_ { 0 };
    int   sliceLen_ { 0 };
    float rdL_ { 0 }, rdR_ { 0 };
    int   fadePos_ { 0 };
    bool  engaged_ { false };
};

} // namespace onda::fx
