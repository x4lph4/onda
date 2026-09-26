#pragma once

#include <array>
#include <cstdint>
#include <functional>

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
    std::array<Entry, 256> values_ {};
};

} // namespace onda::mixer
