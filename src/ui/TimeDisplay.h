#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace onda::ui {

/// §37-38 — track time readouts. Pure formatting; the views call it
/// per paint, tests verify the strings.
///
/// Format rule (§38): M:SS below one hour, H:MM:SS above — a DJ
/// never needs .1s precision on a 2-hour recording, and a 6-minute
/// track must not waste width on leading zero-hours.
struct TimeDisplay {

    enum class Mode : int { Elapsed = 0, Remaining, Total };

    /// The three values a deck status line can show (§37).
    struct Triple { double elapsed, remaining, total; };

    static Triple triple(double positionSec, double durationSec) {
        const double dur = durationSec > 0.0 ? durationSec : 0.0;
        const double el  = std::clamp(positionSec, 0.0, dur);
        return { el, dur - el, dur };
    }

    /// "M:SS" under an hour, "H:MM:SS" at/over one hour.
    /// Negative input clamps to 0 — a seek-past-end never prints
    /// garbage on the status line.
    static void format(char* out, size_t n, double sec) {
        if (sec < 0.0 || !(sec == sec)) sec = 0.0;   // NaN-safe
        const auto t = (int64_t)(sec + 0.5);         // nearest second
        const int64_t h = t / 3600;
        const int64_t m = (t % 3600) / 60;
        const int64_t s = t % 60;
        if (h > 0) std::snprintf(out, n, "%lld:%02lld:%02lld",
                                 (long long)h, (long long)m,
                                 (long long)s);
        else       std::snprintf(out, n, "%lld:%02lld",
                                 (long long)m, (long long)s);
    }

    /// Sub-hour with centiseconds "M:SS.ss" (scratch-precision
    /// readout); at/over an hour falls back to "H:MM:SS" — centis
    /// are noise at that scale.
    static void formatCs(char* out, size_t n, double sec) {
        if (sec < 0.0 || !(sec == sec)) sec = 0.0;
        if (sec >= 3599.995) { format(out, n, sec); return; }
        const int64_t cs = (int64_t)(sec * 100.0 + 0.5);
        std::snprintf(out, n, "%lld:%02lld.%02lld",
                      (long long)(cs / 6000),
                      (long long)((cs % 6000) / 100),
                      (long long)(cs % 100));
    }

    /// Remaining is conventionally shown negative ("-1:23").
    static void formatMode(char* out, size_t n, Mode mode,
                           double positionSec, double durationSec) {
        const Triple t = triple(positionSec, durationSec);
        switch (mode) {
            case Mode::Remaining:
                if (t.remaining > 0.5) {
                    char tmp[24];
                    format(tmp, sizeof tmp, t.remaining);
                    std::snprintf(out, n, "-%s", tmp);
                } else {
                    format(out, n, 0.0);
                }
                return;
            case Mode::Total:   format(out, n, t.total);   return;
            case Mode::Elapsed:
            default:            format(out, n, t.elapsed); return;
        }
    }

    static const char* tag(Mode mode) {
        switch (mode) {
            case Mode::Remaining: return "REM";
            case Mode::Total:     return "TOT";
            default:              return "EL";
        }
    }
};

} // namespace onda::ui
