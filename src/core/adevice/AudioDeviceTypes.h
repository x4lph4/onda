#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onda::adevice {

/// Stable-ish device identity: backend + OS endpoint name. JUCE (the
/// platform layer) does not expose persistent hardware ids on all
/// backends, so `id` = backend|name — never just the display name.
struct DeviceInfo {
    std::string id;            // "WASAPI|Speakers (DDJ-FLX4)"
    std::string backend;       // "WASAPI", "CoreAudio", "ASIO", ...
    std::string name;
    std::string manufacturer;  // when the backend reports it, else ""
    int    inCh = 0, outCh = 0;
    bool   isDefaultOut = false;
    bool   isDefaultIn  = false;
    bool   present = true;     // false for a vanished current device
    static std::string makeId(const std::string& backend,
                            const std::string& name) {
        return backend + "|" + name;
    }
};

/// What a device can do — probed, not assumed.
struct DeviceCapabilities {
    int   inCh = 0, outCh = 0;
    std::vector<double> sampleRates;
    std::vector<int>    bufferSizes;
    bool  duplex = false;           // in+out on one endpoint
    bool  exclusiveCapable = false; // WASAPI-exclusive / ASIO-style
    bool  lowLatency = false;       // accepts <128-frame buffers
    bool  known = false;            // probe succeeded at all
};

/// Per-device channel routing — output indices into the device stream.
/// -1 = unmapped. Inputs reserved for future capture paths.
/// Phase 26 adds the booth pair (§17): the mixer already owns a booth
/// bus on outMap[4..5]; the domain layer now models it explicitly.
struct ChannelMapping {
    int masterL = 0, masterR = 1;
    int cueL = 2, cueR = 3;         // headphone bus; -1 → no cue out
    int boothL = -1, boothR = -1;   // booth monitor; -1 → no booth out
    int inL = -1, inR = -1;
    bool hasCue()   const { return cueL >= 0 && cueR >= 0; }
    bool hasBooth() const { return boothL >= 0 && boothR >= 0; }
    bool operator==(const ChannelMapping& o) const {
        return masterL == o.masterL && masterR == o.masterR
            && cueL == o.cueL && cueR == o.cueR
            && boothL == o.boothL && boothR == o.boothR
            && inL == o.inL && inR == o.inR;
    }
};

enum class LatencyPref : int { Low = 0, Balanced = 1, Stable = 2 };

/// Persisted per device id — settings a user tuned for that hardware.
struct DeviceProfile {
    std::string   deviceId;
    double        sampleRate = 0.0;
    int           bufferSize = 0;
    LatencyPref   pref = LatencyPref::Balanced;
    ChannelMapping map;
    // minimal hand-rolled JSON (single level, known keys)
    std::string toJson() const;
    static bool fromJson(const std::string&, DeviceProfile&);
};

/// User-facing device settings — the "AudioSettingsModel".
struct DeviceSettings {
    std::string   outputId;         // device id; "" = system default
    std::string   inputId;          // "" = none / system default
    double        sampleRate = 0.0; // 0 = let backend decide
    int           bufferSize = 0;   // 0 = from LatencyPref
    LatencyPref   pref = LatencyPref::Balanced;
    ChannelMapping map;
    bool          exclusive = false; // when the backend allows it
};

/// Latency math — every number is labelled estimated vs measured.
struct LatencyReport {
    double bufferMs = 0.0;      // bufferSize / rate * 1000
    double inputMs = 0.0;       // device-reported input latency
    double outputMs = 0.0;
    double roundTripMs = 0.0;   // buffer + in + out (all reported)
    bool   measured = false;    // false = estimated, never faked
    std::string explain;        // "256/48000 = 5.3 ms + device 2.1+2.1 ms"
};

enum class HealthState : int {
    Healthy = 0, Warning, DeviceLost, Reconnecting, Recovered, Failed
};

/// §32 — typed failure reasons, never bare strings. `DeviceResult`
/// carries the code plus the actionable human message.
enum class AudioError : int {
    Ok = 0,
    DeviceNotFound,
    UnsupportedSampleRate,
    UnsupportedBufferSize,
    ChannelConfigurationFailed,
    InitializationFailed,
    DeviceDisconnected,
    StreamStartFailed,
    StreamStopFailed,
    ManualConfigRequired      // auto-route could not find a safe map
};
inline const char* audioErrorName(AudioError e) {
    switch (e) {
        case AudioError::Ok:                          return "OK";
        case AudioError::DeviceNotFound:              return "DEVICE_NOT_FOUND";
        case AudioError::UnsupportedSampleRate:       return "UNSUPPORTED_SAMPLE_RATE";
        case AudioError::UnsupportedBufferSize:       return "UNSUPPORTED_BUFFER_SIZE";
        case AudioError::ChannelConfigurationFailed:  return "CHANNEL_CONFIGURATION_FAILED";
        case AudioError::InitializationFailed:        return "INITIALIZATION_FAILED";
        case AudioError::DeviceDisconnected:          return "DEVICE_DISCONNECTED";
        case AudioError::StreamStartFailed:           return "STREAM_START_FAILED";
        case AudioError::StreamStopFailed:            return "STREAM_STOP_FAILED";
        case AudioError::ManualConfigRequired:        return "MANUAL_CONFIGURATION_REQUIRED";
    }
    return "?";
}
struct DeviceResult {
    AudioError  code = AudioError::Ok;
    std::string message;      // actionable detail for the user
    bool        ok()   const { return code == AudioError::Ok; }
    bool        empty()const { return message.empty(); } // legacy-compat
    const char* codeName() const { return audioErrorName(code); }
    static DeviceResult success() { return {}; }
    static DeviceResult fail(AudioError c, std::string m) {
        return { c, std::move(m) };
    }
};

/// §31 — user-facing device state, derived from backend status +
/// recovery FSM. Distinct from HealthState (internal diagnostic FSM).
enum class AudioDeviceStatus : int {
    Unavailable = 0,   // no device selected / none present
    Available,         // present, not streaming
    Initializing,      // select()/apply() in flight
    Active,            // streaming
    Disconnected,      // vanished while selected
    Error,             // last operation failed
    Recovering         // recovery FSM attempting a reopen
};
inline const char* audioDeviceStatusName(AudioDeviceStatus s) {
    switch (s) {
        case AudioDeviceStatus::Unavailable:  return "UNAVAILABLE";
        case AudioDeviceStatus::Available:    return "AVAILABLE";
        case AudioDeviceStatus::Initializing: return "INITIALIZING";
        case AudioDeviceStatus::Active:       return "ACTIVE";
        case AudioDeviceStatus::Disconnected: return "DISCONNECTED";
        case AudioDeviceStatus::Error:        return "ERROR";
        case AudioDeviceStatus::Recovering:   return "RECOVERING";
    }
    return "?";
}

/// One over-budget callback — recorded for the health report.
struct UnderrunEvent {
    int64_t tsMs = 0;
    double  cbMs = 0.0;         // measured callback duration
    double  budgetMs = 0.0;
    float   cpu = 0.0f;
    int     buffer = 0;
};

struct HealthReport {
    uint64_t underruns = 0;     // over-budget callbacks
    uint64_t xruns = 0;         // scheduling gaps (engine counter)
    double   cbBudgetMs = 0.0;
    double   cbWorstMs = 0.0;
    double   cbAvgMs = 0.0;
    float    cpu = 0.0f;
    bool     overload = false;  // cpu > ~85% of budget
    std::vector<UnderrunEvent> recent;  // capped ring
};

/// Routing-test signal modes (§30) — rendered post-mixer, replacing
/// program audio on the mapped outputs.
enum class TestSignal : int {
    Off = 0, Silence, Sine440, LeftOnly, RightOnly, InPhase,
    AltPing,        // L/R alternate — routing check
    CueSine         // sine on the cue/headphone channels only
};
inline const char* testSignalName(TestSignal m) {
    switch (m) {
        case TestSignal::Off:      return "off";
        case TestSignal::Silence:  return "silence";
        case TestSignal::Sine440:  return "sine 440Hz";
        case TestSignal::LeftOnly: return "left only";
        case TestSignal::RightOnly:return "right only";
        case TestSignal::InPhase:  return "in-phase";
        case TestSignal::AltPing:  return "alternate L/R";
        case TestSignal::CueSine:  return "cue sine";
    }
    return "?";
}

} // namespace onda::adevice
