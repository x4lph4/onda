#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Sample-rate policy (§7-8): THE ENGINE FOLLOWS THE DEVICE. There is
/// no application-level resampler — decks pitch-shift per track anyway
/// and the recorder stamps the real device rate. `choose()` picks the
/// device rate closest to the preference; `explain()` states clearly
/// when the device's rate differs from what was asked for.
struct SampleRateManager {
    /// Best supported rate for `preferred` (0 → device default best:
    /// 48000 when offered, else the highest common rate).
    static double choose(const std::vector<double>& supported,
                         double preferred) {
        if (supported.empty()) return 0.0;
        double want = preferred;
        if (want <= 0.0)
            want = std::find(supported.begin(), supported.end(),
                             48000.0) != supported.end()
                 ? 48000.0 : supported.back();
        double best = supported.front();
        for (double r : supported)
            if (std::abs(r - want) < std::abs(best - want))
                best = r;
        return best;
    }

    /// Human explanation when the applied rate ≠ the requested rate
    /// ("" when they match).
    static std::string explain(double requested, double applied) {
        if (requested <= 0.0 || requested == applied) return {};
        char b[160];
        std::snprintf(b, sizeof b,
            "Device does not support %.0f Hz — running at %.0f Hz "
            "instead. No resampling is applied; decks and the recorder "
            "use the device rate directly.",
            requested, applied);
        return b;
    }

    static bool supported(const std::vector<double>& rates, double r) {
        return std::find(rates.begin(), rates.end(), r) != rates.end();
    }
};

} // namespace onda::adevice
