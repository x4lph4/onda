#pragma once

#include <memory>

#include "DelayFX.h"
#include "EchoFX.h"
#include "FilterFX.h"
#include "FlangerFX.h"
#include "FXProcessor.h"
#include "GateFX.h"
#include "PhaserFX.h"
#include "ReverbFX.h"

namespace onda::fx {

/// Effect factory. ALLOCATES — call only from a non-RT thread (the
/// control thread via FXSlot::loadEffect / MixerController).
inline std::unique_ptr<FXProcessor> createFX(FXKind kind) noexcept {
    switch (kind) {
        case FXKind::Delay:   return std::make_unique<DelayFX>();
        case FXKind::Echo:    return std::make_unique<EchoFX>();
        case FXKind::Reverb:  return std::make_unique<ReverbFX>();
        case FXKind::Filter:  return std::make_unique<FilterFX>();
        case FXKind::Flanger: return std::make_unique<FlangerFX>();
        case FXKind::Phaser:  return std::make_unique<PhaserFX>();
        case FXKind::Gate:    return std::make_unique<GateFX>();
        default:              return nullptr;
    }
}

inline const char* fxKindName(FXKind k) noexcept {
    switch (k) {
        case FXKind::Delay:   return "Delay";
        case FXKind::Echo:    return "Echo";
        case FXKind::Reverb:  return "Reverb";
        case FXKind::Filter:  return "Filter";
        case FXKind::Flanger: return "Flanger";
        case FXKind::Phaser:  return "Phaser";
        case FXKind::Gate:    return "Gate";
        default:              return "None";
    }
}

} // namespace onda::fx
