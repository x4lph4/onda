#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#include "../audio/AudioFormat.h"
#include "../rt/SpscRingBuffer.h"
#include "FXRack.h"
#include "FXTypes.h"

namespace onda::fx {

/// Owns every FX rack and the send/return buses, and routes FX
/// parameter ids to the right slot. Lives inside MixerEngine — all
/// processing methods are RT-only, parameter writes arrive through
/// rt::SetParam, effect swaps come from the control thread via
/// loadEffect() (non-RT allocate + lock-free handoff).
///
/// Topology handled here:
///   deck i   -> rack[DeckA+i]  (insert, into the strip input)
///   strip post-fader -> send bus b (per-channel send level)
///        -> rack[SendA+b] -> return level -> master sum
///   channel sum -> rack[Master] (insert) -> master bus gain/limiter
///
/// Tempo: setTempo() is called per block by the mixer; send buses and
/// the master rack follow fxSyncMaster_ (the sync master deck index).
class FXEngine {
public:
    static constexpr int kNumTargets   = (int)FXTarget::kCount;
    static constexpr int kSendBuses    = 2;
    static constexpr int kDeckRacks    = 4;

    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate;
        for (int t = 0; t < kNumTargets; ++t)
            racks_[(size_t)t].prepare(sampleRate, &events_, t);
        for (auto& b : sendL_) b.assign(AudioFormat::kMaxBlockFrames, 0.0f);
        for (auto& b : sendR_) b.assign(AudioFormat::kMaxBlockFrames, 0.0f);
        smRet_[0].prepare(sampleRate, 8.0f);
        smRet_[1].prepare(sampleRate, 8.0f);
        smRet_[0].snap(1.0f);
        smRet_[1].snap(1.0f);
    }

    // ---------------- non-RT ----------------
    /// Park a new effect for (target, slot). ALLOCATES — control thread.
    void loadEffect(int target, int slot, FXKind kind) {
        racks_[(size_t)(target & 0xF)].slot(slot).loadEffect(kind);
    }
    /// Free retired processors. Call periodically from a control
    /// thread (the MixerController loader lambda does it per swap).
    void collectTrash() noexcept {
        for (auto& r : racks_) r.collectTrash();
    }

    FXRack&       rack(int t) noexcept       { return racks_[(size_t)(t & 0xF)]; }
    const FXRack& rack(int t) const noexcept { return racks_[(size_t)(t & 0xF)]; }

    // ---------------- RT ----------------
    void setParam(uint16_t id, float v) noexcept {
        const int t = fxTarget(id);
        const int s = fxSlot(id);
        const auto k = fxKindOf(id);
        if (t < 0 || t >= kNumTargets || s < 0 || s >= FXRack::kSlotsPerRack)
            return;
        racks_[(size_t)t].slot(s).setParam(k, v);
    }

    /// Bus-level send return level (b = 0..1), RT.
    void setSendReturn(int b, float v) noexcept {
        if (b < 0 || b >= kSendBuses) return;
        ret_[b] = v;
        smRet_[b].setTarget(v);
    }
    float sendReturn(int b) const noexcept { return ret_[b]; }

    /// Tempo feed — call per block before the corresponding process*.
    void setTempo(int target, float bpm, float beatPhase = 0.0f) noexcept {
        if (target < 0 || target >= kNumTargets) return;
        ctx_[(size_t)target].bpm = bpm > 1.0f ? bpm : 120.0f;
        ctx_[(size_t)target].beatPhase = beatPhase;
    }
    /// Which deck index supplies tempo to sends + master racks.
    void setSyncMaster(int deck) noexcept { fxSyncMaster_ = deck; }
    int  syncMaster() const noexcept { return fxSyncMaster_; }

    /// Insert rack for channel `d` — applied to the strip input.
    void processDeckInsert(int d, float* l, float* r, int n) noexcept {
        auto& rk = racks_[(size_t)d];
        if (!rk.busy()) return;
        rk.process(l, r, n, ctx_[(size_t)d]);
    }

    /// Clear + accumulate one channel's post-fader signal into send b.
    void sendAccum(int b, const float* postL, const float* postR,
                   int n, float gain) noexcept {
        if (b < 0 || b >= kSendBuses || gain <= 0.0f) return;
        float* sl = sendL_[(size_t)b].data();
        float* sr = sendR_[(size_t)b].data();
        for (int i = 0; i < n; ++i) { sl[i] += postL[i] * gain; sr[i] += postR[i] * gain; }
    }

    /// Zero the send bus before accumulation (call once per bus/block).
    void sendReset(int b, int n) noexcept {
        std::memset(sendL_[(size_t)b].data(), 0, (size_t)n * sizeof(float));
        std::memset(sendR_[(size_t)b].data(), 0, (size_t)n * sizeof(float));
    }

    /// Process send bus b through its rack and sum the return into mix.
    void processSendBus(int b, float* mixL, float* mixR, int n) noexcept {
        if (b < 0 || b >= kSendBuses) return;
        auto& rk = racks_[(size_t)((int)FXTarget::SendA + b)];
        float* sl = sendL_[(size_t)b].data();
        float* sr = sendR_[(size_t)b].data();
        if (rk.busy()) {
            const int m = fxSyncMaster_ >= 0 && fxSyncMaster_ < kDeckRacks
                          ? fxSyncMaster_ : 0;
            rk.process(sl, sr, n, ctx_[(size_t)m]);
        } else {
            std::memset(sl, 0, (size_t)n * sizeof(float));
            std::memset(sr, 0, (size_t)n * sizeof(float));
        }
        float g = smRet_[b].current();
        for (int i = 0; i < n; ++i) {
            g = smRet_[b].tick();
            mixL[i] += sl[i] * g;
            mixR[i] += sr[i] * g;
        }
    }

    /// Master insert rack — applied to the summed mix before the
    /// master bus gain/limiter (the limiter always stays last).
    void processMaster(float* l, float* r, int n) noexcept {
        auto& rk = racks_[(size_t)FXTarget::Master];
        if (!rk.busy()) return;
        const int m = fxSyncMaster_ >= 0 && fxSyncMaster_ < kDeckRacks
                      ? fxSyncMaster_ : 0;
        rk.process(l, r, n, ctx_[(size_t)m]);
    }

    // ---------------- latency / cpu / events ----------------
    int targetLatency(int t) const noexcept {
        return t >= 0 && t < kNumTargets ? racks_[(size_t)t].latencySamples() : 0;
    }
    /// Worst-case insert latency across all targets — every current
    /// effect reports 0; the report is what a future PDC pass consumes.
    int maxLatencySamples() const noexcept {
        int mx = 0;
        for (const auto& r : racks_) mx = std::max(mx, r.latencySamples());
        return mx;
    }

    float fxCpuUs() const noexcept {
        float total = 0.0f;
        for (const auto& r : racks_)
            for (int s = 0; s < FXRack::kSlotsPerRack; ++s)
                total += r.slot(s).cpuUs.load(std::memory_order_relaxed);
        return total;
    }
    void setCpuMetering(bool on) noexcept {
        for (auto& r : racks_)
            for (int s = 0; s < FXRack::kSlotsPerRack; ++s)
                r.slot(s).setCpuMetering(on);
    }

    /// RT->control FX events; polled by UI/controller code.
    bool pollEvent(FXEvent& e) noexcept { return events_.tryPop(e); }

private:
    double rate_ { 48000.0 };
    std::array<FXRack, kNumTargets> racks_;
    std::array<SyncContext, kNumTargets> ctx_;
    int fxSyncMaster_ { 0 };

    std::array<std::vector<float>, kSendBuses> sendL_, sendR_;
    float ret_[kSendBuses] { 1.0f, 1.0f };
    mixer::GainSmoother smRet_[kSendBuses];

    rt::SpscRingBuffer<FXEvent, 256> events_;
};

} // namespace onda::fx
