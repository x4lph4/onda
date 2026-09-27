#pragma once

#include <algorithm>
#include <string>
#include <utility>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Channel routing policy (§14-17). Master always gets a stereo pair;
/// the cue/headphone pair exists only when the device has ≥4 outs and
/// the booth pair only with ≥6 outs — never assumed, always
/// configurable.
struct AudioRouting {
    /// Sensible default for a device with `numOut` outputs:
    /// stereo master on 0/1, cue on 2/3, booth on 4/5 when available.
    /// Pairs the device cannot host are explicitly -1 — never left at
    /// the ChannelMapping{} sentinel values.
    static ChannelMapping defaults(int numOut) {
        ChannelMapping m;
        m.cueL = m.cueR = m.boothL = m.boothR = -1;
        m.masterL = numOut > 0 ? 0 : -1;
        m.masterR = numOut > 1 ? 1 : -1;
        if (numOut >= 4) { m.cueL = 2; m.cueR = 3; }
        if (numOut >= 6) { m.boothL = 4; m.boothR = 5; }
        return m;
    }

    /// DDJ-FLX4 documented topology: outs 1-2 MASTER (RCA), 3-4 PHONES.
    /// The FLX4 has exactly 4 outs — no booth pair exists to map.
    static ChannelMapping flx4() {
        ChannelMapping m;
        m.masterL = 0; m.masterR = 1; m.cueL = 2; m.cueR = 3;
        return m;
    }

    /// §46 — automatic routing decision. Returns a safe mapping from
    /// detected capabilities, or `ok=false` with a reason string when
    /// nothing safe can be determined — never a guessed route.
    struct RouteChoice {
        ChannelMapping map;
        bool           ok = false;
        std::string    reason;          // why manual config is needed
        bool manualRequired() const { return !ok; }
    };
    static RouteChoice autoRoute(int numOut) {
        RouteChoice c;
        if (numOut <= 0) {
            c.reason = "device reports no output channels — "
                       "Manual Configuration Required";
            return c;
        }
        c.map = defaults(numOut);
        if (numOut == 1) {
            // mono master is legal: both legs collapse onto channel 0
            c.map.masterR = 0;
            c.reason = "mono device — cue unmapped";
        }
        c.ok = true;
        return c;
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
                            {m.cueL, "cue L"}, {m.cueR, "cue R"},
                            {m.boothL, "booth L"}, {m.boothR, "booth R"}}) {
            const auto e = bad(ch, w);
            if (!e.empty()) return e;
        }
        if (m.masterL < 0 || m.masterR < 0)
            return "master output needs a stereo pair";
        if ((m.cueL < 0) != (m.cueR < 0))
            return "cue output needs both channels or neither";
        if ((m.boothL < 0) != (m.boothR < 0))
            return "booth output needs both channels or neither";
        auto overlaps = [&](int a, int b) {
            return a >= 0 && b >= 0 && a == b; };
        if (overlaps(m.cueL, m.masterL) || overlaps(m.cueL, m.masterR)
            || overlaps(m.cueR, m.masterL) || overlaps(m.cueR, m.masterR))
            return "cue channels overlap the master pair";
        if (overlaps(m.boothL, m.masterL) || overlaps(m.boothL, m.masterR)
            || overlaps(m.boothR, m.masterL) || overlaps(m.boothR, m.masterR)
            || overlaps(m.boothL, m.cueL) || overlaps(m.boothL, m.cueR)
            || overlaps(m.boothR, m.cueL) || overlaps(m.boothR, m.cueR))
            return "booth channels overlap another output pair";
        return {};
    }

    /// Highest channel index used (+1) — for stream sizing.
    static int span(const ChannelMapping& m) {
        int hi = m.masterR;
        if (m.hasCue())   hi = std::max(hi, m.cueR);
        if (m.hasBooth()) hi = std::max(hi, m.boothR);
        return hi + 1;
    }
};

} // namespace onda::adevice
