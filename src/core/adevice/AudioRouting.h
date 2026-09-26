#pragma once

#include <algorithm>
#include <string>
#include <utility>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Channel routing policy (§14-16). Master always gets a stereo pair;
/// the cue/headphone pair exists only when the device has ≥4 outs and
/// the mapping says so — never assumed, always configurable.
struct AudioRouting {
    /// Sensible default for a device with `numOut` outputs:
    /// stereo master on 0/1, cue on 2/3 when available.
    static ChannelMapping defaults(int numOut) {
        ChannelMapping m;
        m.masterL = numOut > 0 ? 0 : -1;
        m.masterR = numOut > 1 ? 1 : -1;
        if (numOut >= 4) { m.cueL = 2; m.cueR = 3; }
        else             { m.cueL = -1; m.cueR = -1; }
        return m;
    }

    /// DDJ-FLX4 documented topology: outs 1-2 MASTER (RCA), 3-4 PHONES.
    static ChannelMapping flx4() {
        ChannelMapping m;
        m.masterL = 0; m.masterR = 1; m.cueL = 2; m.cueR = 3;
        return m;
    }

    /// Validate a mapping against the device's output count.
    /// Returns "" when valid, else an actionable message.
    static std::string check(const ChannelMapping& m, int numOut) {
        auto bad = [&](int ch, const char* what) -> std::string {
            if (ch < -1 || ch >= numOut)
                return std::string(what) + " channel " +
                       std::to_string(ch) + " outside 0.." +
                       std::to_string(numOut - 1);
            return {};
        };
        for (auto [ch, w] : {std::pair{m.masterL, "master L"},
                            {m.masterR, "master R"},
                            {m.cueL, "cue L"}, {m.cueR, "cue R"}}) {
            const auto e = bad(ch, w);
            if (!e.empty()) return e;
        }
        if (m.masterL < 0 || m.masterR < 0)
            return "master output needs a stereo pair";
        if ((m.cueL < 0) != (m.cueR < 0))
            return "cue output needs both channels or neither";
        if (m.cueL == m.masterL || m.cueL == m.masterR
            || m.cueR == m.masterL || m.cueR == m.masterR)
            return "cue channels overlap the master pair";
        return {};
    }

    /// Highest channel index used (+1) — for stream sizing.
    static int span(const ChannelMapping& m) {
        int hi = m.masterR;
        if (m.hasCue()) hi = std::max(hi, m.cueR);
        return hi + 1;
    }
};

} // namespace onda::adevice
