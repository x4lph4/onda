#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ControllerDiagnostics.h"
#include "ControllerProfile.h"
#include "DeviceManager.h"
#include "FeedbackEngine.h"
#include "MappingEngine.h"
#include "ProfileManager.h"

namespace onda::ctrl {

/// Runtime state for one connected controller — no musical state is
/// duplicated here; everything mirrors ApplicationState through
/// snapshots/commands.
struct DeviceState {
    DeviceInfo    info;
    bool          connected  = false;
    std::string   profileId;
    uint32_t      capabilities = 0;

    MappingEngine mapping;
    FeedbackEngine feedback;

    /// platform output (set by the MIDI layer / test double).
    std::function<void(const MidiMsg&)> sendRaw;

    int64_t inCount = 0, actedCount = 0;
};

/// The controller subsystem façade.
///
///   Hardware (MIDI/HID adapters / virtual devices)
///       → processInput(device, msg)
///       → LearnMode? → MappingEngine → command/param/jog ports
///   Engine tick → FeedbackEngine → sendRaw → hardware
///
/// The engine owns NO business logic of its own — actions are always
/// expressed through the same ports the UI/keyboard use.
class ControllerEngine {
public:
    /// Outbound ports — wired once by the host (MainComponent).
    struct Ports {
        /// → CommandManager::invoke(id, CommandSource::Midi, v)
        std::function<void(uint32_t cmd, float v)> sendCommand;
        /// → CommandManager::state(id)  (-1/0/1)
        std::function<int(uint32_t)> commandState;
        /// → CommandManager::idFor(name) — resolve profile names
        std::function<uint32_t(const std::string&)> commandIdFor;
        /// → MixerController::setParam(id, v)
        std::function<void(uint16_t paramId, float v)> setParam;
        /// → MixerController::value(id)
        std::function<float(uint16_t)> paramState;
        /// → jog path (deck, JogMode, normalized delta)
        std::function<void(int deck, int mode, double delta)> jog;
        /// → NotificationCenter (device events, errors, learn hints)
        std::function<void(const std::string& text,
                           const std::string& detail, int level)>
            notify;
    };

    explicit ControllerEngine(std::string profileDir);

    void setPorts(Ports p);
    Ports& ports() { return ports_; }

    // ---- lifecycle ----
    /// Platform layer reports a device appearing.
    int  attachDevice(const DeviceInfo& d,
                      std::function<void(const MidiMsg&)> send,
                      double nowSec);
    /// Platform layer reports a device gone — releases resources,
    /// keeps state for the reconnect window.
    void detachDevice(uint32_t deviceId, double nowSec);

    // ---- input ----
    /// One inbound message. LearnMode eats armed captures; clock msgs
    /// go to MidiClock; the rest feeds echo-check → mapping → ports.
    void processInput(uint32_t deviceId, const MidiMsg& msg);

    // ---- periodic ----
    /// Feedback poll + housekeeping; call ~15–30 Hz from the UI tick.
    void tick(double nowSec);

    // ---- learn mode ----
    LearnMode& learn() { return learn_; }
    /// Arm learn on a specific device with a mapping template
    /// (target+mode chosen in the editor). Next input captures `in`.
    void learnArmFor(uint32_t deviceId, const Mapping& tmpl) {
        learnDevice_ = deviceId;
        learn_.arm(tmpl);
    }
    void learnCancel() { learn_.cancel(); }
    /// When learn captures on a device, the new mapping is added to
    /// that device's table and (optionally) merged into its profile.
    std::function<void(uint32_t deviceId, const Mapping&)>
        onLearnedMapping;

    // ---- pieces (public for UI/diagnostics/tests) ----
    DeviceManager&          devices()  { return devMgr_; }
    ProfileManager&         profiles() { return profiles_; }
    ControllerDiagnostics&  diag()     { return diag_; }
    MidiClock&              clock()    { return clock_; }
    HidManager&             hid()      { return hid_; }

    DeviceState*       device(uint32_t id);
    const DeviceState* device(uint32_t id) const;
    std::vector<uint32_t> deviceIds() const;

    /// (Re)apply a profile to a device — resolves command names,
    /// installs mappings + feedback watches.
    bool applyProfile(uint32_t deviceId, const ControllerProfile& p,
                      std::string& err);

    /// Restore feedback outputs after reconnect.
    void restoreFeedback(uint32_t deviceId);

    /// Total per-device processed counts (stress tests).
    int64_t totalIn() const   { return totalIn_; }
    int64_t totalActed() const{ return totalActed_; }

private:
    void onDeviceEvent(const DeviceInfo&, DeviceManager::Event);
    void wireFeedbackPorts(DeviceState& st, uint32_t id);

    Ports            ports_;
    DeviceManager    devMgr_;
    ProfileManager   profiles_;
    ControllerDiagnostics diag_;
    LearnMode        learn_;
    MidiClock        clock_;
    HidManager       hid_;
    double           nowSec_ = 0.0;

    std::unordered_map<uint32_t, std::unique_ptr<DeviceState>> states_;
    uint32_t learnDevice_ = 0;         // device learn is armed on
    int64_t  totalIn_ = 0, totalActed_ = 0;
};

} // namespace onda::ctrl
