#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "MappingTypes.h"

namespace onda::ctrl {

/// MidiMessage → action. The engine is library-agnostic: it calls
/// plain std::function ports that the ControllerEngine wires to
/// CommandManager / MixerController / jog commands.
///
/// A Mapping is pure data; per-entry runtime (pickup arm, press edge)
/// lives in a parallel array so profiles are freely swappable.
///
/// Input is classified first:
///   - pressed/released edges for NoteOn/NoteOff and CC-as-button
///   - continuous value 0..1 (CC / pitch-bend / pressure)
///   - encoder deltas (per configured EncoderMode)
class MappingEngine {
public:
    /// Ports — set once by the engine host.
    struct Ports {
        /// command id + normalized action value (press/release or
        /// continuous, per mode); src keeps CommandSource::Midi.
        std::function<void(uint32_t cmd, float v)> sendCommand;
        /// sanitized param push (ParameterRegistry id + value).
        std::function<void(uint16_t paramId, float v)> setParam;
        /// jog event: deck, JogMode, normalized rotation delta.
        std::function<void(int deck, int jogMode, double delta)> jog;
        /// current command state for toggle/pickup (-1/0/1).
        std::function<int(uint32_t cmd)> commandState;
        /// current param value for pickup/relative modes.
        std::function<float(uint16_t paramId)> paramState;
        /// echo suppression check — was this input just caused by our
        /// own feedback output? (see FeedbackEngine)
        std::function<bool(const MidiAddr&, uint8_t, int64_t nowUs)>
            isEcho;
    };

    void setPorts(Ports p) { ports_ = std::move(p); }

    // ---- mapping table (pure data in, engine keeps runtime) ----
    /// Replace the whole table (profile switch). Run-state resets.
    void setMappings(std::vector<Mapping> maps);
    /// Single-entry ops for Learn Mode / editor.
    int  addMapping(const Mapping& m);            // → index
    bool removeMapping(int index);
    bool setMappingEnabled(int index, bool on);
    void clear();
    const std::vector<Mapping>& mappings() const { return maps_; }
    int find(const MidiAddr& a) const;            // -1 or index

    // ---- input ----
    /// Process one message. Returns true when it produced an action.
    bool process(const MidiMsg& msg, double nowSec);

    /// Update the known target value for pickup/relative bookkeeping
    /// (called with feedback state or after UI-side changes).
    void noteTargetValue(const MidiAddr& in, float v);

    // ---- diagnostics ----
    int64_t processed() const { return processed_; }
    int64_t dropped()   const { return dropped_; }
    /// µs spent inside process() (accumulated + last call).
    int64_t processUs() const { return processUs_; }
    int64_t lastProcessUs() const { return lastUs_; }

private:
    void indexAddr(const Mapping& m, int idx);
    bool dispatch(int idx, const MidiMsg& msg, float raw01,
                  bool press, bool release, double nowSec);
    int  decodeDelta(const Mapping& m, uint8_t v) const;
    float scale(const Mapping& m, float raw01) const;

    Ports ports_;
    std::vector<Mapping>        maps_;
    std::vector<MappingRuntime> run_;
    // addr → indices for O(1) lookup (multiple mappings per addr ok)
    std::unordered_map<uint32_t, std::vector<int>> byAddr_;

    int64_t processed_ = 0, dropped_ = 0;
    int64_t processUs_ = 0, lastUs_ = 0;
};

/// ---- shared helpers -------------------------------------------------

/// Decode an encoder byte to a signed tick delta (-127..127, 0 = none).
int encoderDelta(EncoderMode mode, uint8_t v);

/// Normalize 0..1 → curve → out range.
float applyCurve(Curve c, float x);
float scaleValue(const Mapping& m, float raw01);

} // namespace onda::ctrl
