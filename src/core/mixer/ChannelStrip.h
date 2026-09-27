#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "../audio/AudioFormat.h"
#include "../model/DeckStats.h"
#include "CrossfaderEngine.h"
#include "EqProcessor.h"
#include "FilterProcessor.h"
#include "GainSmoother.h"
#include "MeteringEngine.h"
#include "MixerTypes.h"
#include "PanProcessor.h"

namespace onda::mixer {

/// One mixer channel. Signal path per block:
///
///   source (rendered into inL/inR by the owning engine)
///     -> input trim (smoothed)
///     -> EqProcessor (3-band today, 4-capable)
///     -> FilterProcessor (LP/HP/position knob)
///     -> [pre-fader meter + PFL cue tap]
///     -> channel fader (curve, smoothed) * mute/solo * instant-cut
///     -> pan (balance law, smoothed)
///     -> [post meter]
///     -> crossfader assignment gain (A / B / Thru)
///     -> summed into the master bus
///
/// Every user-controlled parameter is smoothed; nothing allocates or
/// locks inside process(). Sources other than decks (sampler, external
/// in, FX return) can feed the same strip later by writing into
/// inL/inR before process().
class ChannelStrip {
public:
    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate;
        inL_.assign(AudioFormat::kMaxBlockFrames, 0.0f);
        inR_.assign(AudioFormat::kMaxBlockFrames, 0.0f);
        smGain_.prepare(sampleRate, 12.0f);
        smGain_.snap(gain_);
        smFader_.prepare(sampleRate, 5.0f);
        smFader_.snap(curvedFaderGain());
        smMute_.prepare(sampleRate, 4.0f);
        smMute_.snap(1.0f);
        smCut_.prepare(sampleRate, 0.4f); // ~instant but clickless
        smCut_.snap(1.0f);
        smPhase_.prepare(sampleRate, 0.5f);
        smPhase_.snap(1.0f);
        eq_.prepare(sampleRate);
        filter_.prepare(sampleRate);
        metersEngine_.prepare(sampleRate);
        postL_.assign(AudioFormat::kMaxBlockFrames, 0.0f);
        postR_.assign(AudioFormat::kMaxBlockFrames, 0.0f);
        smSend_[0].prepare(sampleRate, 5.0f);
        smSend_[1].prepare(sampleRate, 5.0f);
        smSend_[0].snap(0.0f);
        smSend_[1].snap(0.0f);
        pan_.prepare(sampleRate);
    }

    // ---- source input (written by the rendering engine each block) ----
    float* inL() noexcept { return inL_.data(); }
    float* inR() noexcept { return inR_.data(); }
    /// §5 — the strip's input as a bus handle: producers fill it per
    /// block (frames = block size). Non-owning; lifetime = the strip.
    AudioBus inBus(int frames) noexcept {
        return { inL_.data(), inR_.data(), frames };
    }

    // ---- RT parameters (called from the engine's command dispatch) ----
    void setInputGain(float g) noexcept { smGain_.setTarget(g); }
    float inputGain() const noexcept { return smGain_.target(); }
    EqProcessor&     eq() noexcept     { return eq_; }
    const EqProcessor& eq() const noexcept { return eq_; }
    FilterProcessor& filter() noexcept { return filter_; }
    const FilterProcessor& filter() const noexcept { return filter_; }

    void setFader(float v) noexcept {
        faderPos_ = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        smFader_.setTarget(curvedFaderGain());
    }
    float fader() const noexcept { return faderPos_; }
    void setFaderCurve(FaderCurve c) noexcept {
        curve_ = c;
        smFader_.setTarget(curvedFaderGain());
    }
    FaderCurve faderCurve() const noexcept { return curve_; }
    void setCustomFaderCurve(std::function<float(float)> f) noexcept {
        customCurve_ = std::move(f);
        if (curve_ == FaderCurve::Custom) smFader_.setTarget(curvedFaderGain());
    }

    void setMute(bool m) noexcept      { mute_ = m; smMute_.setTarget(m ? 0.0f : 1.0f); }
    bool muted() const noexcept        { return mute_; }
    void setSolo(bool s) noexcept      { solo_ = s; }
    bool soloed() const noexcept       { return solo_; }
    void setInstantCut(bool c) noexcept { smCut_.setTarget(c ? 0.0f : 1.0f); }

    /// Polarity flip — smoothed through zero so the inversion is
    /// clickless (gain dips for ~0.5 ms rather than stepping).
    void setPhaseInvert(bool p) noexcept { phase_ = p; smPhase_.setTarget(p ? -1.0f : 1.0f); }
    bool phaseInverted() const noexcept  { return phase_; }

    void setCue(bool on) noexcept      { cue_ = on; }
    bool cueOn() const noexcept        { return cue_; }
    void setXfAssign(XfAssign a) noexcept { assign_ = a; }
    XfAssign xfAssign() const noexcept    { return assign_; }

    /// §22 — channel pan, -1 (L) .. +1 (R). Applied to the post-fader
    /// signal so the pre-fader meter and PFL cue stay honest, while FX
    /// sends and the master sum carry the panned image.
    void setPan(float p) noexcept  { pan_.setPosition(p); }
    float pan() const noexcept     { return pan_.position(); }

    /// FX send taps (post-fader, pre-crossfader): bus 0/1 levels 0..1.
    void setSendLevel(int bus, float v) noexcept {
        if (bus < 0 || bus > 1) return;
        sendLv_[bus] = v;
        smSend_[bus].setTarget(v);
    }
    float sendLevel(int bus) const noexcept {
        return bus >= 0 && bus <= 1 ? sendLv_[bus] : 0.0f;
    }
    /// Advance the send-level smoother across a finished block (called
    /// once per block by the engine after process()).
    float sendGain(int bus, int n) noexcept {
        float g = smSend_[bus].current();
        for (int i = 0; i < n; ++i) g = smSend_[bus].tick();
        return smSend_[bus].current();
    }

    /// Post-fader channel signal (fader x mute x solo x instant-cut,
    /// BEFORE crossfader-assignment gain). Written every block by
    /// process(); FX sends and future recording taps read it here.
    const float* postL() const noexcept { return postL_.data(); }
    const float* postR() const noexcept { return postR_.data(); }

    // ---- metering / stats ----
    const ChannelMeters& meters() const noexcept { return meters_; }
    void bindStats(DeckStats* s) noexcept { dstats_ = s; }

    /// Effective post gain last computed (for tests/diagnostics).
    float postFaderGain() const noexcept { return lastPostGain_; }

    /// RT. Consumes inL/inR (prepared by the source), taps the cue bus,
    /// and adds this channel's post-fader/crossfader contribution to
    /// mixL/mixR. `anySolo` = another strip is soloed.
    void process(int n, float* mixL, float* mixR,
                 float* cueL, float* cueR,
                 const CrossfaderEngine& xf, bool anySolo) noexcept {
        float* l = inL_.data();
        float* r = inR_.data();

        // 1) trim + polarity
        {
            float g = smGain_.current();
            for (int i = 0; i < n; ++i) {
                g = smGain_.tick();
                const float s = g * smPhase_.tick();
                l[i] *= s; r[i] *= s;
            }
        }

        // 2) EQ -> filter
        eq_.tick();
        eq_.process(l, r, n);
        filter_.process(l, r, n);

        // 3) pre-fader meter + optional DeckStats compat write-back
        metersEngine_.update(l, r, n, meters_);
        if (dstats_ != nullptr) {
            dstats_->levelL.store(meters_.peakL.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
            dstats_->levelR.store(meters_.peakR.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        }

        // 4) PFL cue tap (pre-fader — hears even with fader down)
        if (cue_ && cueL != nullptr && cueR != nullptr)
            for (int i = 0; i < n; ++i) { cueL[i] += l[i]; cueR[i] += r[i]; }

        // 5) fader/mute/cut/solo -> pan -> post tap -> xf assign -> sum
        const float postG = (anySolo && !solo_) ? 0.0f : 1.0f;
        const float xfG   = xf.gainFor(assign_);
        float panL = 1.0f, panR = 1.0f;
        pan_.tick(n, panL, panR);
        float post = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float g = smFader_.tick() * smMute_.tick()
                          * smCut_.tick() * postG;
            post = g;
            postL_[i] = l[i] * g * panL;
            postR_[i] = r[i] * g * panR;
            if (mixL != nullptr && mixR != nullptr) {
                mixL[i] += postL_[i] * xfG;
                mixR[i] += postR_[i] * xfG;
            }
        }
        lastPostGain_ = post;
    }

    /// RT: channel with a Silence/no-source mapping — keeps the post
    /// taps quiet so send buses and diagnostics never read stale audio,
    /// and decays the meters. ~free (two memsets).
    void silence(int n) noexcept {
        std::fill_n(postL_.data(), n, 0.0f);
        std::fill_n(postR_.data(), n, 0.0f);
        float z = 0.0f;
        metersEngine_.update(&z, &z, 1, meters_);
        lastPostGain_ = 0.0f;
    }

private:
    float curvedFaderGain() const noexcept {
        const float p = faderPos_;
        switch (curve_) {
            case FaderCurve::Linear:     return p;
            case FaderCurve::AudioTaper: return p * p;
            case FaderCurve::Custom:
                if (customCurve_) {
                    const float v = customCurve_(p);
                    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                }
                return p;
        }
        return p;
    }

    double rate_ { 48000.0 };

    std::vector<float> inL_, inR_;
    std::vector<float> postL_, postR_;   // post-fader tap (sends/record)

    float sendLv_[2] { 0.0f, 0.0f };
    GainSmoother smSend_[2];

    float gain_    { 1.0f };
    float faderPos_{ 1.0f };
    FaderCurve curve_ { FaderCurve::Linear };
    std::function<float(float)> customCurve_;
    bool mute_ { false };
    bool solo_ { false };
    bool cue_  { false };
    bool phase_ { false };
    XfAssign assign_ { XfAssign::Thru };

    GainSmoother smGain_, smFader_, smMute_, smCut_, smPhase_;
    EqProcessor     eq_;
    FilterProcessor filter_;
    PanProcessor    pan_;
    MeteringEngine  metersEngine_;
    ChannelMeters   meters_;

    DeckStats* dstats_ { nullptr };
    float lastPostGain_ { 0.0f };
};

} // namespace onda::mixer
