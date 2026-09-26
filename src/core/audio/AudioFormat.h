#pragma once

#include <cstddef>
#include <cstdint>

namespace onda {

/// Global audio format contract for the engine.
///
/// Internal processing format: float32 interleaved stereo.
/// Device rates supported: 44100 / 48000 / 88200 / 96000 (set by HAL).
/// Decoded tracks are stored at the *device* rate, so all positional math
/// in decks/mixer happens in a single timebase.
struct AudioFormat {
    static constexpr int    kChannels       = 2;
    static constexpr size_t kMaxBlockFrames = 8192;   // engine-wide max render block
    static constexpr float  kFullScale      = 1.0f;
    static constexpr float  kClipThreshold  = 1.0f;
};

} // namespace onda
