#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "DelayLine.h"
#include "FXProcessor.h"

namespace onda::fx {

/// Schroeder-style stereo reverb: 8 parallel feedback combs + 4 series
/// allpasses per channel, mono pre-delay, stereo width on the wet sum.
/// Original implementation of the classic public-domain topology
/// (tuning scaled from the usual 44.1 kHz constants).
///
/// Params:  0 Size | 1 Decay | 2 Damping | 3 PreDelay ms | 4 Width
class ReverbFX : public FXBase<5> {
public:
    FXKind      kind() const noexcept override { return FXKind::Reverb; }
    const char* name() const noexcept override { return "Reverb"; }
    bool supportsSync() const noexcept override { return false; }
    bool hasTail()      const noexcept override { return true; }

    FXParamInfo paramInfo(int i) const noexcept override {
        static constexpr FXParamInfo k[] = {
            { "Size",     0.0f, 1.0f,   0.55f,  "",   0.0f },
            { "Decay",    0.0f, 1.0f,   0.60f,  "",   0.0f },
            { "Damping",  0.0f, 1.0f,   0.35f,  "",   0.0f },
            { "PreDelay", 0.0f, 100.0f, 10.0f,  "ms", 0.0f },
            { "Width",    0.0f, 1.0f,   0.90f,  "",   0.0f },
        };
        return k[i % 5];
    }

    void prepare(double sr, int /*maxBlock*/) override {
        rate_ = sr;
        prepareSmooth(sr, 20.0f);
        const double sc = sr / 44100.0;
        static const int kComb[8]  = { 1116, 1188, 1277, 1356,
                                       1422, 1491, 1557, 1617 };
        static const int kAllp[4]  = { 556, 441, 341, 225 };
        constexpr int kSpread = 23;
        for (int i = 0; i < 8; ++i) {
            combL_[i].prepare((int)(kComb[i] * sc) + 4);
            combR_[i].prepare((int)((kComb[i] + kSpread) * sc) + 4);
            combLenL_[i] = (float)(kComb[i] * sc);
            combLenR_[i] = (float)((kComb[i] + kSpread) * sc);
        }
        for (int i = 0; i < 4; ++i) {
            allpL_[i].prepare((int)(kAllp[i] * sc) + 4);
            allpR_[i].prepare((int)((kAllp[i] + kSpread) * sc) + 4);
            allpLenL_[i] = (float)(kAllp[i] * sc);
            allpLenR_[i] = (float)((kAllp[i] + kSpread) * sc);
        }
        pre_.prepare((int)(0.120 * sr) + 4);
        reset();
    }

    void reset() noexcept override {
        for (auto& c : combL_) c.reset();
        for (auto& c : combR_) c.reset();
        for (auto& a : allpL_) a.reset();
        for (auto& a : allpR_) a.reset();
        pre_.reset();
        for (float& s : store_) s = 0.0f;
    }

    /// §55 — Low trades density for CPU: 4 combs + 2 allpasses per
    /// side instead of 8 + 4. The wet sum is gain-compensated by tap
    /// count so the perceived level stays put. Read once per block;
    /// switching mid-render is click-safe (delay state persists).
    void setQuality(FXQuality q) noexcept override {
        quality_ = q;
        nCombs_ = q == FXQuality::Low ? 4 : 8;
        nAllp_  = q == FXQuality::Low ? 2 : 4;
    }
    FXQuality quality() const noexcept override { return quality_; }

    void process(float* l, float* r, int n) noexcept override {
        const int nC = nCombs_, nA = nAllp_;
        const float tapComp = 8.0f / (float)nC; // §55 level parity
        for (int i = 0; i < n; ++i) {
            // Comb read length follows Size (0.5x..1.25x of the tuning).
            const float scale = 0.5f + 0.75f * tick(0);
            const float decay = 0.70f + 0.28f * tick(1); // comb feedback
            const float damp  = tick(2) * 0.4f;          // lp in loop
            const float pd    = tick(3) * 0.001f * (float)rate_;
            const float width = tick(4);
            const float in = 0.5f * (l[i] + r[i]);
            // pre-delay line feeds the reverb network
            const float pd_in = pre_.read(pd);
            pre_.write(in);
            const float x = pd_in * 0.75f;

            float accL = 0.0f, accR = 0.0f;
            for (int c = 0; c < nC; ++c) {
                // L channel comb
                {
                    auto& cl = combL_[c];
                    const float out = cl.read(combLenL_[c] * scale);
                    const int si = c; // store index (L uses 0..7)
                    store_[si] = out * (1.0f - damp) + store_[si] * damp;
                    cl.write(x + store_[si] * decay);
                    accL += out;
                }
                // R channel comb
                {
                    auto& cr = combR_[c];
                    const float out = cr.read(combLenR_[c] * scale);
                    const int si = 8 + c;
                    store_[si] = out * (1.0f - damp) + store_[si] * damp;
                    cr.write(x + store_[si] * decay);
                    accR += out;
                }
            }
            for (int a = 0; a < nA; ++a) {
                accL = allpass(allpL_[a], allpLenL_[a], accL);
                accR = allpass(allpR_[a], allpLenR_[a], accR);
            }
            const float wl = accL * 0.08f * tapComp;
            const float wr = accR * 0.08f * tapComp;
            l[i] = wl * (0.5f + 0.5f * width) + wr * (0.5f - 0.5f * width);
            r[i] = wr * (0.5f + 0.5f * width) + wl * (0.5f - 0.5f * width);
        }
    }

private:
    /// Classic allpass section: out = -in + buf; buf = in + buf*g.
    static float allpass(DelayLine& dl, float len, float in) noexcept {
        const float b = dl.read(len);
        dl.write(in + b * 0.5f);
        return b - in;
    }

    double rate_ { 48000.0 };
    FXQuality quality_ { FXQuality::Medium };
    int nCombs_ { 8 }, nAllp_ { 4 };
    DelayLine combL_[8], combR_[8];
    DelayLine allpL_[4], allpR_[4];
    DelayLine pre_;
    float combLenL_[8] {}, combLenR_[8] {};
    float allpLenL_[4] {}, allpLenR_[4] {};
    float store_[16] {}; // per-comb damping one-pole state (L 0..7, R 8..15)
};

} // namespace onda::fx
