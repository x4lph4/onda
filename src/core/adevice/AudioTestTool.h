#pragma once

#include <cmath>
#include <cstring>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Internal routing-test generator (§30). RT-safe: writes into the
/// output block in place, after the mixer render — when a mode other
/// than Off is active it REPLACES program audio on the mapped outs.
/// `phase` is caller-owned continuity state (samples since start);
/// `pingHalf` toggles for AltPing. No allocation, no locks.
struct AudioTestTool {
    static constexpr double kSineHz = 440.0;
    static constexpr float  kAmp = 0.35f;   // ~-9 dBFS, audible not hot

    static void render(TestSignal mode, const ChannelMapping& map,
                       float* const* out, int numOut, int n,
                       double rate, uint64_t& phase) {
        if (mode == TestSignal::Off || rate <= 0.0) return;
        for (int c = 0; c < numOut; ++c)
            if (out[c]) std::memset(out[c], 0, (size_t)n * sizeof(float));

        auto sine = [&](uint64_t s) {
            return (float)(kAmp *
                std::sin(2.0 * 3.14159265358979 * kSineHz *
                         (double)s / rate));
        };
        auto put = [&](int ch, int i, float v) {
            if (ch >= 0 && ch < numOut && out[ch]) out[ch][i] = v;
        };
        const uint64_t ping =
            (uint64_t)(rate * 0.5);             // toggle every 0.5 s
        for (int i = 0; i < n; ++i, ++phase) {
            const float v = mode == TestSignal::Silence ? 0.0f
                          : sine(phase);
            switch (mode) {
            case TestSignal::Off:   return;
            case TestSignal::Silence: break;    // already zeroed
            case TestSignal::Sine440:
                put(map.masterL, i, v); put(map.masterR, i, v); break;
            case TestSignal::LeftOnly:  put(map.masterL, i, v); break;
            case TestSignal::RightOnly: put(map.masterR, i, v); break;
            case TestSignal::InPhase:
                put(map.masterL, i, v); put(map.masterR, i, v); break;
            case TestSignal::AltPing: {
                const bool left = (phase / ping) % 2 == 0;
                put(left ? map.masterL : map.masterR, i, v);
                break;
            }
            case TestSignal::CueSine:
                put(map.cueL, i, v); put(map.cueR, i, v); break;
            }
        }
    }
};

} // namespace onda::adevice
