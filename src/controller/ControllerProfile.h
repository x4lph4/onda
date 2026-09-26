#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MappingTypes.h"

namespace onda::ctrl {

/// A declarative controller profile — pure data, validated before
/// use. Versioned JSON; never executed.
///
/// Schema (profileVersion = 1):
/// {
///   "profileVersion": 1,
///   "controller": {
///     "manufacturer": "…", "model": "…",
///     "nameContains": "…"        // auto-match hint vs OS device name
///   },
///   "capabilities": ["JOG","TOUCH","PADS",…],
///   "jog": [{"deck":0,"mode":"pitchbend","in":{…},"sensitivity":1.0}],
///   "mappings": [ {in, target, mode, curve, encoder,
///                  min, max, step, enabled, out}, … ]
/// }
struct ControllerMeta {
    std::string manufacturer;
    std::string model;
    std::string nameContains;      // match substring for auto-assign
    std::string author;
    int         schemaMin = 1;     // oldest profileVersion we accept
};

/// Optional explicit jog entry — a jog is just a mapping with
/// Target::Jog, listed separately for readability.
struct ControllerProfile {
    static constexpr int kVersion = 1;

    int                    version = kVersion;
    ControllerMeta         meta;
    uint32_t               capabilities = 0;
    std::vector<Mapping>   mappings;

    // ---- validation (pure data checks — no execution) ----
    /// Returns "" or the first problem found.
    std::string validate() const;

    // ---- JSON (MiniJson — no code, no eval) ----
    std::string toJson() const;
    /// Parse + validate. ok=false → err filled, profile untouched.
    bool        fromJson(const std::string& text, std::string& err);

    /// Does this profile match an OS device name? (case-insensitive
    /// substring; empty nameContains never matches).
    bool matchesDevice(const std::string& osName) const;
};

/// capability name ↔ bit for JSON arrays
const char* capName(uint32_t bit);
uint32_t    capFromName(const std::string& s);
std::string capListToString(uint32_t caps);

/// enum ↔ string helpers shared by profile + tests
const char*   modeName(MappingMode m);
MappingMode   modeFromName(const std::string& s, bool& ok);
const char*   curveName(Curve c);
Curve         curveFromName(const std::string& s, bool& ok);
const char*   encoderName(EncoderMode e);
EncoderMode   encoderFromName(const std::string& s, bool& ok);
const char*   addrKindName(AddrKind k);
AddrKind      addrKindFromName(const std::string& s, bool& ok);
const char*   jogModeName(JogMode j);
JogMode       jogModeFromName(const std::string& s, bool& ok);

} // namespace onda::ctrl
