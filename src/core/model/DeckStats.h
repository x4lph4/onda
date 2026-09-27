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

    // ---- beatgrid / performance (RT-written, filled when a grid is set) ----
    std::atomic<double>   bpm{0.0};        // grid bpm at current beat
    std::atomic<double>   anchorSec{0.0};  // grid first-beat position
    std::atomic<double>   beatPos{-1.0};   // fractional beat from grid start
    std::atomic<double>   barPos{0.0};     // 1-based bar number
    std::atomic<double>   effBpm{0.0};     // grid bpm * |rate| (sounding bpm)
    std::atomic<int>      syncMode{0};     // SyncMode echo
    std::atomic<float>    syncErrMs{0.0f}; // phase error to master, ms
    std::atomic<bool>     syncLimited{false}; // sync tempo out of pitch range
    std::atomic<bool>     slip{false};
    /// Phase 28 §22 — SlipReturnPosition: where the track would resume
    /// when the last slip ref releases; -1 when slip is off.
    std::atomic<double>   slipShadowSec{-1.0};
    /// Phase 28 — vinyl brake engaged (spin-down/up ramp running).
    std::atomic<bool>     brake{false};

    // ---- stems (Phase 21) ----
    std::atomic<int>      stemMode{0};        // 0 original, 1 stems
    std::atomic<uint32_t> stemsAudible{0};    // bit per rendered stem
    std::atomic<int>      stemsAvail{0};      // stems::StemAvailability
};

} // namespace onda
