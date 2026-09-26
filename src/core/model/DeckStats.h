#pragma once

#include <atomic>
#include <cstdint>

namespace onda {

enum class DeckState : int { Empty = 0, Loading, Ready, Playing, Paused, Error };

/// Per-deck status shared between streamer thread (writer: state, duration,
/// streamEnded) and RT thread (writer: position, level, underruns). Readers:
/// UI/diagnostics. Each field is atomic — no locks anywhere.
struct DeckStats {
    std::atomic<int>      state{0};          // DeckState
    std::atomic<int64_t>  framesRead{0};     // device-rate frames consumed by RT
    std::atomic<double>   durationSec{0.0};
    std::atomic<double>   fileSampleRate{0.0};
    std::atomic<double>   positionSec{0.0};
    std::atomic<float>    levelL{0.0f};
    std::atomic<float>    levelR{0.0f};
    std::atomic<uint64_t> underruns{0};
    std::atomic<bool>     streamEnded{false};
};

} // namespace onda
