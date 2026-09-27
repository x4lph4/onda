#pragma once

#include <memory>

#include "BeatRepeatFX.h"
#include "BeatRollFX.h"
#include "BitCrusherFX.h"
#include "ChorusFX.h"
#include "CompressorFX.h"
#include "DelayFX.h"
#include "DistortionFX.h"
#include "EchoFX.h"
#include "FilterFX.h"
#include "FlangerFX.h"
#include "FXProcessor.h"
#include "GateFX.h"
#include "NoiseFX.h"
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
        case FXKind::Chorus:    return std::make_unique<ChorusFX>();
        case FXKind::Distortion:return std::make_unique<DistortionFX>();
        case FXKind::BitCrusher:return std::make_unique<BitCrusherFX>();
        case FXKind::Compressor:return std::make_unique<CompressorFX>();
        case FXKind::BeatRepeat:return std::make_unique<BeatRepeatFX>();
        case FXKind::BeatRoll:  return std::make_unique<BeatRollFX>();
        case FXKind::Noise:     return std::make_unique<NoiseFX>();
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
        case FXKind::Chorus:    return "Chorus";
        case FXKind::Distortion:return "Distortion";
        case FXKind::BitCrusher:return "Bit Crusher";
        case FXKind::Compressor:return "Compressor";
        case FXKind::BeatRepeat:return "Beat Repeat";
        case FXKind::BeatRoll:  return "Beat Roll";
        case FXKind::Noise:     return "Noise";
        default:              return "None";
    }
}

} // namespace onda::fx
