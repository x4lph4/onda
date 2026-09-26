#pragma once

#include <array>
#include <cstdint>

namespace onda {

/// Loop/roll/beat-jump size tables + index math (Phase 20 §11/§14/§16/§19).
/// One canonical list so UI pads, keyboard steps, MIDI maps and the
/// performance engine can never disagree about what "size 3" means.
/// Sizes are in BEATS — never seconds (§20).
struct LoopSizeController {
    /// Musical loop sizes selectable from pads/encoders (§11).
    static constexpr std::array<double, 11> kLoopSizes {
        1.0/32, 1.0/16, 1.0/8, 1.0/4, 1.0/2, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };
    /// Loop-roll sizes (§16) — one-bar bound by default; indices ≥6
    /// extend musically (2^i beats) so pads/encoders can grow past 1.
    static constexpr std::array<double, 6> kRollSizes {
        1.0/32, 1.0/16, 1.0/8, 1.0/4, 1.0/2, 1.0 };
    /// Beat-jump magnitudes (§19) — signed at the call site.
    static constexpr std::array<double, 7> kJumpSizes {
        1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0 };

    static double loopSize(int idx) {
        return kLoopSizes[(size_t)clampIdx(idx, (int)kLoopSizes.size())];
    }
    static double rollSize(int idx) {
        if (idx < 0) idx = 0;
        // extended rolls: idx 6→2bt, 7→4bt, 8→8bt …
        if (idx >= (int)kRollSizes.size())
            return (double)(1 << (idx - (int)kRollSizes.size() + 1));
        return kRollSizes[(size_t)idx];
    }
    static double jumpSize(int idx) {
        return kJumpSizes[(size_t)clampIdx(idx, (int)kJumpSizes.size())];
    }

    /// Nearest table index for an arbitrary beat size (encoder/pad
    /// friendly — an out-of-table value snaps to the closest step).
    static int nearestLoopIndex(double beats) {
        int best = 0; double bd = 1e30;
        for (int i = 0; i < (int)kLoopSizes.size(); ++i) {
            const double d = kLoopSizes[(size_t)i] > beats
                ? kLoopSizes[(size_t)i] / beats
                : beats / kLoopSizes[(size_t)i];
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
    static int nearestJumpIndex(double beats) {
        int best = 0; double bd = 1e30;
        const double b = beats < 0 ? -beats : beats;
        for (int i = 0; i < (int)kJumpSizes.size(); ++i) {
            const double d = kJumpSizes[(size_t)i] > b
                ? kJumpSizes[(size_t)i] / b : b / kJumpSizes[(size_t)i];
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }

    /// Step helpers for "+/- size" commands (encoder-friendly).
    static int stepIndex(int idx, int delta, int count) {
        const int n = idx + delta;
        return n < 0 ? 0 : (n >= count ? count - 1 : n);
    }

    static constexpr int loopCount() { return (int)kLoopSizes.size(); }
    static constexpr int rollCount() { return (int)kRollSizes.size(); }
    static constexpr int jumpCount() { return (int)kJumpSizes.size(); }

    /// Display text for a beat size ("1/4", "2", "32").
    static void formatBeats(double beats, char* out, int cap) {
        if (beats < 1.0) {
            const int den = (int)(1.0 / beats + 0.5);
            std::snprintf(out, (size_t)cap, "1/%d", den);
        } else {
            std::snprintf(out, (size_t)cap, "%d", (int)(beats + 0.5));
        }
    }

private:
    static int clampIdx(int i, int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); }
};

} // namespace onda
