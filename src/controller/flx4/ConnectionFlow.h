#pragma once

#include <string>
#include <vector>

namespace onda::ctrl::flx4 {

/// Staged controller bring-up (task §15) with per-stage diagnostics:
///
///   UsbConnected → DeviceDetected → CapsDetected → ProfileLoaded
///   → MidiInitialized → AudioDetected → Ready
///
/// Each stage records ok/detail so the UI/diagnostics can name the
/// exact failing step instead of "controller not working".
struct ConnectionFlow {
    enum Stage : int {
        S_UsbConnected = 0, S_DeviceDetected, S_CapsDetected,
        S_ProfileLoaded, S_MidiInitialized, S_AudioDetected,
        S_Ready, S_Count
    };

    struct Report {
        bool        ok = false;
        int         failedAt = -1;   // stage index or -1 = ready
        std::string detail[S_Count];
    };

    static const char* stageName(int s) {
        switch (s) {
            case S_UsbConnected:    return "usb-connected";
            case S_DeviceDetected:  return "device-detected";
            case S_CapsDetected:    return "capabilities-detected";
            case S_ProfileLoaded:   return "profile-loaded";
            case S_MidiInitialized: return "midi-initialized";
            case S_AudioDetected:   return "audio-detected";
            case S_Ready:           return "ready";
            default:                return "?";
        }
    }

    Report rep;

    bool advance(int stage, const std::string& detail) {
        rep.detail[stage] = detail;
        rep.failedAt = -1;
        if (stage == S_Ready) rep.ok = true;
        return true;
    }
    bool fail(int stage, const std::string& why) {
        rep.detail[stage] = why;
        rep.failedAt = stage;
        rep.ok = false;
        return false;
    }
    /// one-line summary for toasts/diagnostics
    std::string summary() const {
        if (rep.ok) return "ready";
        if (rep.failedAt >= 0)
            return std::string("failed at ") +
                   stageName(rep.failedAt) + ": " +
                   rep.detail[rep.failedAt];
        return "in progress";
    }
};

} // namespace onda::ctrl::flx4
