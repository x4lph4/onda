#pragma once

#include <atomic>
#include <vector>

#include "../model/DeckStats.h"

namespace onda {

class AudioFifo;

/// RT-side deck. Pulls device-rate stereo audio from its streamer's fifo
/// and adds it (gain-staged, level-metered) into the stereo mix bus.
/// Setters are atomic — callable from any thread. addToMix() is RT-only.
class DeckEngine {
public:
    static constexpr size_t kMaxBlockFrames = 8192;

    DeckEngine();

    /// Setup only (engine start); never on the RT path.
    void bind(AudioFifo* fifo, DeckStats* stats) noexcept;
    void setDeviceRate(double rate) noexcept;

    void setPlaying(bool playing) noexcept;
    void setGain(float gain) noexcept;
    bool isPlaying() const noexcept { return playing_.load(std::memory_order_acquire); }

    /// RT. Adds this deck's output into the mix buffers (pre-cleared).
    void addToMix(float* mixL, float* mixR, int numSamples) noexcept;

private:
    AudioFifo* fifo_   { nullptr };
    DeckStats* stats_  { nullptr };
    double     deviceRate_ { 48000.0 };

    std::atomic<bool>  playing_{false};
    std::atomic<float> gain_{1.0f};
    float smoothedGain_{1.0f};

    std::vector<float> scratch_; // interleaved, kMaxBlockFrames * 2
};

} // namespace onda
