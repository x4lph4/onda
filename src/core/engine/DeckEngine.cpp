#include "DeckEngine.h"

#include <algorithm>
#include <cmath>

#include "../audio/AudioFifo.h"

namespace onda {

DeckEngine::DeckEngine() : scratch_(kMaxBlockFrames * 2, 0.0f) {}

void DeckEngine::bind(AudioFifo* fifo, DeckStats* stats) noexcept {
    fifo_  = fifo;
    stats_ = stats;
}

void DeckEngine::setDeviceRate(double rate) noexcept { deviceRate_ = rate; }

void DeckEngine::setPlaying(bool playing) noexcept {
    playing_.store(playing, std::memory_order_release);
    if (stats_) {
        const int cur = stats_->state.load(std::memory_order_acquire);
        if (cur == (int)DeckState::Ready || cur == (int)DeckState::Playing ||
            cur == (int)DeckState::Paused)
            stats_->state.store(playing ? (int)DeckState::Playing
                                        : (int)DeckState::Paused);
    }
}

void DeckEngine::setGain(float gain) noexcept {
    gain_.store(gain, std::memory_order_release);
}

void DeckEngine::addToMix(float* mixL, float* mixR, int numSamples) noexcept {
    if (!playing_.load(std::memory_order_acquire) || fifo_ == nullptr)
        return;

    const int n = std::min(numSamples, (int)kMaxBlockFrames);
    const size_t got = fifo_->read(scratch_.data(), (size_t)n);

    if (got < (size_t)n && stats_)
        stats_->underruns.fetch_add(1, std::memory_order_relaxed);

    const float target = gain_.load(std::memory_order_acquire);
    constexpr float kSmooth = 0.001f; // per-sample gain smoothing (click-free)
    float g = smoothedGain_;
    float peakL = 0.0f, peakR = 0.0f;

    for (size_t i = 0; i < got; ++i) {
        g += kSmooth * (target - g);
        const float l = scratch_[i * 2]     * g;
        const float r = scratch_[i * 2 + 1] * g;
        mixL[i] += l;
        mixR[i] += r;
        peakL = std::max(peakL, std::fabs(l));
        peakR = std::max(peakR, std::fabs(r));
    }
    smoothedGain_ = g;

    if (stats_) {
        stats_->levelL.store(peakL, std::memory_order_relaxed);
        stats_->levelR.store(peakR, std::memory_order_relaxed);
        const int64_t frames = stats_->framesRead.fetch_add(
                                   (int64_t)got, std::memory_order_relaxed) + (int64_t)got;
        stats_->positionSec.store(frames / deviceRate_, std::memory_order_relaxed);
    }
}

} // namespace onda
