#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "../audio/AudioFormat.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Beat roll — a moving musical repeat (§22). Unlike BeatRepeat's
/// frozen latch, the roll keeps capturing while engaged: every slice
/// boundary the played window re-locks onto the freshest `size`
/// frames, so the roll rides the track's groove instead of freezing
/// one bar. Releasing returns to the live signal — the musical
/// position underneath was never lost.
///
/// Params:  0 Engage (momentary) | 1 SizeMult (x division)
///          2 Gate (mute fraction of each repeat cycle)
///          3 Tone Hz (low-pass on the repeated slice)
class BeatRollFX : public FXBase<4> {
public:
    static constexpr float kMaxSliceSec = 1.0f;
    static constexpr int   kFadeFrames  = 64;

    FXKind      kind() const noexcept override { return FXKind::BeatRoll; }
    const char* name() const noexcept override { return "BeatRoll"; }
    bool supportsSync() const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Engage",   0.0f,   1.0f,     0.0f,    "",   1.0f },
            { "SizeMult", 0.125f, 2.0f,     0.5f,    "x",  0.0f },
            { "Gate",     0.0f,   0.9f,     0.0f,    "",   0.0f },
            { "Tone",     300.0f, 18000.0f, 12000.0f,"Hz", 0.0f, FXParamType::Float, FXParamCurve::Log },
        };
        return k[i & 3];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 2.0f);
        const int cap = (int)(kMaxSliceSec * sr) + 4;
        capL_.assign((size_t)cap, 0.0f);
        capR_.assign((size_t)cap, 0.0f);
        lpL_.reset(); lpR_.reset();
        w_ = 0; sliceLen_ = 0; engaged_ = false;
    }

    void reset() noexcept override {
        std::fill(capL_.begin(), capL_.end(), 0.0f);
        std::fill(capR_.begin(), capR_.end(), 0.0f);
        lpL_.reset(); lpR_.reset();
        w_ = 0; sliceLen_ = 0; engaged_ = false;
    }

    void process(float* l, float* r, int n) noexcept override {
        const bool  eng  = target(0) > 0.5f;
        const float mult = target(1);
        const float gate = target(2);
        const float tone = target(3);

        if (eng && !engaged_) {
            // enter: window = the last `len` frames
            const double len = std::min(
                ctx().divisionFrames(rate_) * (double)mult,
                (double)capL_.size() - 4.0);
            sliceLen_ = (int)std::max(16.0, len);
            winStart_ = w_ - sliceLen_;
            if (winStart_ < 0) winStart_ += (int)capL_.size();
            sinceLatch_ = 0;
            fadePos_ = 0;
        }
        engaged_ = eng;

        lpL_.setCutoff(rate_, tone);
        lpR_.setCutoff(rate_, tone);

        for (int i = 0; i < n; ++i) {
            // capture never stops — the roll always reads fresh audio
            capL_[(size_t)w_] = l[i];
            capR_[(size_t)w_] = r[i];

            if (engaged_ && sliceLen_ > 0) {
                // roll refresh: each cycle re-locks the window onto
                // the slice that just finished capturing — the roll
                // rides the groove instead of freezing one bar
                if (sinceLatch_ >= sliceLen_) {
                    sinceLatch_ = 0;
                    fadePos_ = 0;
                    winStart_ = w_ - sliceLen_;
                    if (winStart_ < 0)
                        winStart_ += (int)capL_.size();
                }
                const int cap = (int)capL_.size();
                int rp = winStart_ + sinceLatch_;
                if (rp >= cap) rp -= cap;
                // gate = mute the tail fraction of each repeat cycle
                const float cyc = (float)sinceLatch_ / (float)sliceLen_;
                const float g   = cyc < (1.0f - gate) ? 1.0f : 0.0f;
                const float env = fadePos_ < kFadeFrames
                    ? (float)fadePos_ / (float)kFadeFrames : 1.0f;
                ++fadePos_;
                ++sinceLatch_;
                l[i] = lpL_.tick(capL_[(size_t)rp]) * env * g;
                r[i] = lpR_.tick(capR_[(size_t)rp]) * env * g;
            }
            if (++w_ >= (int)capL_.size()) w_ = 0;
        }
    }

private:
    double rate_ { 48000.0 };
    std::vector<float> capL_, capR_;
    OnePoleLp lpL_, lpR_;
    int  w_ { 0 };
    int  sliceLen_ { 0 };
    int  winStart_ { 0 };
    int  sinceLatch_ { 0 };
    int  fadePos_ { 0 };
    bool engaged_ { false };
};

} // namespace onda::fx
