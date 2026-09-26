#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../MappingTypes.h"

namespace onda::ctrl::flx4 {

/// DDJ-FLX4 control inventory — a declarative description of every
/// documented control: physical function, message address, logical
/// target and where the byte stream came from. `verified` marks
/// confirmation on a physical unit by the cited source; NOTHING is
/// verified on our own hardware — docs/controllers/ddj-flx4.md
/// (HARDWARE TEST PENDING).
enum Group : uint8_t {
    G_DECK_A = 0, G_DECK_B, G_MIXER, G_PADS, G_FX, G_UTILITY
};

inline const char* groupName(int g) {
    switch (g) {
        case G_DECK_A:  return "DECK_A";
        case G_DECK_B:  return "DECK_B";
        case G_MIXER:   return "MIXER";
        case G_PADS:    return "PADS";
        case G_FX:      return "FX";
        default:        return "UTILITY";
    }
}

struct Flx4Control {
    std::string id;        // "deck0.play", "mix.crossfader", ...
    std::string label;     // physical panel label
    int         group;
    MidiAddr    in;        // inbound address; pack()==0 = none
    std::string target;    // "cmd:deck0.playToggle",
                         // "param:CH_0_FADER", "plugin:jog", "unmapped"
    bool        feedback;  // documented LED/motor output exists
    const char* source;    // "OFF" Pioneer list / "MIX" Mixxx /
                           // "SLP" slipmate hw-measured
    bool        verified;  // confirmed on hardware by cited source
};

/// The whole documented control surface (both decks + mixer + pads +
/// fx + utility), generated once from Flx4Ids.h constants.
const std::vector<Flx4Control>& inventory();

/// Human-readable control name for a MIDI address — feeds the
/// Diagnostic Mode live-input decoder. Returns "" when the address
/// matches no documented control.
std::string controlNameFor(const MidiAddr& a);

} // namespace onda::ctrl::flx4
