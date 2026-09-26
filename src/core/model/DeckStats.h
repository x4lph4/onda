#pragma once

#include <atomic>
#include <cstdint>

namespace onda {

enum class DeckState : int {
    Empty = 0, Loading, Ready, Playing, Paused, Seeking, Error
};

/// Per-deck status shared between the loader thread (writer: state,
/// duration, load metrics, streamEnded) and the RT thread (writer:
/// position, level, underruns, param echoes). Readers: UI/diagnostics.
/// Every field atomic — no locks anywhere.
struct DeckStats {
    // ---- loader-written ----
    std::atomic<int>      state{0};            // DeckState
    std::atomic<double>   durationSec{0.0};
    std::atomic<double>   fileSampleRate{0.0};
    std::atomic<float>    loadProgress{0.0f};
    std::atomic<float>    loadLatencyMs{0.0f};
    std::atomic<int64_t>  totalBytes{0};
    std::atomic<bool>     streamEnded{false};

    // ---- RT-written ----
    std::atomic<int64_t>  framesRead{0};
    std::atomic<double>   positionSec{0.0};
    std::atomic<float>    levelL{0.0f}, levelR{0.0f};
    std::atomic<uint64_t> underruns{0};

    // ---- param echoes (RT -> UI) ----
    std::atomic<float>    pitchPct{0.0f};
    std::atomic<float>    pitchRange{8.0f};
    std::atomic<float>    bendPct{0.0f};
    std::atomic<bool>     keylock{false};
    std::atomic<bool>     reverse{false};
    std::atomic<bool>     quantize{false};
    std::atomic<bool>     loopOn{false};
    std::atomic<double>   loopInSec{0.0};
    std::atomic<double>   loopOutSec{0.0};

    // ---- beatgrid placeholders (wired by the analysis phase) ----
    std::atomic<double>   bpm{0.0};
    std::atomic<double>   anchorSec{0.0};
    std::atomic<double>   beatPos{-1.0};
};

} // namespace onda
