#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "FXRouting.h"
#include "FXTypes.h"

namespace onda::fx {

/// Diagnostic snapshots (§39/§43): a consistent read-only picture of
/// every slot/rack for UI display, clipboard dumps and failure
/// investigation. Snapshots are filled on the control thread from the
/// slots' atomic meters — never built on the audio thread.
struct FXSlotDiag {
    int      kind       { 0 };
    bool     enabled    { false };
    bool     busy       { false };
    bool     pending    { false };
    float    mix        { 0.0f };
    int      syncMode   { 0 };
    int      division   { 0 };
    float    inPeak     { 0.0f };
    float    outPeak    { 0.0f };
    float    cpuUs      { 0.0f };
    uint64_t sanitized  { 0 };
    uint64_t faults     { 0 };
    int      latency    { 0 };
};

struct FXRackDiag {
    int      target      { -1 };
    int      route       { 0 };
    int      node        { -1 };
    uint32_t orderPacked { 0 };
    int      totalLatency{ 0 };
    float    totalCpuUs  { 0.0f };
    uint64_t totalFaults { 0 };
    uint64_t totalSanitized { 0 };
    std::array<FXSlotDiag, 4> slots {};
    /// Per-stem FX slots (deck targets only; §47) — empty kinds when
    /// the deck didn't bind its stem slots.
    std::array<FXSlotDiag, 4> stems {};
};

/// Format a rack snapshot as a human-readable report (allocates —
/// control thread only).
inline std::string formatRackDiag(const FXRackDiag& d) {
    std::string s;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "FX %s  route=%s node=%d  latency=%d smp  cpu=%.1f us  "
        "faults=%llu  sanitized=%llu",
        fxTargetName((FXTarget)d.target), fxRouteName((FXRoute)d.route),
        d.node, d.totalLatency, (double)d.totalCpuUs,
        (unsigned long long)d.totalFaults,
        (unsigned long long)d.totalSanitized);
    s += buf;
    for (int i = 0; i < 4; ++i) {
        const auto& sl = d.slots[(size_t)i];
        std::snprintf(buf, sizeof(buf),
            "\n  slot%d  kind=%d  %s  mix=%.2f  sync=%d div=%d"
            "  in=%.2f out=%.2f  cpu=%.1fus  lat=%d  san=%llu flt=%llu%s",
            i, sl.kind, sl.enabled ? "ON " : "off",
            (double)sl.mix, sl.syncMode, sl.division,
            (double)sl.inPeak, (double)sl.outPeak, (double)sl.cpuUs,
            sl.latency, (unsigned long long)sl.sanitized,
            (unsigned long long)sl.faults,
            sl.pending ? "  [pending]" : "");
        s += buf;
    }
    return s;
}

} // namespace onda::fx
