#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "../rt/CommandQueue.h"
#include "SamplePitchProcessor.h"
#include "SampleVoice.h"
#include "SamplerTypes.h"

namespace onda::sampler {

/// RT-side sampler engine. Lives inside AudioEngine; renders into a
/// mixer channel strip's inL/inR (channel `kSamplerChannel`).
///
/// Threading contract:
///   - render()/applyCommand(): RT thread only.
///   - publishSlot()/retireTrash(): loader thread only.
///   - slotState(): any thread (atomics).
///
/// RT rules inside render(): no allocation, no locks, no exceptions,
/// no virtuals on the hot path, everything bounded.
class SamplerRt {
public:
    static constexpr int kMaxSlots   = 32;   // bank-addressable slots
    static constexpr int kMaxVoices  = 32;   // global voice pool

    explicit SamplerRt(double sampleRate = 48000.0) : rate_(sampleRate) {}

    void prepare(double sampleRate) noexcept { rate_ = sampleRate; }

    // ---------------- loader thread ----------------
    /// Hand decoded audio for `slot` to the RT engine. Takes ownership;
    /// previous data moves to the retire list (reclaimed by the next
    /// retireTrash()). Lock-free single-producer handoff.
    void publishSlot(int slot, std::shared_ptr<const SampleData> d) {
        if (slot < 0 || slot >= kMaxSlots) return;
        auto* raw = new SlotBox{ std::move(d) };
        SlotBox* old = inbox_[slot].exchange(raw,
                                             std::memory_order_acq_rel);
        if (old) retire(old);
    }
    /// Loader thread: reclaim retired boxes (bounded, off RT).
    void retireTrash() noexcept {
        SlotBox* t = trash_.exchange(nullptr, std::memory_order_acq_rel);
        while (t) { SlotBox* n = t->next; delete t; t = n; }
    }
    void clearSlot(int slot) { publishSlot(slot, {}); }

    // ---------------- any thread ----------------
    int  activeVoices() const noexcept {
        return activeCount_.load(std::memory_order_relaxed);
    }
    int  slotVoices(int slot) const noexcept {
        return slotVoices_[slot & (kMaxSlots - 1)]
            .load(std::memory_order_relaxed);
    }
    int  slotState(int slot) const noexcept {
        return slotState_[slot & (kMaxSlots - 1)]
            .load(std::memory_order_relaxed);
    }
    uint64_t renderBlocks() const noexcept {
        return blocks_.load(std::memory_order_relaxed);
    }

    // ---------------- RT thread ----------------
    /// Feed the musical clock for quantized triggers + sync speed.
    /// Called once per block by AudioEngine from the master deck.
    void setClock(double bpm, double beatPos) noexcept {
        clockBpm_ = bpm;
        clockBeat_ = beatPos;
        haveClock_ = bpm > 0.0;
    }
    double clockBpm() const noexcept { return clockBpm_; }

    void applyCommand(const rt::Command& cmd) noexcept {
        std::visit([this](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, rt::SampleTrigger>)
                onTrigger(c.slot, c.vel);
            else if constexpr (std::is_same_v<T, rt::SampleRelease>)
                onRelease(c.slot);
            else if constexpr (std::is_same_v<T, rt::SampleStop>)
                onStop(c.slot);
            else if constexpr (std::is_same_v<T, rt::SampleSlotParam>)
                onSlotParam(c.slot, c.kind, c.v);
            else if constexpr (std::is_same_v<T, rt::SamplerParam>)
                onBankParam(c.kind, c.v);
        }, cmd);
    }

    /// Pull-model render — adds into l/r (strips expect summing).
    void renderPcm(float* l, float* r, int n) noexcept {
        collectInbox();
        anySolo_ = false;
        for (const auto& s : slots_)
            if (s.solo && s.data) { anySolo_ = true; break; }
        for (auto& v : voices_) {
            if (!v.active) continue;
            if (v.delay > 0) {               // quantized hold-off
                v.delay -= n;
                if (v.delay > 0) continue;
            }
            if (v.data == nullptr || v.data->frames <= 0) {
                v.active = false; continue;
            }
            if (anySolo_ && !slots_[v.slot].solo) v.releasing = true;
            renderVoice(v, l, r, n);
        }
        recountActive();
        blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    StealPolicy stealPolicy = StealPolicy::Oldest;
    int        maxVoices    = kMaxVoices;    // configurable (task §7)

private:
    struct SlotBox;   // fwd — referenced inside SlotRt
    // ---- per-slot RT-side shadow of domain params ----
    struct SlotRt {
        float volume = 1.0f, pan = 0.0f, gain = 1.0f, pitch = 0.0f;
        PlaybackMode mode = PlaybackMode::OneShot;
        QuantizeMode quant = QuantizeMode::Off;
        bool  syncOn = false, loopOn = false;
        bool  mute = false, solo = false;
        float filter = 0.0f;      // §32 tilt filter -1..+1
        float attackMs  = 0.0f;   // §28 0 = engine default
        float releaseMs = 0.0f;
        // §59-61 non-destructive window (decoded frames, -1 = unset)
        double startF = -1.0, endF = -1.0;
        double loopIn = -1.0, loopOut = -1.0;
        const SampleData* data = nullptr;
        SlotBox* box = nullptr;    // keeps SampleData alive (RT-owned)
        uint64_t lastUse = 0;
    };
    /// Lock-free mailbox node (loader → RT).
    struct SlotBox {
        std::shared_ptr<const SampleData> data;
        SlotBox* next = nullptr;
    };

    SamplePitchProcessor pitch_{};

    // ---- command handlers (RT) ----
    void onTrigger(int slot, float vel) noexcept {
        slot &= (kMaxSlots - 1);
        SlotRt& s = slots_[slot];
        if (s.data == nullptr || s.mute) return;
        const int64_t delay = quantDelay(s);
        startVoice(slot, s, vel, delay);
    }
    /// §23 — quantized stop: when the slot quantizes, the release
    /// waits for the next boundary instead of cutting immediately.
    void scheduleRelease(SampleVoice& v, int slot) noexcept {
        const int64_t d = quantDelay(slots_[slot]);
        if (d > 0) v.releaseAfter = d;
        else       v.releasing = true;
    }
    void onRelease(int slot) noexcept {
        slot &= (kMaxSlots - 1);
        for (auto& v : voices_)
            if (v.active && v.slot == slot &&
                v.mode == PlaybackMode::Hold) {
                v.held = false;
                scheduleRelease(v, slot);
            }
    }
    void onStop(int slot) noexcept {
        if (slot >= kMaxSlots) {          // 255+ → stop everything
            for (auto& v : voices_)
                if (v.active && !v.releasing)
                    scheduleRelease(v, v.slot);
            return;
        }
        for (auto& v : voices_)
            if (v.active && v.slot == slot && !v.releasing)
                scheduleRelease(v, slot);
    }
    void onSlotParam(int slot, uint8_t kind, float v) noexcept {
        slot &= (kMaxSlots - 1);
        SlotRt& s = slots_[slot];
        switch (kind) {
            case SP_Volume:   s.volume = clamp01(v); break;
            case SP_Pan:      s.pan = std::clamp(v, -1.f, 1.f); break;
            case SP_Gain:     s.gain = std::clamp(v, 0.f, 2.f); break;
            case SP_Pitch:    s.pitch = std::clamp(v, -48.f, 48.f); break;
            case SP_Mode:     s.mode = (PlaybackMode)(int)v; break;
            case SP_LoopOn:   s.loopOn = v > 0.5f; break;
            case SP_SyncOn:   s.syncOn = v > 0.5f; break;
            case SP_Quantize: s.quant = (QuantizeMode)(int)v; break;
            case SP_Mute:     s.mute = v > 0.5f;
                              if (s.mute) onStop(slot); break;
            case SP_Solo:     s.solo = v > 0.5f; break;
            case SP_Filter:   s.filter = std::clamp(v, -1.f, 1.f);
                              break;
            case SP_Attack:   s.attackMs = std::clamp(v, 0.f, 500.f);
                              break;
            case SP_Release:  s.releaseMs = std::clamp(v, 0.f, 2000.f);
                              break;
            case SP_TrimStart: s.startF = v < 0.0f ? -1.0 : (double)v;
                              break;
            case SP_TrimEnd:  s.endF   = v <= 0.0f ? -1.0 : (double)v;
                              break;
            case SP_LoopIn:   s.loopIn = v < 0.0f ? -1.0 : (double)v;
                              break;
            case SP_LoopOut:  s.loopOut = v < 0.0f ? -1.0 : (double)v;
                              break;
            default: break;
        }
        // live-update running voices for this slot (vol/pan/pitch)
        if (kind == SP_Volume || kind == SP_Pan || kind == SP_Gain ||
            kind == SP_Pitch)
            for (auto& vc : voices_)
                if (vc.active && vc.slot == slot) retune(vc, s);
    }
    void onBankParam(uint8_t kind, float v) noexcept {
        switch (kind) {
            case 0: bankGain_ = std::clamp(v, 0.f, 2.f); break;
            case 1: maxVoices = std::clamp((int)v, 1, kMaxVoices); break;
            default: break;
        }
    }

    // ---- voice machinery ----
    int64_t quantDelay(const SlotRt& s) noexcept {
        const double qb = quantizeBeats(s.quant);
        if (qb <= 0.0 || !haveClock_) return 0;
        // frames until the next quantum boundary on the master grid
        const double posInQ = std::fmod(clockBeat_, qb);
        const double toNext = (posInQ < 0.0 ? -posInQ : qb - posInQ);
        const double framesPerBeat = rate_ * 60.0 / clockBpm_;
        int64_t d = (int64_t)(toNext * framesPerBeat + 0.5);
        return d < 0 ? 0 : d;
    }

    /// §28 — env step for a millisecond ramp (0 ms → engine default).
    float envStep(float ms, float def) const noexcept {
        if (ms <= 0.0f) return def;
        const double n = (double)ms * 0.001 * rate_;
        return n > 1.0 ? (float)(1.0 / n) : 1.0f;
    }

    void startVoice(int slot, SlotRt& s, float vel, int64_t delay) {
        SampleVoice* v = nullptr;
        // same-slot retrigger semantics by mode
        for (auto& w : voices_) {
            if (!w.active) continue;
            if (w.slot != slot) continue;
            switch (s.mode) {
                case PlaybackMode::Gate:
                    w.releasing = true;      // toggle off
                    return;                  // press toggles: off now
                case PlaybackMode::Retrigger:
                    break;                   // §9 poly — no choke
                case PlaybackMode::OneShot:
                case PlaybackMode::Loop:
                case PlaybackMode::Hold:
                    w.releasing = true;      // replace, no poly pileup
                    break;
            }
        }
        v = allocVoice(slot);
        if (!v) return;
        v->data  = s.data;
        v->slot  = slot;
        v->step  = stepFor(s);
        v->mode  = s.mode;
        v->active = true;
        v->releasing = false;
        v->held  = s.mode == PlaybackMode::Hold;
        v->loop  = s.mode == PlaybackMode::Loop || s.loopOn;
        v->vel   = std::clamp(vel, 0.0f, 1.0f);
        v->delay = delay;
        v->releaseAfter = -1;
        v->age   = 0;
        v->env   = 0.0f;
        v->envInc = envStep(s.attackMs, SampleVoice::kFadeIn);
        v->envDec = envStep(s.releaseMs, SampleVoice::kFadeOut);
        v->seq   = ++seq_;
        // §59-61 window: clamp into the decoded data
        const double N = (double)(s.data ? s.data->frames : 0);
        v->startF  = s.startF >= 0.0 ? std::min(s.startF, N) : 0.0;
        v->endF    = s.endF   >= 0.0 ? std::min(s.endF,   N) : -1.0;
        v->loopIn  = s.loopIn  >= 0.0 ? std::min(s.loopIn,  N)
                                    : v->startF;
        v->loopOut = s.loopOut >= 0.0 ? std::min(s.loopOut, N)
                                    : v->endF;
        v->pos     = v->startF;
        // §32 tilt filter (one-pole, coefficient set once per trigger):
        // f<0 → LP sweep 20kHz→80Hz cubic; f>0 → HP tilt 40Hz→9kHz.
        const float fa = std::fabs(s.filter);
        v->fltMode = fa < 0.02f ? 0 : (s.filter < 0.0f ? -1 : 1);
        float fc = 0.0f;
        if (v->fltMode < 0) {
            const float t = 1.0f - fa;
            fc = 80.0f + 19920.0f * t * t * t;
        } else if (v->fltMode > 0) {
            fc = 40.0f + 8960.0f * fa * fa;
        }
        if (v->fltMode != 0) {
            const float w = 6.2831853f * fc;
            v->fltA = w / ((float)rate_ + w);   // RC pole coefficient
        }
        v->fltYl   = v->fltYr = 0.0f;
        retune(*v, s);
        slotState_[slot].store(
            (int)(s.loopOn || s.mode == PlaybackMode::Loop
                  ? SlotState::Looping : SlotState::Playing),
            std::memory_order_relaxed);
        s.lastUse = blocks_.load(std::memory_order_relaxed);
    }

    double stepFor(const SlotRt& s) const noexcept {
        double st = pitch_.stepFor(s.pitch);
        if (s.syncOn && s.data && s.data->bpm > 0.0 && clockBpm_ > 0.0)
            st *= clockBpm_ / s.data->bpm;
        return st;
    }

    void retune(SampleVoice& v, const SlotRt& s) noexcept {
        const float g = s.volume * s.gain * bankGain_ * v.vel;
        // equal-power pan
        const double a = (s.pan * 0.5 + 0.5) * 1.5707963267948966;
        v.targetGL = g * (float)std::cos(a) * 1.41421356237f;
        v.targetGR = g * (float)std::sin(a) * 1.41421356237f;
        v.step = stepFor(s);
    }

    SampleVoice* allocVoice(int slot) noexcept {
        int active = 0;
        for (auto& v : voices_) active += v.active ? 1 : 0;
        if (active < maxVoices)
            for (auto& v : voices_) if (!v.active) return &v;
        // pool at limit → steal policy
        SampleVoice* victim = nullptr;
        if (stealPolicy == StealPolicy::Deny) return nullptr;
        if (stealPolicy == StealPolicy::SameSample)
            for (auto& v : voices_)
                if (v.active && v.slot == slot &&
                    (!victim || v.age > victim->age)) victim = &v;
        if (!victim && stealPolicy == StealPolicy::Quietest)
            for (auto& v : voices_)
                if (v.active &&
                    (!victim || v.level() < victim->level()))
                    victim = &v;
        if (!victim)   // Oldest, or SameSample/others with no match
            for (auto& v : voices_)
                if (v.active && (!victim || v.age > victim->age))
                    victim = &v;
        if (!victim) return nullptr;
        *victim = SampleVoice{};            // recycled, env ramp covers
        return victim;
    }

    void renderVoice(SampleVoice& v, float* l, float* r, int n) noexcept {
        const SampleData& d = *v.data;
        const float* sl = d.l.get();
        const float* sr = d.r.get();
        const int64_t N = d.frames;
        // §59-61 resolved window: end defaults to the sample end,
        // loop-out to the window end, loop-in to the window start.
        const int64_t endF  = v.endF   >= 0.0 ? (int64_t)v.endF   : N;
        const int64_t loopI = v.loopIn >= 0.0 ? (int64_t)v.loopIn
                                            : (int64_t)v.startF;
        const int64_t loopO = v.loopOut >= 0.0 ? (int64_t)v.loopOut
                                             : endF;
        // §10 — a valid loop region wraps at loopOut; otherwise the
        // window end binds (and ends) playback.
        const bool region = v.loop && loopO > loopI;
        const int64_t playEnd = region ? std::min(endF, loopO) : endF;
        // §23 — quantized stop: count down, then open the release.
        if (v.releaseAfter >= 0) {
            v.releaseAfter -= n;
            if (v.releaseAfter < 0) v.releasing = true;
        }
        ++v.age;
        for (int i = 0; i < n; ++i) {
            // envelope — click-free start/release (per-slot rates §28)
            if (v.releasing) {
                v.env -= v.envDec;
                if (v.env <= 0.0f) { v.active = false; break; }
            } else if (v.env < 1.0f) {
                v.env = std::min(1.0f, v.env + v.envInc);
            }
            // gain smoothing per-sample-ish (cheap one-pole)
            v.gainL += (v.targetGL - v.gainL) * 0.02f;
            v.gainR += (v.targetGR - v.gainR) * 0.02f;

            const int64_t i0 = (int64_t)v.pos;
            const float frac = (float)(v.pos - (double)i0);
            if (i0 >= playEnd - 1) {         // window/loop end
                if (region) { v.pos = (double)loopI; continue; }
                v.active = false; break;
            }
            float outL = sl[i0] + (sl[i0+1] - sl[i0]) * frac;
            float outR = sr[i0] + (sr[i0+1] - sr[i0]) * frac;
            // §32 tilt filter: LP sweeps darker, HP tilts thin
            if (v.fltMode != 0) {
                v.fltYl += v.fltA * (outL - v.fltYl);
                v.fltYr += v.fltA * (outR - v.fltYr);
                if (v.fltMode < 0) { outL = v.fltYl; outR = v.fltYr; }
                else               { outL -= v.fltYl; outR -= v.fltYr; }
            }
            const float e = v.env;
            l[i] += outL * v.gainL * e;
            r[i] += outR * v.gainR * e;
            v.pos += v.step;
        }
    }

    void collectInbox() noexcept {
        for (int s = 0; s < kMaxSlots; ++s) {
            SlotBox* b = inbox_[s].exchange(nullptr,
                                            std::memory_order_acq_rel);
            if (!b) continue;
            if (slots_[s].box) retire(slots_[s].box);
            slots_[s].box  = b;
            slots_[s].data = b ? b->data.get() : nullptr;
            slotState_[s].store(
                (int)(slots_[s].data ? SlotState::Ready : SlotState::Empty),
                std::memory_order_relaxed);
        }
    }

    void retire(SlotBox* b) noexcept {
        b->next = trash_.load(std::memory_order_relaxed);
        while (!trash_.compare_exchange_weak(b->next, b,
                       std::memory_order_release,
                       std::memory_order_relaxed)) {}
    }

    void recountActive() noexcept {
        int total = 0;
        int per[kMaxSlots] = {};
        bool anyPlaying[kMaxSlots] = {};
        for (auto& v : voices_) {
            if (!v.active) continue;
            ++total; ++per[v.slot]; anyPlaying[v.slot] = true;
        }
        activeCount_.store(total, std::memory_order_relaxed);
        for (int s = 0; s < kMaxSlots; ++s) {
            slotVoices_[s].store(per[s], std::memory_order_relaxed);
            if (slots_[s].data == nullptr)
                slotState_[s].store((int)SlotState::Empty,
                                    std::memory_order_relaxed);
            else if (anyPlaying[s]) {
                const int cur = slotState_[s].load(std::memory_order_relaxed);
                if (cur == (int)SlotState::Ready ||
                    cur == (int)SlotState::Queued)
                    slotState_[s].store(
                        (int)SlotState::Playing,
                        std::memory_order_relaxed);
            } else {
                const int cur = slotState_[s].load(std::memory_order_relaxed);
                if (cur == (int)SlotState::Playing ||
                    cur == (int)SlotState::Looping)
                    slotState_[s].store((int)SlotState::Ready,
                                        std::memory_order_relaxed);
            }
        }
    }

    static float clamp01(float v) noexcept {
        return std::clamp(v, 0.0f, 1.0f);
    }

    double rate_ = 48000.0;

    // ---- RT-owned state ----
    SlotRt       slots_[kMaxSlots];
    SampleVoice  voices_[kMaxVoices];
    std::atomic<SlotBox*> inbox_[kMaxSlots] = {};
    std::atomic<SlotBox*> trash_{nullptr};
    bool         anySolo_ = false;
    double       clockBpm_ = 0.0, clockBeat_ = 0.0;
    bool         haveClock_ = false;
    float        bankGain_ = 1.0f;
    uint32_t     seq_ = 0;

    // ---- published stats (atomics, any-thread read) ----
    std::atomic<int>      activeCount_{0};
    std::atomic<int>      slotVoices_[kMaxSlots] = {};
    std::atomic<int>      slotState_[kMaxSlots] = {};
    std::atomic<uint64_t> blocks_{0};
};

} // namespace onda::sampler
