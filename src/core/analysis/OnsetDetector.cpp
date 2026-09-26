#include "OnsetDetector.h"

#include <algorithm>
#include <cmath>

#include "../dsp/Fft.h"

namespace onda {

OnsetDetector::Result OnsetDetector::detect(const float* samples,
                                            int64_t numFrames,
                                            int channels, double sampleRate) {
    constexpr int kOrder = 10;            // 1024 FFT ≈ 21 ms @ 48 kHz
    constexpr int kHop   = 256;           // ~5.3 ms resolution

    Result r;
    r.hop = kHop;
    r.rate = sampleRate;
    if (samples == nullptr || numFrames <= 0) return r;

    Fft fft;
    fft.prepare(kOrder);
    const int n = fft.size();
    const int bins = n / 2;
    mags_.assign((size_t)bins, 0.0f);
    prevMags_.assign((size_t)bins, 0.0f);

    std::vector<float> mono((size_t)n, 0.0f);
    std::vector<float>& env = r.envelope;

    for (int64_t hop = 0; hop + n <= numFrames; hop += kHop) {
        for (int i = 0; i < n; ++i) {
            float acc = 0.0f;
            for (int c = 0; c < channels; ++c)
                acc += samples[(size_t)(hop + i) * (size_t)channels + (size_t)c];
            mono[(size_t)i] = acc / (float)channels;
        }
        fft.magnitudeSpectrum(mono.data(), n, mags_.data());
        float flux = 0.0f;
        for (int b = 1; b < bins; ++b) {
            const float d = std::log1p(mags_[(size_t)b]) - std::log1p(prevMags_[(size_t)b]);
            if (d > 0.0f) flux += d;
        }
        env.push_back(flux);
        std::swap(mags_, prevMags_);
    }

    // ---- adaptive threshold peak picking ----
    const int count = (int)env.size();
    if (count < 4) return r;
    const int win = 16;                        // ±16 hops ≈ ±85 ms context
    const int minGap = (int)(0.10 * sampleRate / kHop); // ≥100 ms between onsets

    float envMax = 0.0f;
    for (float v : env) envMax = std::max(envMax, v);
    if (envMax <= 0.0f) return r;

    for (int t = 1; t < count - 1; ++t) {
        const float v = env[(size_t)t];
        if (v < envMax * 0.02f) continue;      // noise floor
        // local mean
        float mean = 0.0f; int wn = 0;
        for (int u = std::max(0, t - win); u <= std::min(count - 1, t + win); ++u, ++wn)
            mean += env[(size_t)u];
        mean /= (float)wn;
        if (v < mean * 1.5f) continue;         // local peak prominence
        if (v < env[(size_t)(t - 1)] || v < env[(size_t)(t + 1)]) continue;
        if (!r.onsets.empty() && t - (int)(r.onsets.back().frame / kHop) < minGap)
            continue;
        Onset o;
        o.frame = (int64_t)t * kHop;
        o.strength = v / envMax;
        r.onsets.push_back(o);
    }
    return r;
}

} // namespace onda
