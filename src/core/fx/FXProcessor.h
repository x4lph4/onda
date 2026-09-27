#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "../audio/AudioFormat.h"
#include "../mixer/GainSmoother.h"
#include "FXTypes.h"

namespace onda::fx {

/// Parameter value type (§30). Tags drive UI/HID presentation
/// (switch vs fader vs enumeration) without touching DSP.
enum class FXParamType : uint8_t {
    Float = 0,
    Integer,
    Boolean,
    Enum,
    BeatDivision   // specialized: value indexes BeatDiv
};

/// §37 — normalized(0..1) → native mapping law. Frequency-type
/// parameters use Log (equal knob travel per octave); Exp eases the
/// low end for taper-style params; Custom is reserved for processors
/// that override the normalized setters themselves (FXBase treats it
/// as Linear — the hook is the override).
enum class FXParamCurve : uint8_t {
    Linear = 0,
    Log,
    Exp,
    Custom,
};

/// Static description of one native effect parameter.
struct FXParamInfo {
    const char* name;
    float       minV;
    float       maxV;
    float       defV;
    const char* unit;
    float       step;    // quantization hint (0 = continuous)
    FXParamType type  { FXParamType::Float };
    FXParamCurve curve { FXParamCurve::Linear };  // §37
};

/// Type inference for tables that didn't tag explicitly: a stepped
/// 0..1 param is Boolean, a stepped param with a wider range is an
/// enumeration/integer, everything else is a continuous float.
constexpr FXParamType fxParamTypeOf(const FXParamInfo& i) noexcept {
    if (i.type != FXParamType::Float) return i.type;
    if (i.step >= 1.0f)
        return (i.minV == 0.0f && i.maxV == 1.0f) ? FXParamType::Boolean
                                                : FXParamType::Enum;
    return i.step > 0.0f ? FXParamType::Integer : FXParamType::Float;
}

/// Common interface every effect implements. The engine never sees an
/// effect's internals — it only drives this contract.
///
/// Params are exposed two ways:
///   * normalized 0..1 — what ParameterRegistry/MIDI/automation use
///   * native units    — tests, presets, state serialization
///
/// process() runs fully wet; dry/wet mixing and bypass crossfades are
/// the FXSlot's job so every effect gets identical behavior for free.
class FXProcessor {
public:
    virtual ~FXProcessor() = default;

    virtual FXKind      kind() const noexcept = 0;
    virtual const char* name() const noexcept = 0;

    virtual int         paramCount() const noexcept = 0;
    virtual FXParamInfo paramInfo(int i) const noexcept = 0;

    virtual void  setParamNormalized(int i, float v) noexcept = 0;
    virtual float paramNormalized(int i) const noexcept = 0;
    virtual void  setParamNative(int i, float v) noexcept = 0;
    virtual float paramNative(int i) const noexcept = 0;

    virtual bool supportsSync() const noexcept = 0;
    virtual int  latencySamples() const noexcept = 0;

    /// §55 — quality/CPU tier. A processor may reduce its internal
    /// workload on Low (e.g. ReverbFX drops to 4 combs + 2 allpasses).
    /// Default no-op: most effects already run at negligible cost.
    /// Call from the control thread; implementations must make the
    /// switch click-safe (state is preserved, only tap count changes).
    virtual void setQuality(FXQuality) noexcept {}
    virtual FXQuality quality() const noexcept { return FXQuality::Medium; }

    /// §42 — true when the effect produces a decaying tail after its
    /// input stops (delay/echo/reverb). The slot uses this to choose
    /// tail-drain bypass (feed silence, let the wet decay naturally)
    /// instead of the ~10 ms crossfade cut.
    virtual bool hasTail() const noexcept { return false; }

    /// Allocate worst-case resources. Non-RT only.
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    /// Clear internal audio state (delay lines, filters). RT-safe.
    virtual void reset() noexcept = 0;
    /// Per-block musical context (called by the owning slot, RT).
    virtual void setSyncContext(const SyncContext& s) noexcept = 0;
    /// RT. In-place stereo processing, fully wet.
    virtual void process(float* l, float* r, int n) noexcept = 0;

    /// Serializable state (params only — audio state is not persisted).
    virtual FXState saveState() const noexcept = 0;
    virtual void    restoreState(const FXState& s) noexcept = 0;
};

/// Helper base: implements normalized<->native mapping and per-sample
/// smoothing over a fixed array of GainSmoothers. The derived class
/// only defines the param table (infos()), kind()/name() and process().
template <size_t N>
class FXBase : public FXProcessor {
public:
    int paramCount() const noexcept override { return (int)N; }

    /// §37 — normalized → native under the param's declared curve.
    static float curveToNative(const FXParamInfo& d, float v) noexcept {
        switch (d.curve) {
            case FXParamCurve::Log:
                if (d.minV > 0.0f && d.maxV > d.minV)
                    return d.minV * std::pow(d.maxV / d.minV, v);
                return d.minV + v * (d.maxV - d.minV); // degenerate range
            case FXParamCurve::Exp:
                return d.minV + (d.maxV - d.minV) * v * v;
            default:
                return d.minV + v * (d.maxV - d.minV);
        }
    }
    /// Inverse of curveToNative — native → normalized.
    static float curveToNorm(const FXParamInfo& d, float x) noexcept {
        const float rng = d.maxV - d.minV;
        if (rng <= 0.0f) return 0.0f;
        switch (d.curve) {
            case FXParamCurve::Log:
                if (d.minV > 0.0f && d.maxV > d.minV && x > 0.0f)
                    return std::log(x / d.minV)
                         / std::log(d.maxV / d.minV);
                return (x - d.minV) / rng;
            case FXParamCurve::Exp: {
                const float n = (x - d.minV) / rng;
                return n > 0.0f ? std::sqrt(n) : 0.0f;
            }
            default:
                return (x - d.minV) / rng;
        }
    }

    void setParamNormalized(int i, float v) noexcept override {
        if (i < 0 || i >= (int)N) return;
        const FXParamInfo& d = paramInfo(i);
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        setParamNative(i, curveToNative(d, v));
    }
    float paramNormalized(int i) const noexcept override {
        if (i < 0 || i >= (int)N) return 0.0f;
        const FXParamInfo& d = paramInfo(i);
        return curveToNorm(d, paramNative(i));
    }
    void setParamNative(int i, float v) noexcept override {
        if (i < 0 || i >= (int)N) return;
        const FXParamInfo& d = paramInfo(i);
        v = v < d.minV ? d.minV : (v > d.maxV ? d.maxV : v);
        if (d.step > 0.0f)
            v = d.minV + d.step * (float)(int)((v - d.minV) / d.step + 0.5f);
        sm_[(size_t)i].setTarget(v);
    }
    float paramNative(int i) const noexcept override {
        if (i < 0 || i >= (int)N) return 0.0f;
        return sm_[(size_t)i].target();
    }

    int latencySamples() const noexcept override { return 0; }

    void setSyncContext(const SyncContext& s) noexcept override { ctx_ = s; }

    FXState saveState() const noexcept override {
        FXState s;
        s.kind = (int)kind();
        for (size_t i = 0; i < N && i < (size_t)kMaxFxParams; ++i)
            s.params[i] = sm_[i].target();
        return s;
    }
    void restoreState(const FXState& s) noexcept override {
        if (s.kind != (int)kind()) return;
        for (size_t i = 0; i < N && i < (size_t)kMaxFxParams; ++i)
            sm_[i].snap(s.params[i]); // instant restore, not ramped
    }

protected:
    /// Call from the derived prepare(): initializes all smoothers to
    /// their parameter defaults with `ms` smoothing.
    void prepareSmooth(double sr, float ms) {
        for (size_t i = 0; i < N; ++i) {
            sm_[i].prepare(sr, ms);
            sm_[i].snap(paramInfo((int)i).defV);
        }
    }

    float value(int i) const noexcept { return sm_[(size_t)i].current(); }
    float tick (int i) noexcept       { return sm_[(size_t)i].tick(); }
    float target(int i) const noexcept{ return sm_[(size_t)i].target(); }
    void  snap (int i, float v) noexcept { sm_[(size_t)i].snap(v); }

    const SyncContext& ctx() const noexcept { return ctx_; }

private:
    std::array<mixer::GainSmoother, N> sm_;
    SyncContext ctx_{};
};

} // namespace onda::fx
