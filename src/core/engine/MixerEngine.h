#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "../rt/AudioStats.h"

namespace onda {

class DeckEngine;

/// RT mix bus: clears scratch buffers, pulls every deck in, applies
/// smoothed master gain, counts clipped samples, writes the device outs.
class MixerEngine {
public:
    static constexpr size_t kMaxBlockFrames = 8192; // == DeckEngine::kMaxBlockFrames

    MixerEngine();

    void setMasterGain(float g) noexcept      { masterGain_.store(g); }
    void setLimiterEnabled(bool on) noexcept  { limiter_.store(on); }

    /// RT. Writes up to numOut output channels (stereo pair used).
    void render(float* const* out, int numOut, int numSamples,
                DeckEngine* decks, size_t numDecks,
                rt::AudioStats& stats) noexcept;

private:
    std::atomic<float> masterGain_{1.0f};
    std::atomic<bool>  limiter_{true};
    float smoothedMaster_{1.0f};
    std::vector<float> mixL_, mixR_;
};

} // namespace onda
