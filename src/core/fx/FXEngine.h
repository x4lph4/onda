#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#include "../audio/AudioFormat.h"
#include "../rt/SpscRingBuffer.h"
#include "FXDiagnostics.h"
#include "FXRack.h"
#include "FXRouting.h"
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
    /// Per-stem FX slots live inside the decks (they must intercept
    /// the stem render before it joins the deck mix). Param space:
    /// target = deck, slot = 4..7 → stem 0..3 (§47).
    static constexpr int kStemSlots    = 4;
    static constexpr int kStemSlotBase = 4; // slot ids 4..7 on decks

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
    /// Deck slots 4..7 route to the bound deck's per-stem slots (§47).
    void loadEffect(int target, int slot, FXKind kind) {
        if (FXSlot* st = stemSlotFor(target, slot)) {
            st->loadEffect(kind);
            return;
        }
        racks_[(size_t)(target & 0xF)].slot(slot).loadEffect(kind);
    }
    /// Free retired processors. Call periodically from a control
    /// thread (the MixerController loader lambda does it per swap).
    void collectTrash() noexcept {
        for (auto& r : racks_) r.collectTrash();
        for (auto& row : stemSlots_)
            for (auto* s : row)
                if (s != nullptr) s->collectTrash();
    }

    /// Register a deck's per-stem FX slots (control thread, once at
    /// startup — the deck owns a contiguous FXSlot[kStemSlots] and
    /// keeps ownership). Also binds their event queue as target=deck,
    /// slot=4..7.
    void bindStemSlots(int deck, FXSlot* slots) noexcept {
        if (deck < 0 || deck >= kDeckRacks || slots == nullptr) return;
        for (int s = 0; s < kStemSlots; ++s) {
            stemSlots_[(size_t)deck][(size_t)s] = &slots[s];
            slots[s].bindEvents(&events_, deck, kStemSlotBase + s);
        }
    }

    /// Bound per-stem slot for (deck target, slot id 4..7), or nullptr.
    FXSlot* stemSlot(int deck, int stem) noexcept {
        return (deck >= 0 && deck < kDeckRacks && stem >= 0 && stem < kStemSlots)
             ? stemSlots_[(size_t)deck][(size_t)stem] : nullptr; }
    const FXSlot* stemSlot(int deck, int stem) const noexcept {
        return (deck >= 0 && deck < kDeckRacks && stem >= 0 && stem < kStemSlots)
             ? stemSlots_[(size_t)deck][(size_t)stem] : nullptr; }

    FXRack&       rack(int t) noexcept       { return racks_[(size_t)(t & 0xF)]; }
    const FXRack& rack(int t) const noexcept { return racks_[(size_t)(t & 0xF)]; }

    /// Reorder chain positions (§7). Non-RT; takes effect next block.
    void moveSlot(int target, int from, int to) noexcept {
        if (target < 0 || target >= kNumTargets) return;
        racks_[(size_t)target].moveSlot(from, to);
    }

    /// Routing descriptor for a target (declarative table, §9).
    FXRouting routing(int t) const noexcept {
        return routingFor((FXTarget)(t < 0 || t >= kNumTargets
                                     ? (int)FXTarget::Master : t));
    }

    /// Snapshot one target's diagnostics — control thread only.
    void fillDiagnostics(int t, FXRackDiag& d) const noexcept {
        if (t < 0 || t >= kNumTargets) return;
        const auto& rk = racks_[(size_t)t];
        const auto rt = routingFor((FXTarget)t);
        d.target = t; d.route = (int)rt.route; d.node = rt.node;
        d.orderPacked = rk.orderPacked();
        d.totalLatency = rk.latencySamples();
        d.totalCpuUs = 0.0f; d.totalFaults = 0; d.totalSanitized = 0;
        for (int s = 0; s < FXRack::kSlotsPerRack; ++s) {
            const auto& sl = rk.slot(s);
            auto& out = d.slots[(size_t)s];
            out.kind = (int)sl.activeKind();
            out.enabled = sl.enabled();
            out.busy = sl.busy();
            out.pending = sl.hasPending();
            out.mix = sl.mix();
            out.syncMode = sl.syncMode();
            out.division = sl.division();
            out.inPeak  = sl.inPeak.load(std::memory_order_relaxed);
            out.outPeak = sl.outPeak.load(std::memory_order_relaxed);
            out.cpuUs   = sl.cpuUs.load(std::memory_order_relaxed);
            out.sanitized = sl.sanitized.load(std::memory_order_relaxed);
            out.faults = sl.faults.load(std::memory_order_relaxed);
            out.latency = sl.latencySamples();
            d.totalCpuUs += out.cpuUs;
            d.totalFaults += out.faults;
            d.totalSanitized += out.sanitized;
        }
        for (int s = 0; s < kStemSlots; ++s) {
            const FXSlot* st = stemSlot(t, s);
            if (st == nullptr) continue;
            auto& out = d.stems[(size_t)s];
            out.kind = (int)st->activeKind();
            out.enabled = st->enabled();
            out.busy = st->busy();
            out.pending = st->hasPending();
            out.mix = st->mix();
            out.syncMode = st->syncMode();
            out.division = st->division();
            out.inPeak  = st->inPeak.load(std::memory_order_relaxed);
            out.outPeak = st->outPeak.load(std::memory_order_relaxed);
            out.cpuUs   = st->cpuUs.load(std::memory_order_relaxed);
            out.sanitized = st->sanitized.load(std::memory_order_relaxed);
            out.faults = st->faults.load(std::memory_order_relaxed);
            out.latency = st->latencySamples();
        }
    }

    /// Full report across every target (allocates — control thread).
    std::string diagnosticReport() const {
        std::string s;
        for (int t = 0; t < kNumTargets; ++t) {
            FXRackDiag d;
            fillDiagnostics(t, d);
            s += formatRackDiag(d);
            s += '\n';
        }
        return s;
    }

    // ---------------- RT ----------------
    void setParam(uint16_t id, float v) noexcept {
        const int t = fxTarget(id);
        const int s = fxSlot(id);
        const auto k = fxKindOf(id);
        if (FXSlot* st = stemSlotFor(t, s)) { st->setParam(k, v); return; }
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

    /// Target sync context (deck/sampler/master tempo feed).
    const SyncContext& context(int t) const noexcept {
        return ctx_[(size_t)(t < 0 ? 0 : (t >= kNumTargets ? 0 : t))]; }
    /// Sync-master context — the tempo source MASTER slots follow (§22).
    const SyncContext& masterContext() const noexcept {
        return ctx_[(size_t)masterIdx()]; }

    /// Insert rack for channel `d` — applied to the strip input.
    void processDeckInsert(int d, float* l, float* r, int n) noexcept {
        auto& rk = racks_[(size_t)d];
        if (!rk.busy()) return;
        rk.process(l, r, n, ctx_[(size_t)d], &ctx_[(size_t)masterIdx()]);
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
            const int m = masterIdx();
            rk.process(sl, sr, n, ctx_[(size_t)m], &ctx_[(size_t)m]);
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
        const int m = masterIdx();
        rk.process(l, r, n, ctx_[(size_t)m], &ctx_[(size_t)m]);
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
    int masterIdx() const noexcept {
        return fxSyncMaster_ >= 0 && fxSyncMaster_ < kDeckRacks
               ? fxSyncMaster_ : 0; }
    /// Slot routing helper: deck slots 4..7 address the bound deck's
    /// per-stem FX slots; everything else stays on the rack.
    FXSlot* stemSlotFor(int t, int s) noexcept {
        return (t >= 0 && t < kDeckRacks && s >= kStemSlotBase
                && s < kStemSlotBase + kStemSlots)
               ? stemSlots_[(size_t)t][(size_t)(s - kStemSlotBase)]
               : nullptr; }

    double rate_ { 48000.0 };
    std::array<FXRack, kNumTargets> racks_;
    std::array<SyncContext, kNumTargets> ctx_;
    int fxSyncMaster_ { 0 };

    std::array<std::vector<float>, kSendBuses> sendL_, sendR_;
    /// Borrowed deck stem FX slots (owned by DeckEngine, bound at
    /// startup by bindStemSlots). [deck][stem 0..3]
    FXSlot* stemSlots_[kDeckRacks][kStemSlots] {};
    float ret_[kSendBuses] { 1.0f, 1.0f };
    mixer::GainSmoother smRet_[kSendBuses];

    rt::SpscRingBuffer<FXEvent, 256> events_;
};

} // namespace onda::fx
