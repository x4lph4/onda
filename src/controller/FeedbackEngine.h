#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "MappingTypes.h"

namespace onda::ctrl {

/// Pushes application state back to hardware LEDs/pads/motors.
///
/// Feedback is driven by state *polling*, not by input source — so a
/// SYNC button lit from MIDI stays correct when the same toggle fires
/// from the keyboard or the UI.
///
/// Loop protection: outbound feedback to an address suppresses
/// identical inbound messages for a short window (motorized faders
/// echo host-sent values back).
class FeedbackEngine {
public:
    /// LED render mode (§42) — blink is evaluated in tick() from the
    /// wall-clock phase, so blink rate is exact and independent of
    /// the poll cadence (the throttle still applies).
    enum class LedPattern : uint8_t {
        Off = 0,      // follow source: on → onValue, off → offValue
        On,           // same as Off (explicit "solid")
        Blink,        // ~2 Hz square while source is on
        SlowBlink,    // ~0.5 Hz square while source is on
        FastBlink,    // ~4 Hz square while source is on
    };

    /// What to watch for one output address.
    struct Watch {
        MidiAddr   out;             // where to send
        // source: command state (-1/0/1) or normalized param 0..1
        uint32_t   cmd        = 0;  // command id (0 = use paramId)
        uint16_t   paramId    = 0;  // ParameterRegistry id
        uint8_t    onValue    = 127;
        uint8_t    offValue   = 0;
        bool       followValue = false; // echo param magnitude (motors)
        float      srcMin = 0.0f, srcMax = 1.0f; // param range → 0..1
        double     minIntervalSec = 0.02; // per-output throttle
        LedPattern pattern = LedPattern::Off; // §42 blink rendering
    };

    struct Ports {
        /// send a message to the device (device id + msg).
        std::function<void(const MidiAddr&, uint8_t)> send;
        /// command state read (-1/0/1); from CommandManager::state.
        std::function<int(uint32_t)> commandState;
        /// param value read (registry units); from MixerController.
        std::function<float(uint16_t)> paramState;
    };

    void setPorts(Ports p) { ports_ = std::move(p); }

    void setWatches(std::vector<Watch> w);
    int  addWatch(const Watch& w);
    void clear() { watches_.clear(); lastSent_.clear(); recentOut_.clear(); }
    const std::vector<Watch>& watches() const { return watches_; }

    /// Poll sources, emit changes. Call at UI rate (15–30 Hz).
    /// `nowSec` monotonic seconds; `force` re-sends everything
    /// (device reconnect restore).
    int tick(double nowSec, bool force = false);

    /// Echo suppression — MappingEngine calls this before dispatch:
    /// an input identical to a feedback message we sent <window> ago
    /// is treated as device echo, not user input.
    bool isEcho(const MidiAddr& in, uint8_t value,
                int64_t nowUs) const;

    /// Record an output (the send port may be used directly elsewhere).
    void noteSent(const MidiAddr& a, uint8_t v, int64_t timeUs);

    int64_t sentCount() const { return sent_; }
    int64_t suppressed() const { return suppressed_; }

private:
    struct Recent { uint8_t v = 0; int64_t us = 0; };
    struct Last   { uint8_t v = 0xFF; double t = -1e9; };

    Ports ports_;
    std::vector<Watch> watches_;
    std::unordered_map<uint32_t, Last>   lastSent_;   // per out addr
    std::unordered_map<uint32_t, Recent> recentOut_;  // echo window

    int64_t sent_ = 0;
    mutable int64_t suppressed_ = 0;   // diagnostic counter
    double echoWindowSec_ = 0.15;      // 150 ms
};

} // namespace onda::ctrl
