#pragma once

#include <algorithm>
#include <string>

#include "core/adevice/AudioDeviceTypes.h"
#include "core/adevice/AudioRouting.h"
#include "core/adevice/DeviceCatalog.h"

namespace onda::adevice {

/// §27-28 — a *detected* DJ-controller audio profile. This is an audio
/// device profile only — it says nothing about the control (MIDI/HID)
/// interface, which lives in `ctrl::` and is detected independently
/// (§29-30: controller connected while audio device = something else
/// is a fully legal configuration).
///
/// Profiles are produced ONLY when the detected capabilities match —
/// never assumed from a name string alone, and never inventing
/// channels the device did not report.
struct DjAudioProfile {
    enum class Role : int { None = 0, DjController, DjInterface };

    Role        role = Role::None;
    std::string name;            // detected endpoint name
    ChannelMapping suggested;    // routing derived from detected caps
    int  outCh = 0, inCh = 0;
    bool headphonesOn34 = false; // phones live on outs 3/4
    bool hasBoothPair   = false; // a third stereo pair exists

    bool detected() const { return role != Role::None; }

    /// Detect a DJ audio profile from enumeration + probed
    /// capabilities. Returns role=None when nothing matches.
    static DjAudioProfile detect(const DeviceInfo& d,
                                 const DeviceCapabilities& caps) {
        DjAudioProfile p;
        p.name  = d.name;
        p.outCh = caps.known ? caps.outCh : d.outCh;
        p.inCh  = caps.known ? caps.inCh  : d.inCh;
        // baseline suggestion = what the detected outs can host —
        // never the ChannelMapping{} sentinel (a 2-out FLX4 must not
        // "suggest" a cue pair that doesn't exist).
        p.suggested = AudioRouting::defaults(p.outCh);

        if (DeviceCatalog::isFlx4(d)) {
            // DDJ-FLX4 audio endpoint: 4 outs — 1/2 master (RCA),
            // 3/4 headphones. Only claim the profile when the device
            // actually exposes ≥4 outs (caps probed or enum-reported).
            if (p.outCh >= 4) {
                p.role = Role::DjController;
                p.suggested = AudioRouting::flx4();
                p.headphonesOn34 = true;
            }
            return p;
        }
        // Generic multi-out DJ interface: only suggest what the
        // hardware provably has — master pair always, phones ≥4,
        // booth pair only ≥6 (no invented channels).
        if (p.outCh >= 4) {
            p.role = Role::DjInterface;
            p.suggested = AudioRouting::defaults(p.outCh);
            p.headphonesOn34 = true;
            p.hasBoothPair = p.outCh >= 6;
        }
        return p;
    }
};

} // namespace onda::adevice
