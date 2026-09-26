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
struct ChannelMapping {
    int masterL = 0, masterR = 1;
    int cueL = 2, cueR = 3;         // headphone bus; -1 → no cue out
    int inL = -1, inR = -1;
    bool hasCue() const { return cueL >= 0 && cueR >= 0; }
    bool operator==(const ChannelMapping& o) const {
        return masterL == o.masterL && masterR == o.masterR
            && cueL == o.cueL && cueR == o.cueR
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
