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
#include "JogWheelEngine.h"
#include "MappingEngine.h"
#include "PadEngine.h"
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
    /// optional SysEx path — virtual devices / HID-side channels.
    std::function<void(const SysExMsg&)> sendSysex;

    int64_t inCount = 0, actedCount = 0;

    // ---- Phase 27: lifecycle + layer + error (§5,§19,§46) ----
    DeviceLifecycle lifecycle = DeviceLifecycle::Disconnected;
    std::string     lastError;
};

/// Public snapshot of one controller's state (§46) — copyable POD-ish
/// view for UI/diagnostics; never exposes mutable engine internals.
struct ControllerState {
    uint32_t        id = 0;
    std::string     name;
    std::string     manufacturer;
    std::string     model;
    Transport       transport   = Transport::MidiUsb;
    bool            connected   = false;
    bool            initialized = false; // profile applied
    DeviceLifecycle lifecycle   = DeviceLifecycle::Disconnected;
    std::string     profileId;
    uint8_t         activeLayer = 0;
    int             mappingCount  = 0;
    int             feedbackWatches = 0;
    int64_t         inCount = 0, actedCount = 0;
    std::string     lastError;
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
    /// SysEx path (§13) — routed to the device plugin hook +
    /// diagnostics, never to MappingEngine.
    void processSysex(uint32_t deviceId, const SysExMsg& sx);

    // ---- layers (§18-19) ----
    /// Set the active mapping layer for a device (0=Default, 1=Shift,
    /// 2=Alt, 3=Custom). Device plugins and layer-switch mappings use
    /// this; hardware-split devices (FLX4) leave it at Default.
    void    setLayer(uint32_t deviceId, uint8_t layer);
    uint8_t layer(uint32_t deviceId) const;

    // ---- state snapshot (§46) ----
    /// Copyable state view of one device (empty id → disconnected
    /// snapshot). Cheap; safe to call every UI tick.
    ControllerState state(uint32_t deviceId) const;

    // ---- generic input surfaces (§20-29) ----
    /// Device-agnostic jog engine — plugins and jog mappings feed
    /// decoded turns here; mode/sensitivity/browse policy applied
    /// before the deck jog port.
    JogWheelEngine& jogWheel() { return jogWheel_; }
    /// Device-agnostic pad engine — 8 pads × 4 decks × shift state;
    /// emits forPad commands through the command port.
    PadEngine&      pads()     { return pads_; }

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

    // ---- device plugin seam (ADR-0009 §HID, ADR-0010) ----
    /// Optional per-device semantic handler — sees every inbound
    /// message after LearnMode capture, before static mappings.
    /// Return true = consumed (logged as acted). Vendor plugins use
    /// this for modeful surfaces (pad banks, jog touch, shift).
    std::function<bool(uint32_t deviceId, const MidiMsg&)>
        preDispatch;
    /// Optional lifecycle observer — invoked after internal handling
    /// of connect/disconnect/reconnect (event = DeviceManager::Event
    /// cast to int).
    std::function<void(const DeviceInfo&, int)> onDevice;
    /// Optional SysEx observer (§13) — sees every inbound SysEx
    /// (position dumps, vendor handshakes). Return true = consumed.
    std::function<bool(uint32_t deviceId, const SysExMsg&)> onSysex;

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
    /// §50 monitor decode — logical/mapped control name for an input.
    std::string decodeControl(const DeviceState& st,
                              const MidiAddr& a) const;

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

    JogWheelEngine jogWheel_;          // §20-26 generic jog layer
    PadEngine      pads_;              // §27-29 generic pad layer
};

} // namespace onda::ctrl
