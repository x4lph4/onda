#pragma once

#include <algorithm>
#include <vector>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Buffer-size policy (§9). Three preferences plus exact sizes when
/// the backend accepts them; never assumes a size the device rejected.
struct BufferManager {
    /// Target sizes per preference, in order of preference.
    static int forPreset(LatencyPref p,
                         const std::vector<int>& supported) {
        const int want = p == LatencyPref::Low     ? 128
                       : p == LatencyPref::Balanced ? 256
                       :                              512;
        return nearest(supported, want);
    }

    /// Closest supported size; empty list → 0 (unsupported).
    static int nearest(const std::vector<int>& sizes, int want) {
        if (sizes.empty()) return 0;
        return *std::min_element(sizes.begin(), sizes.end(),
            [want](int a, int b) {
                const int da = std::abs(a - want), db = std::abs(b - want);
                return da == db ? a < b : da < db;   // tie → smaller
            });
    }

    /// Acceptable only when the device advertises it.
    static bool supported(const std::vector<int>& sizes, int size) {
        return std::find(sizes.begin(), sizes.end(), size)
               != sizes.end();
    }

    /// Latency preference implied by a size (for UI echo).
    static LatencyPref prefFor(int size) {
        if (size <= 128) return LatencyPref::Low;
        if (size <= 384) return LatencyPref::Balanced;
        return LatencyPref::Stable;
    }
};

} // namespace onda::adevice
