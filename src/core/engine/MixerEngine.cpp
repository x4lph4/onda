#include "MixerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../dsp/CrossfaderCurves.h"
#include "DeckEngine.h"

namespace onda {

MixerEngine::MixerEngine()
    : mixL_(AudioFormat::kMaxBlockFrames, 0.0f),
      mixR_(AudioFormat::kMaxBlockFrames, 0.0f),
      cueL_(AudioFormat::kMaxBlockFrames, 0.0f),
      cueR_(AudioFormat::kMaxBlockFrames, 0.0f) {
    for (int i = 0; i < kMaxDecks; ++i) {
        bufL_[i].assign(AudioFormat::kMaxBlockFrames, 0.0f);
        bufR_[i].assign(AudioFormat::kMaxBlockFrames, 0.0f);
    }
}

void MixerEngine::prepare(double sampleRate) noexcept {
    rate_ = sampleRate;
    limiter_.prepare(sampleRate);
    limiter_.setThreshold(0.891f);
}

void MixerEngine::setFader(int deck, float v) noexcept {
    if (deck >= 0 && deck < kMaxDecks) fader_[deck] = v;
}

void MixerEngine::setCrossfader(float x) noexcept {
    xf_ = std::clamp(x, -1.0f, 1.0f);
}

void MixerEngine::setPfl(int deck, bool on) noexcept {
    if (deck >= 0 && deck < kMaxDecks) pfl_[deck] = on;
}

void MixerEngine::render(float* const* out, int numOut, int n,
                         DeckEngine* decks, int numDecks,
                         rt::AudioStats& stats) noexcept {
    n = std::min(n, (int)AudioFormat::kMaxBlockFrames);

    // 1) decks render their post-chain audio (writes, not adds)
    for (int d = 0; d < numDecks && d < kMaxDecks; ++d)
        decks[d].renderPcm(bufL_[d].data(), bufR_[d].data(), n);

    // 2) sum into buses
    std::memset(mixL_.data(), 0, (size_t)n * sizeof(float));
    std::memset(mixR_.data(), 0, (size_t)n * sizeof(float));
    std::memset(cueL_.data(), 0, (size_t)n * sizeof(float));
    std::memset(cueR_.data(), 0, (size_t)n * sizeof(float));

    const auto curve = (XfCurve)curve_;
    for (int d = 0; d < numDecks && d < kMaxDecks; ++d) {
        const float g = fader_[d] * xfGain(curve, xf_, deckOnSideA(d));
        const bool pfl = pfl_[d];
        const float* bl = bufL_[d].data();
        const float* br = bufR_[d].data();
        for (int i = 0; i < n; ++i) {
            mixL_[i] += bl[i] * g;
            mixR_[i] += br[i] * g;
            if (pfl) { cueL_[i] += bl[i]; cueR_[i] += br[i]; }
        }
    }

    // 3) master chain: smoothed gain -> limiter -> meters -> out
    smMaster_ += 0.05f * (master_ - smMaster_);
    const float mg = smMaster_;
    const bool cue = numOut >= 4 && out[2] != nullptr && out[3] != nullptr;

    float* L = numOut > 0 ? out[0] : nullptr;
    float* R = numOut > 1 ? out[1] : nullptr;
    float peakL = 0.0f, peakR = 0.0f, sumL = 0.0f, sumR = 0.0f;
    uint64_t clipped = 0;

    for (int i = 0; i < n; ++i) {
        const float ml = mixL_[i] * mg, mr = mixR_[i] * mg;
        mixL_[i] = ml; mixR_[i] = mr; // reuse for cue blend + limiter
    }
    if (limiterOn_)
        limiter_.process(mixL_.data(), mixR_.data(), n);
    else
        limiter_.reset();

    if (L != nullptr && R != nullptr) {
        for (int i = 0; i < n; ++i) {
            const float ml = mixL_[i], mr = mixR_[i];
            if (std::fabs(ml) > AudioFormat::kClipThreshold ||
                std::fabs(mr) > AudioFormat::kClipThreshold) ++clipped;
            L[i] = ml; R[i] = mr;
            peakL = std::max(peakL, std::fabs(ml));
            peakR = std::max(peakR, std::fabs(mr));
            sumL += ml * ml; sumR += mr * mr;
        }
        if (clipped > 0) {
            stats.clipCount.fetch_add(clipped, std::memory_order_relaxed);
            mstats_.clipCount.fetch_add(clipped, std::memory_order_relaxed);
        }
    }

    // 4) cue/phones bus (outputs 2-3): blend cue-sum and master
    if (cue) {
        const float cm = cueMix_, cv = cueVol_;
        for (int i = 0; i < n; ++i) {
            out[2][i] = (cueL_[i] * (1.0f - cm) + mixL_[i] * cm) * cv;
            out[3][i] = (cueR_[i] * (1.0f - cm) + mixR_[i] * cm) * cv;
        }
    }
    for (int ch = 4; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            std::memset(out[ch], 0, (size_t)n * sizeof(float));

    // 5) publish meters
    mstats_.peakL.store(peakL, std::memory_order_relaxed);
    mstats_.peakR.store(peakR, std::memory_order_relaxed);
    mstats_.rmsL.store(n > 0 ? std::sqrt(sumL / (float)n) : 0.0f,
                       std::memory_order_relaxed);
    mstats_.rmsR.store(n > 0 ? std::sqrt(sumR / (float)n) : 0.0f,
                       std::memory_order_relaxed);
    mstats_.limiterGain.store(limiter_.lastGainReduction(),
                              std::memory_order_relaxed);
    mstats_.cueAvailable.store(cue, std::memory_order_relaxed);
}

} // namespace onda
