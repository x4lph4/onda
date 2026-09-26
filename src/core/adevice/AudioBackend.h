#pragma once

#include <string>
#include <vector>

#include "core/adevice/DeviceCatalog.h"

namespace onda::adevice {

/// What we ask the backend to apply (§5, §25). Empty names mean
/// "system default" / "no input"; zero rate/buffer mean "keep".
struct DesiredSetup {
    std::string outName;
    std::string inName;
    double rate = 0.0;
    int    buffer = 0;
    int    inChans = 0;
    int    outChans = 4;
    bool   exclusive = false;
};

/// Current state of the live stream, as the backend reports it.
struct BackendStatus {
    std::string backend;
    std::string outName, inName;
    double rate = 0.0;
    int    buffer = 0;
    int    inCh = 0, outCh = 0;
    int    inLatSmp = 0, outLatSmp = 0;
    bool   running = false;
    std::string lastError;
};

/// Platform seam (§2, §37): everything AudioDeviceEngine needs from
/// the OS. Implemented by JuceAudioBackend (juce::AudioDeviceManager)
/// in the app and by FakeBackend in tests — musical code never sees
/// whether audio is WASAPI, CoreAudio, ASIO, ALSA or JACK.
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string backendName() const = 0;
    virtual std::vector<RawDevice> enumerate() = 0;
    virtual BackendStatus status() = 0;
    /// Best-effort capability probe; `known=false` when it can't tell.
    virtual DeviceCapabilities probe(const std::string& name) = 0;
    /// Apply a setup; "" on success. Must leave a previously working
    /// device running on failure whenever the platform allows it.
    virtual std::string apply(const DesiredSetup&) = 0;
    /// Reopen on the system default output (last-resort recovery).
    virtual std::string reopenDefault() = 0;
};

} // namespace onda::adevice
