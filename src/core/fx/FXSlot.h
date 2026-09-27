#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "../audio/AudioFormat.h"
#include "../rt/SpscRingBuffer.h"
#include "FXFactory.h"
#include "FXProcessor.h"
#include "FXTypes.h"

namespace onda::fx {

/// One effect position inside a rack. Holds at most one FXProcessor;
/// chains are expressed with several slots in a row.
///
/// Effect swaps are allocation-free on the audio thread: loadEffect()
/// builds + prepares the processor on the *calling* thread and parks it
/// in `pending_`; the RT thread adopts it at the top of the next block
/// and retires the old one into `trash_`, which collectTrash() frees on
/// a control thread. Calling loadEffect() again before adoption drops
/// the stale pending object on the caller's thread — always safe.
///
/// Signal path per block (when enabled):
///   in * inGain -> FX (wet) -> outGain
///   out = dry*(1-w) + wet*w   where w = mix * bypassSmoother
/// Bypass/enabled changes crossfade over ~10 ms: no clicks, and the
/// same ramp dips briefly when a new effect is adopted mid-playback.
///
/// Safety: output is sanitized every block (NaN/Inf -> 0, clamp +-8);
/// sustained bad output auto-resets the processor after 4 bad blocks.
class FXSlot {
public:
    static constexpr float kClamp   = 8.0f;
    static constexpr int   kTailSec = 8; // max natural tail drain

    // ---------------- non-RT (control thread) ----------------
    void prepare(double sampleRate) {
        rate_ = sampleRate;
        smMix_.prepare(sampleRate, 10.0f); smMix_.snap(mix_);
        smIn_ .prepare(sampleRate, 8.0f);  smIn_.snap(inGain_);
        smOut_.prepare(sampleRate, 8.0f);  smOut_.snap(outGain_);
        smEn_ .prepare(sampleRate, 10.0f); smEn_.snap(0.0f);
        smEnDry_.prepare(sampleRate, 10.0f); smEnDry_.snap(0.0f);
        dryL_.assign(AudioFormat::kMaxBlockFrames, 0.0f);
        dryR_.assign(AudioFormat::kMaxBlockFrames, 0.0f);
    }

    /// Build + park a new effect (ALLOCATES — non-RT only).
    void loadEffect(FXKind kind) {
        std::unique_ptr<FXProcessor> p = createFX(kind);
        if (p != nullptr) {
            p->prepare(rate_, (int)AudioFormat::kMaxBlockFrames);
        }
        // Drop any stale pending object on this (control) thread.
        delete pending_.exchange(p.release(), std::memory_order_acq_rel);
    }

    /// Free processors retired by the RT thread. Non-RT only; call
    /// after loadEffect() and periodically from the app's own timer.
    void collectTrash() noexcept {
        delete trash_.exchange(nullptr, std::memory_order_acq_rel);
    }

    void bindEvents(rt::SpscRingBuffer<FXEvent, 256>* q,
                    int target, int slot) noexcept {
        events_ = q; target_ = target; slot_ = slot;
    }

    // ---------------- queries ----------------
    FXKind activeKind() const noexcept {
        return fx_ != nullptr ? fx_->kind() : FXKind::None;
    }
    FXProcessor* effect() const noexcept { return fx_; }
    bool  enabled()  const noexcept { return enabled_; }
    /// The slot still consumes source audio (enabled, draining a tail
    /// or mid crossfade) — used by stem inserts to decide whether a
    /// stem renders through its FX stage or straight into the mix.
    bool  takesInput() const noexcept {
        return enabled_ || busy_ || tailing_; }
    /// §42 — currently draining a natural tail after being disabled.
    bool  tailing()  const noexcept { return tailing_; }
    float mix()      const noexcept { return mix_; }
    int   syncMode() const noexcept { return syncMode_; }
    int   division() const noexcept { return division_; }
    int   wantType() const noexcept { return wantType_; }
    int   latencySamples() const noexcept {
        return fx_ != nullptr ? fx_->latencySamples() : 0;
    }

    /// True while the slot is doing audible work — used by the rack to
    /// skip hibernated slots cheaply.
    bool busy() const noexcept { return busy_; }
    /// A parked effect is waiting for RT adoption — must keep process()
    /// being called so it can be picked up.
    bool hasPending() const noexcept {
        return pending_.load(std::memory_order_acquire) != nullptr;
    }

    // meters (atomics — read anywhere)
    std::atomic<float> inPeak  { 0.0f };
    std::atomic<float> outPeak { 0.0f };
    std::atomic<float> cpuUs   { 0.0f };
    std::atomic<uint64_t> sanitized { 0 };
    /// Times the processor was auto-reset for sustained bad output.
    std::atomic<uint64_t> faults { 0 };

    // ---------------- RT ----------------
    void setParam(FXSlotKind k, float v) noexcept {
        switch (k) {
            case FXSlotKind::Type:
                wantType_ = (int)v;
                if ((fx_ == nullptr || (int)fx_->kind() != wantType_) &&
                    (pendingFxKind() != wantType_))
                    push(FXEventType::TypeRequested, v);
                break;
            case FXSlotKind::Enabled: {
                const bool on = v > 0.5f;
                if (on != enabled_) {
                    enabled_ = on;
                    if (on) {
                        tailing_ = false; tailDone_ = false;
                        tailSamples_ = 0;
                        smEn_.setTarget(1.0f);
                        smEnDry_.setTarget(1.0f);
                    } else {
                        // §42: tail-capable effects drain naturally —
                        // keep the wet gate open, feed silence, and let
                        // the tail decay (bounded by kTailCap). The dry
                        // path returns to unity over the same ramp so
                        // the bypass is fully transparent underneath.
                        tailing_ = fx_ != nullptr && fx_->hasTail()
                                   && !tailDone_;
                        tailSamples_ = 0;
                        smEn_.setTarget(tailing_ ? 1.0f : 0.0f);
                        smEnDry_.setTarget(0.0f);
                    }
                    push(on ? FXEventType::Enabled : FXEventType::Disabled, v);
                }
                break;
            }
            case FXSlotKind::Mix:     mix_ = v;     smMix_.setTarget(v); push(FXEventType::ParamChanged, v); break;
            case FXSlotKind::InGain:  inGain_ = v;  smIn_.setTarget(v);  push(FXEventType::ParamChanged, v); break;
            case FXSlotKind::OutGain: outGain_ = v; smOut_.setTarget(v); push(FXEventType::ParamChanged, v); break;
            case FXSlotKind::SyncMode:
                if ((int)v != syncMode_) { syncMode_ = (int)v; push(FXEventType::SyncChanged, v); }
                break;
            case FXSlotKind::Division:
                if ((int)v != division_) { division_ = (int)v; push(FXEventType::DivisionChanged, v); }
                break;
            default: {
                const int p = (int)k - (int)FXSlotKind::P1;
                if (p < 0 || p >= kMaxFxParams) break;
                // Stash the normalized value: if a new processor is
                // parked but not yet adopted, fx_ is either null or the
                // OLD kind — the write must not be lost (a preset
                // applied right after a type swap depends on it). The
                // stash is replayed onto the adopted processor.
                pendingP_[(size_t)p] = v;
                pendingMask_ |= (1u << p);
                if (fx_ != nullptr && p < fx_->paramCount()) {
                    fx_->setParamNormalized(p, v);
                    push(FXEventType::ParamChanged, v);
                }
                break;
            }
        }
    }

    /// RT. Serial in-place processing. `ctx` is the rack-level sync
    /// context; the slot stamps its own sync mode/division onto it.
    /// `masterCtx` (optional) supplies the sync-master tempo when the
    /// slot's SyncMode selects MASTER (2) as its tempo source (§22).
    void process(float* l, float* r, int n, SyncContext ctx,
                 const SyncContext* masterCtx = nullptr) noexcept {
        adoptPending();

        if (fx_ == nullptr) { busy_ = false; return; }
        // hibernated: bypassed, no tail draining, crossfade settled
        if (!enabled_ && !tailing_ && smEn_.current() < 1e-4f) {
            busy_ = false;
            return;
        }

        // §22 — tempo source: MANUAL(0)=free, DECK(1)=target tempo,
        // MASTER(2)=the sync-master deck's clock.
        if (syncMode_ == 2 && masterCtx != nullptr) ctx = *masterCtx;
        ctx.synced   = syncMode_ != 0 ? 1 : 0;
        ctx.division = division_;
        fx_->setSyncContext(ctx);

        float inPk = 0.0f;
        int badIn = 0;
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(l[i])) { l[i] = 0.0f; ++badIn; }
            else if (l[i] > kClamp)  { l[i] = kClamp;  ++badIn; }
            else if (l[i] < -kClamp) { l[i] = -kClamp; ++badIn; }
            if (!std::isfinite(r[i])) { r[i] = 0.0f; ++badIn; }
            else if (r[i] > kClamp)  { r[i] = kClamp;  ++badIn; }
            else if (r[i] < -kClamp) { r[i] = -kClamp; ++badIn; }
            inPk = std::max(inPk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        }
        inPeak.store(inPk, std::memory_order_relaxed);

        // dry copy + input gain into the wet path
        float* dl = dryL_.data();
        float* dr = dryR_.data();
        {
            float g = smIn_.current();
            for (int i = 0; i < n; ++i) {
                g = smIn_.tick();
                dl[i] = l[i]; dr[i] = r[i];
                // tailing: the source keeps flowing into the dry copy
                // but the effect is fed silence so its tail decays
                l[i] = tailing_ ? 0.0f : l[i] * g;
                r[i] = tailing_ ? 0.0f : r[i] * g;
            }
        }

        const auto t0 = cpuMeter_.load(std::memory_order_relaxed) > 0.5f
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
        fx_->process(l, r, n);
        if (cpuMeter_.load(std::memory_order_relaxed) > 0.5f) {
            const auto us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - t0).count();
            cpuUs.store((float)us, std::memory_order_relaxed);
        }

        // sanitize: NaN/Inf -> 0, hard-clamp; sustained badness resets.
        // The wet peak here (pre dry/wet crossfade) drives tail-drain
        // detection — the dry signal must not count as tail energy.
        int bad = 0;
        float outPk = 0.0f, wetPk = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(l[i])) { l[i] = 0.0f; ++bad; }
            if (!std::isfinite(r[i])) { r[i] = 0.0f; ++bad; }
            if (l[i] > kClamp)  { l[i] = kClamp;  ++bad; }
            if (l[i] < -kClamp) { l[i] = -kClamp; ++bad; }
            if (r[i] > kClamp)  { r[i] = kClamp;  ++bad; }
            if (r[i] < -kClamp) { r[i] = -kClamp; ++bad; }
            wetPk = std::max(wetPk,
                 std::max(std::fabs(l[i]), std::fabs(r[i])));
        }
        if (tailing_) {
            tailSamples_ += (int64_t)n;
            if (wetPk < 1e-4f ||
                tailSamples_ > (int64_t)(rate_ * kTailSec)) {
                tailing_ = false;
                tailDone_ = true;   // next off goes straight to bypass
                smEn_.setTarget(0.0f);
            }
        }
        if (bad > 0 || badIn > 0) {
            sanitized.fetch_add((uint64_t)(bad + badIn), std::memory_order_relaxed);
            if (++badBlocks_ >= 4) {
                fx_->reset(); badBlocks_ = 0;
                faults.fetch_add(1, std::memory_order_relaxed);
                push(FXEventType::Fault, (float)faults.load(
                    std::memory_order_relaxed));
            }
        } else {
            badBlocks_ = 0;
        }

        // dry/wet crossfade with separate dry gate:
        //   enabled:   out = dry*(1-mix) + wet*mix
        //   tailing:   out = dry*1      + wet*mix   (dry returns to unity,
        //                                        the tail rings on top)
        //   bypassed:  out = dry*1      + wet*0
        {
            float m = smMix_.current(), e = smEn_.current();
            float ed = smEnDry_.current(), g = smOut_.current();
            for (int i = 0; i < n; ++i) {
                m = smMix_.tick(); e = smEn_.tick();
                ed = smEnDry_.tick(); g = smOut_.tick();
                l[i] = dl[i] * (1.0f - m * ed) + l[i] * g * m * e;
                r[i] = dr[i] * (1.0f - m * ed) + r[i] * g * m * e;
                outPk = std::max(outPk,
                     std::max(std::fabs(l[i]), std::fabs(r[i])));
            }
        }
        outPeak.store(outPk, std::memory_order_relaxed);
        busy_ = enabled_ || tailing_ || smEn_.current() > 1e-4f;
    }

    /// CPU metering on/off (read once per block).
    void setCpuMetering(bool on) noexcept {
        cpuMeter_.store(on ? 1.0f : 0.0f, std::memory_order_relaxed);
    }

    /// §55 — quality tier (§55 is a configuration, not a realtime
    /// param). Control thread: applies to the live processor, the
    /// parked pending one, and is replayed onto every future adoption.
    void setQuality(FXQuality q) noexcept {
        quality_.store((uint8_t)q, std::memory_order_relaxed);
        if (auto* p = pending_.load(std::memory_order_acquire))
            p->setQuality(q);
        if (fx_ != nullptr) fx_->setQuality(q);
    }
    FXQuality quality() const noexcept {
        return (FXQuality)quality_.load(std::memory_order_relaxed);
    }

private:
    void adoptPending() noexcept {
        if (auto* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            if (fx_ != nullptr)
                trash_.store(fx_, std::memory_order_release);
            fx_ = p;
            fx_->setQuality(
                (FXQuality)quality_.load(std::memory_order_relaxed));
            fx_->reset();
            // Replay param writes that arrived while the object was
            // parked (type swap + preset applied in the same gesture).
            if (pendingMask_ != 0) {
                const int n = std::min(fx_->paramCount(), kMaxFxParams);
                for (int i = 0; i < n; ++i)
                    if (pendingMask_ & (1u << i))
                        fx_->setParamNormalized(i, pendingP_[(size_t)i]);
                pendingMask_ = 0;
            }
            smEn_.snap(0.0f); // quick clickless fade-in on the new DSP
            smEn_.setTarget(enabled_ ? 1.0f : 0.0f);
            smEnDry_.setTarget(enabled_ ? 1.0f : 0.0f);
            push(FXEventType::TypeChanged, (float)(int)fx_->kind());
        }
    }

    int pendingFxKind() const noexcept {
        // only used for the TypeRequested heuristic — a pending object's
        // kind is read without adopting (safe: control thread owns it
        // until adoption, RT is the only adopter).
        auto* p = pending_.load(std::memory_order_acquire);
        return p != nullptr ? (int)p->kind() : -1;
    }

    void push(FXEventType t, float v) noexcept {
        if (events_ != nullptr) {
            FXEvent e { (uint8_t)t, (uint8_t)target_, (uint8_t)slot_,
                        0, v };
            events_->tryPush(e);
        }
    }

    double rate_ { 48000.0 };

    std::atomic<FXProcessor*> pending_ { nullptr };
    std::atomic<FXProcessor*> trash_   { nullptr };
    std::atomic<uint8_t> quality_ { (uint8_t)FXQuality::Medium }; // §55
    FXProcessor* fx_ { nullptr }; // RT-owned

    bool  enabled_  { false };
    float mix_      { 0.5f };
    float inGain_   { 1.0f };
    float outGain_  { 1.0f };
    int   syncMode_ { 1 };
    int   division_ { (int)BeatDiv::D_1_4 };
    int   wantType_ { 0 };
    /// P1..P8 writes parked until the next processor adoption.
    float    pendingP_[kMaxFxParams] {};
    uint32_t pendingMask_ { 0 };

    mixer::GainSmoother smMix_, smIn_, smOut_, smEn_, smEnDry_;
    std::vector<float> dryL_, dryR_;

    rt::SpscRingBuffer<FXEvent, 256>* events_ { nullptr };
    int target_ { -1 };
    int slot_   { -1 };

    std::atomic<float> cpuMeter_ { 1.0f };
    int     badBlocks_   { 0 };
    bool    busy_        { false };
    bool    tailing_     { false };  // draining a natural tail (§42)
    bool    tailDone_    { false };  // tail already drained once
    int64_t tailSamples_ { 0 };
};

} // namespace onda::fx
