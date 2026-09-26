#include "MixerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "DeckEngine.h"

namespace onda {

MixerEngine::MixerEngine()
    : mixL_(kMaxBlockFrames, 0.0f), mixR_(kMaxBlockFrames, 0.0f) {}

void MixerEngine::render(float* const* out, int numOut, int numSamples,
                         DeckEngine* decks, size_t numDecks,
                         rt::AudioStats& stats) noexcept {
    const int n = std::min(numSamples, (int)kMaxBlockFrames);

    std::memset(mixL_.data(), 0, (size_t)n * sizeof(float));
    std::memset(mixR_.data(), 0, (size_t)n * sizeof(float));

    for (size_t d = 0; d < numDecks; ++d)
        decks[d].addToMix(mixL_.data(), mixR_.data(), n);

    const float target = masterGain_.load(std::memory_order_acquire);
    smoothedMaster_ += 0.05f * (target - smoothedMaster_); // per-block smooth
    const float g = smoothedMaster_;
    const bool lim = limiter_.load(std::memory_order_acquire);

    uint64_t clipped = 0;
    if (numOut >= 2 && out[0] != nullptr && out[1] != nullptr) {
        float* L = out[0];
        float* R = out[1];
        for (int i = 0; i < n; ++i) {
            float l = mixL_[i] * g;
            float r = mixR_[i] * g;
            if (lim) { // cheap safety clip — real limiter comes with FX engine
                l = std::clamp(l, -1.0f, 1.0f);
                r = std::clamp(r, -1.0f, 1.0f);
            }
            if (std::fabs(l) > 1.0f || std::fabs(r) > 1.0f) ++clipped;
            L[i] = l;
            R[i] = r;
        }
        if (clipped > 0)
            stats.clipCount.fetch_add(clipped, std::memory_order_relaxed);
    }
    for (int ch = 2; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            std::memset(out[ch], 0, (size_t)n * sizeof(float));
}

} // namespace onda
