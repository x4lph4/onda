#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MidiTypes.h"
#include "MappingTypes.h"

namespace onda::ctrl {

/// MIDI monitor + latency accounting. Keeps a bounded ring of recent
/// messages both directions and latency stats (input → processed,
/// feedback turnaround). Read by the Device/Diagnostics UI.
class ControllerDiagnostics {
public:
    struct Entry {
        bool     in = true;        // direction
        MidiMsg  msg;
        double   atSec = 0.0;      // engine-local time
        uint32_t device = 0;
        bool     mapped = false;   // produced an action
        bool     sysex  = false;   // SysEx event (msg.data holds len)
        SysExMsg sx;               // payload when sysex == true
        std::string decoded;       // §50 decoded control name
    };

    static constexpr size_t kRing = 256;

    void setMonitorEnabled(bool on) { monitor_ = on; }
    bool monitorEnabled() const { return monitor_; }

    void logIn (uint32_t dev, const MidiMsg& m, double at, bool acted,
                std::string decoded = "");
    void logOut(uint32_t dev, const MidiMsg& m, double at);
    /// SysEx events live in the same ring (§13/§50) — the monitor
    /// shows direction + bytes; never reaches MappingEngine.
    void logSysex(uint32_t dev, const SysExMsg& sx, double at,
                  bool consumed);

    /// input→action latency sample (µs) from MappingEngine timing.
    void noteLatency(int64_t us);

    const std::deque<Entry>& ring() const { return ring_; }

    int64_t inCount()   const { return in_; }
    int64_t outCount()  const { return out_; }
    int64_t actedCount()const { return acted_; }

    double avgLatencyUs() const { return n_ ? (double)(sumUs_ / n_) : 0.0; }
    double maxLatencyUs() const { return (double)maxUs_; }

    /// Human-readable monitor line (DeviceView / logs).
    static std::string describe(const Entry& e);
    static std::string describeMsg(const MidiMsg& m);
    static std::string describeSysex(const SysExMsg& sx);

    /// §51 raw-logger export: last `max` ring entries as text lines
    /// (bounded — the ring itself is the recording; nothing grows).
    std::string dump(size_t max = kRing) const;

private:
    std::deque<Entry> ring_;
    bool     monitor_ = true;
    int64_t  in_ = 0, out_ = 0, acted_ = 0;
    int64_t  sumUs_ = 0, maxUs_ = 0, n_ = 0;
};

/// Learn Mode: arm a target, next mapped-or-unmapped input captures
/// its address and yields a ready-to-save Mapping.
class LearnMode {
public:
    enum class Stage : uint8_t { Idle, Armed };

    /// Arm with the mapping template (target + mode picked in the
    /// editor); the captured input fills `in`.
    void arm(const Mapping& tmpl) { tmpl_ = tmpl; stage_ = Stage::Armed; }
    void cancel() { stage_ = Stage::Idle; }
    bool armed() const { return stage_ == Stage::Armed; }

    /// Feed every inbound message here *before* the mapping engine.
    /// When it returns a Mapping, the capture is complete (engine
    /// must not dispatch that message).
    bool capture(const MidiMsg& m, Mapping& out);

    const Mapping& tmpl() const { return tmpl_; }

private:
    Stage  stage_ = Stage::Idle;
    Mapping tmpl_;
};

/// MIDI clock — 24 PPQN realtime messages, decoupled from the
/// application's MasterClock. The app stays time master by default;
/// this block only estimates/announces tempo. (FLX4-class devices
/// don't need it — the plumbing exists for hosts that do.)
class MidiClock {
public:
    static constexpr int kPpqn = 24;

    enum class Role : uint8_t { Off, Sink, Source };
    void setRole(Role r) { role_ = r; tickN_ = 0; accumUs_ = 0; }
    Role role() const { return role_; }

    /// Feed a realtime message (already filtered to Clock*).
    void ingest(const MidiMsg& m);
    /// Should this tick be emitted? Call for outbound scheduling.
    bool clockOutDue(int64_t nowUs, double bpm);

    bool   running()  const { return running_; }
    double inputBpm() const { return bpm_; }

private:
    Role     role_ = Role::Off;
    bool     running_ = false;
    int      tickN_ = 0;
    int64_t  accumUs_ = 0, lastTickUs_ = 0, lastEmitUs_ = 0;
    double   bpm_ = 0.0;
};

/// HID abstraction — intentionally a stub. Real HID drivers/plugins
/// attach here when a documented, permitted interface exists for a
/// given device. Nothing proprietary is implemented.
class HidManager {
public:
    /// A pluggable HID driver contract — future device support
    /// implements this against a documented interface (e.g. a
    /// published HID report descriptor or a vendor-sanctioned API).
    class Driver {
    public:
        virtual ~Driver() = default;
        virtual bool open(const DeviceInfo&) = 0;
        virtual void close() = 0;
        /// poll for input; return messages (may be empty)
        virtual void poll(std::vector<MidiMsg>& out) = 0;
    };

    /// Register a driver factory for a device pattern. Matching is
    /// done by the caller; the manager owns lifecycle + poll loop.
    using Factory = std::function<std::unique_ptr<Driver>()>;
    void registerFactory(std::string matchName, Factory f) {
        factories_.push_back({ std::move(matchName), std::move(f) });
    }
    size_t factoryCount() const { return factories_.size(); }

private:
    struct Entry { std::string match; Factory make; };
    std::vector<Entry> factories_;
};

} // namespace onda::ctrl
