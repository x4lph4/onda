#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "MidiTypes.h"

namespace onda::ctrl {

/// Platform-agnostic device registry — owns the connected-device set
/// and emits connect/disconnect/reconnect events. The platform MIDI
/// layer (or a test double) calls attach()/detach() when the OS
/// reports changes; ControllerEngine listens and wires profiles +
/// feedback per device.
///
/// Reconnect: a device id that disappears and returns within
/// `reconnectWindowSec` of its last-seen time fires
/// `onReconnected` instead of `onConnected`, letting the engine
/// restore previous state rather than treat it as new.
class DeviceManager {
public:
    enum class Event { Connected, Disconnected, Reconnected };

    using Listener = std::function<void(const DeviceInfo&, Event)>;

    void addListener(Listener l) { listeners_.push_back(std::move(l)); }
    void setReconnectWindow(double sec) { reconnectWindowSec_ = sec; }

    /// Platform callbacks -------------------------------------------------
    /// A previously-unknown id → Connected; an id seen inside the
    /// reconnect window → Reconnected.
    void attach(const DeviceInfo& d, double nowSec);
    void detach(uint32_t id, double nowSec);

    // ---- queries ----
    const DeviceInfo* find(uint32_t id) const;
    bool connected(uint32_t id) const;
    std::vector<DeviceInfo> list() const;
    size_t count() const { return devices_.size(); }

    // ---- counters (diagnostics) ----
    int64_t connects() const    { return connects_; }
    int64_t disconnects() const { return disconnects_; }
    int64_t reconnects() const  { return reconnects_; }

private:
    struct Entry {
        DeviceInfo info;
        double     seenSec = 0.0;
        bool       gone = false;      // disconnected but inside window
        double     goneAtSec = 0.0;
    };

    void emit(const DeviceInfo& d, Event e) {
        for (auto& l : listeners_) l(d, e);
    }

    std::map<uint32_t, Entry> devices_;
    std::vector<Listener> listeners_;
    double reconnectWindowSec_ = 5.0;
    int64_t connects_ = 0, disconnects_ = 0, reconnects_ = 0;
};

} // namespace onda::ctrl
