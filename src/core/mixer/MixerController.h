#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "../fx/FXFactory.h"
#include "../fx/FXMacro.h"
#include "../fx/FXPreset.h"
#include "MixerTypes.h"
#include "ParameterRegistry.h"

namespace onda::mixer {

/// Non-RT control surface for the mixer. UI widgets, MIDI/HID mappings,
/// automation and tests all call setParam() here; the value is validated
/// against ParameterRegistry, stored for read-back, and handed to the
/// audio thread through the command sink (rt::SetParam).
///
/// This class never touches audio — it is safe to call from any control
/// thread. It does not lock: the sink callback is expected to enqueue a
/// lock-free command.
class MixerController {
public:
    /// sink receives already-sanitized (id, value) pairs; typically it
    /// pushes rt::SetParam into the engine's CommandQueue.
    using Sink = std::function<void(uint16_t id, float v)>;

    void setSink(Sink s) noexcept { sink_ = std::move(s); }

    /// Loader for effect *type* changes: allocating/preparing a new
    /// FXProcessor can't happen on the audio thread, so the host wires
    /// this to MixerEngine::fx().loadEffect() (which parks the object
    /// for lock-free adoption). Also the right place to run FX
    /// housekeeping (collectTrash).
    using FxLoader = std::function<void(int target, int slot, int kind)>;
    void setFxLoader(FxLoader f) noexcept { fxLoader_ = std::move(f); }

    /// Mover for chain reorders — wired to FXEngine::moveSlot()
    /// (rewrites the rack's packed order table; takes effect next
    /// audio block).
    using FxMover = std::function<void(int target, int from, int to)>;
    void setFxMover(FxMover f) noexcept { fxMover_ = std::move(f); }

    /// Set a parameter (sanitized per registry). Returns the applied value.
    float setParam(uint16_t id, float v) noexcept {
        v = ParameterRegistry::sanitize(id, v);
        store(id, v);
        if (sink_) sink_(id, v);
        return v;
    }

    /// Last value sent through this controller (0 / default if unset).
    float value(uint16_t id) const noexcept {
        const ParamDef* d = ParameterRegistry::find(id);
        for (const auto& e : values_)
            if (e.id == id) return e.v;
        return d != nullptr ? d->def : 0.0f;
    }

    /// Convenience helpers -------------------------------------------------
    float channelGain(int ch, float v)   { return setParam(ParameterRegistry::channelGain(ch),   v); }
    float channelLow (int ch, float v)   { return setParam(ParameterRegistry::channelLow(ch),    v); }
    float channelMid (int ch, float v)   { return setParam(ParameterRegistry::channelMid(ch),    v); }
    float channelHigh(int ch, float v)   { return setParam(ParameterRegistry::channelHigh(ch),   v); }
    float channelFilter(int ch, float v) { return setParam(ParameterRegistry::channelFilter(ch), v); }
    float channelFader(int ch, float v)  { return setParam(ParameterRegistry::channelFader(ch),  v); }
    float channelCue (int ch, bool on)   { return setParam(ParameterRegistry::channelCue(ch),    on ? 1.0f : 0.0f); }
    float channelMute(int ch, bool on)   { return setParam(ParameterRegistry::channelMute(ch),   on ? 1.0f : 0.0f); }
    float channelSolo(int ch, bool on)   { return setParam(ParameterRegistry::channelSolo(ch),   on ? 1.0f : 0.0f); }
    float channelXfAssign(int ch, XfAssign a) {
        return setParam(ParameterRegistry::channelXfAssign(ch), (float)(int)a);
    }
    float crossfader(float x)     { return setParam(ParameterRegistry::CROSSFADER,  x); }
    float xfCurve(int c)          { return setParam(ParameterRegistry::XF_CURVE,    (float)c); }
    float masterGain(float g)     { return setParam(ParameterRegistry::MASTER_GAIN, g); }
    float cueVolume(float v)      { return setParam(ParameterRegistry::CUE_VOLUME,  v); }
    float cueMix(float m)         { return setParam(ParameterRegistry::CUE_MIX,     m); }
    float limiterOn(bool on)      { return setParam(ParameterRegistry::LIMITER_ON,  on ? 1.0f : 0.0f); }

    // ---- FX helpers -------------------------------------------------
    float channelSend(int ch, int bus, float v) {
        return setParam(bus == 0 ? ParameterRegistry::channelSendA(ch)
                                 : ParameterRegistry::channelSendB(ch), v);
    }
    float fxReturn(int bus, float v) {
        return setParam(bus == 0 ? ParameterRegistry::FX_RETURN_A
                                 : ParameterRegistry::FX_RETURN_B, v);
    }
    float fxEnabled(int t, int s, bool on) {
        return setParam(ParameterRegistry::fxEnabled(t, s), on ? 1.0f : 0.0f);
    }
    float fxMix(int t, int s, float v) {
        return setParam(ParameterRegistry::fxMix(t, s), v);
    }
    float fxSyncMode(int t, int s, bool synced) {
        return setParam(ParameterRegistry::fxSyncMode(t, s), synced ? 1.0f : 0.0f);
    }
    float fxDivision(int t, int s, int div) {
        return setParam(ParameterRegistry::fxDivision(t, s), (float)div);
    }
    /// Normalized 0..1 effect parameter p (0..7 -> P1..P8).
    float fxParam(int t, int s, int p, float v) {
        return setParam(ParameterRegistry::fxParam(t, s, p), v);
    }
    /// Change the effect in a slot: records the registry value and
    /// asks the wired loader to build/park the processor (non-RT).
    void setFxType(int t, int s, int kind) {
        setParam(ParameterRegistry::fxType(t, s), (float)kind);
        if (fxLoader_) fxLoader_(t, s, kind);
    }

    /// Move the effect at chain position `from` to `to` (§7).
    void moveFxSlot(int target, int from, int to) {
        if (fxMover_) fxMover_(target, from, to);
    }

    /// Apply a full preset to (target, slot) — non-RT only.
    /// Loads the effect, then pushes slot params and native effect
    /// params converted to normalized values through a scratch
    /// processor's paramInfo (control-thread allocation, never RT).
    void applyFxPreset(int t, int s, const fx::FXPreset& p) {
        setFxType(t, s, p.effectKind);
        fxSyncMode(t, s, p.syncMode != 0);
        fxDivision(t, s, p.division);
        fxMix(t, s, p.mix);
        setParam(ParameterRegistry::fxInGain(t, s), p.inGain);
        setParam(ParameterRegistry::fxOutGain(t, s), p.outGain);
        auto scratch = fx::createFX((fx::FXKind)p.effectKind);
        if (scratch == nullptr) return;
        for (int i = 0; i < fx::kMaxFxParams && i < scratch->paramCount();
             ++i) {
            const auto d = scratch->paramInfo(i);
            const float rng = d.maxV - d.minV;
            float nv = rng > 0.0f ? (p.params[i] - d.minV) / rng : 0.0f;
            nv = nv < 0.0f ? 0.0f : (nv > 1.0f ? 1.0f : nv);
            fxParam(t, s, i, nv);
        }
    }

    /// Push every write a macro resolves to through setParam —
    /// indistinguishable from individual knob moves downstream.
    /// `focusTarget` resolves kFocusTarget mappings.
    int applyFxMacro(const fx::FXMacro& m, int focusTarget = -1) {
        fx::FXMacro::Write w[fx::FXMacro::kMaxMappings];
        const int n = m.evaluate(w, fx::FXMacro::kMaxMappings,
                                 focusTarget);
        for (int i = 0; i < n; ++i) setParam(w[i].paramId, w[i].value);
        return n;
    }
    // -----------------------------------------------------------------------

private:
    void store(uint16_t id, float v) noexcept {
        for (auto& e : values_) {
            if (e.id == id) { e.v = v; return; }
            if (e.id == kEmpty) { e.id = id; e.v = v; return; }
        }
    }

    static constexpr uint16_t kEmpty = 0xffff;
    struct Entry { uint16_t id = kEmpty; float v = 0.0f; };

    Sink sink_;
    FxLoader fxLoader_;
    FxMover fxMover_;
    std::array<Entry, 256> values_ {};
};

} // namespace onda::mixer
