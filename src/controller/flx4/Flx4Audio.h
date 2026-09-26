#pragma once

#include <string>
#include <vector>

namespace onda::ctrl::flx4 {

/// DDJ-FLX4 USB audio-interface profile (task §13/§14) — pure
/// name/topology logic, no OS calls. The platform layer enumerates
/// real devices and feeds names in; this class only classifies.
///
/// Documented topology (manual + Mixxx doc):
///   Windows: "DDJ-FLX4 WASAPI"; outputs "MASTER" (ch 1-2, RCA) and
///            "PHONES" (ch 3-4, 3.5 mm).
///   macOS:   "DDJ-FLX4" (CoreAudio), same channel layout —
///            community-measured.
///   Rates:   44.1 kHz / 48 kHz, 16/24-bit (spec sheet).
///   Mic:     the 1/4" TS MIC input is NOT routed to the computer —
///            it is hardware-mixed into the master output only
///            (documented in the Mixxx controller page).
struct Flx4AudioProfile {
    int   masterCh[2] = { 1, 2 };   // 1-based output channels
    int   phonesCh[2] = { 3, 4 };
    int   rates[2]    = { 44100, 48000 };
    bool  micToHost   = false;      // mic does not reach USB input
    std::string deviceName;         // matched OS endpoint, "" = none
    bool  present()   const { return !deviceName.empty(); }

    /// Does an OS audio-endpoint name belong to the FLX4?
    static bool matches(const std::string& osName) {
        std::string n;
        for (char c : osName) n += (char)std::tolower((unsigned char)c);
        return n.find("ddj-flx4") != std::string::npos ||
               n.find("ddj flx4") != std::string::npos;
    }

    /// Scan a list of OS endpoint names; adopt the first match.
    bool scan(const std::vector<std::string>& osNames) {
        for (auto& n : osNames)
            if (matches(n)) { deviceName = n; return true; }
        deviceName.clear();
        return false;
    }

    /// Channel role for an output index (0-based, 4-out topology):
    /// 0-1 master, 2-3 phones. -1 = unknown.
    static int roleFor(int outIndex) {
        if (outIndex < 0)  return -1;
        if (outIndex <= 1) return 0;   // master
        if (outIndex <= 3) return 1;   // phones
        return -1;
    }
};

} // namespace onda::ctrl::flx4
