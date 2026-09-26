#pragma once

#include <cstdio>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Latency calculation (§10-11). Everything is ESTIMATED unless a real
/// round-trip loopback ran — `measured` is never set without one.
/// buffer latency = bufferSize / rate; device-reported in/out latencies
/// (driver + hardware) are added on top when the backend provides them.
struct LatencyManager {
    static LatencyReport estimate(double rate, int bufferSize,
                                  int deviceInLatSmp = 0,
                                  int deviceOutLatSmp = 0) {
        LatencyReport r;
        if (rate <= 0.0) return r;
        r.bufferMs = bufferSize / rate * 1000.0;
        r.inputMs  = deviceInLatSmp / rate * 1000.0;
        r.outputMs = deviceOutLatSmp / rate * 1000.0;
        r.roundTripMs = r.bufferMs + r.inputMs + r.outputMs;
        r.measured = false;
        char b[256];
        std::snprintf(b, sizeof b,
            "%d/%d Hz = %.1f ms buffer + %.1f in + %.1f out (driver) "
            "= %.1f ms estimated round trip",
            bufferSize, (int)rate, r.bufferMs, r.inputMs, r.outputMs,
            r.roundTripMs);
        r.explain = b;
        return r;
    }
};

} // namespace onda::adevice
