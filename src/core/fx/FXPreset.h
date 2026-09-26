#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "FXTypes.h"

namespace onda::fx {

/// Serializable snapshot of one FX slot: effect type, slot-level
/// parameters and native effect parameters. No pointers, no object
/// references — the blob survives sessions and machines.
struct FXPreset {
    std::string name;
    bool  factory  { false };
    int   effectKind { (int)FXKind::None };
    int   syncMode   { 1 };
    int   division   { (int)BeatDiv::D_1_4 };
    float mix        { 0.5f };
    float inGain     { 1.0f };
    float outGain    { 1.0f };
    float params[kMaxFxParams] {}; // native values, P1..P8
};

/// Binary serialization (little-endian layout; versioned magic).
/// Format: 'ONFX' | ver u8 | kind u8 | sync u8 | div u8 |
///         mix f32 | in f32 | out f32 | params[8] f32 |
///         nameLen u16 | name bytes
inline std::vector<uint8_t> serializePreset(const FXPreset& p) {
    std::vector<uint8_t> b;
    auto put  = [&](const void* d, size_t n) {
        const auto* c = (const uint8_t*)d;
        b.insert(b.end(), c, c + n);
    };
    const char magic[4] = { 'O', 'N', 'F', 'X' };
    put(magic, 4);
    const uint8_t ver = 1, kind = (uint8_t)p.effectKind,
                  sync = (uint8_t)p.syncMode, div = (uint8_t)p.division;
    put(&ver, 1); put(&kind, 1); put(&sync, 1); put(&div, 1);
    put(&p.mix, 4); put(&p.inGain, 4); put(&p.outGain, 4);
    put(p.params, sizeof(p.params));
    const uint16_t nl = (uint16_t)std::min<size_t>(p.name.size(), 0xFFFF);
    put(&nl, 2);
    put(p.name.data(), nl);
    return b;
}

inline bool deserializePreset(const uint8_t* d, size_t n, FXPreset& p) {
    if (d == nullptr || n < 4 + 4 + 12 + sizeof(p.params) + 2) return false;
    if (std::memcmp(d, "ONFX", 4) != 0) return false;
    if (d[4] != 1) return false; // version
    p.effectKind = d[5];
    p.syncMode   = d[6];
    p.division   = d[7];
    size_t off = 8;
    std::memcpy(&p.mix,     d + off, 4); off += 4;
    std::memcpy(&p.inGain,  d + off, 4); off += 4;
    std::memcpy(&p.outGain, d + off, 4); off += 4;
    std::memcpy(p.params,   d + off, sizeof(p.params));
    off += sizeof(p.params);
    uint16_t nl = 0;
    std::memcpy(&nl, d + off, 2); off += 2;
    if (off + nl > n) return false;
    p.name.assign((const char*)(d + off), nl);
    return true;
}

} // namespace onda::fx
