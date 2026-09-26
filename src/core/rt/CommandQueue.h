#pragma once

#include <variant>
#include "SpscRingBuffer.h"

namespace onda::rt {

/// Commands sent from UI/control threads to the RT audio thread.
/// POD-only payloads; extend as features land.
struct SetMasterGain   { float gain; };   // linear 0..1
struct SetCueMix       { float mix; };    // 0 = cue bus, 1 = master bus
struct SetLimiterOn    { bool enabled; };

using Command = std::variant<SetMasterGain, SetCueMix, SetLimiterOn>;

/// UI thread pushes, audio thread drains at the top of every callback.
using CommandQueue = SpscRingBuffer<Command, 256>;

} // namespace onda::rt
