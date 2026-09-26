#include "WaveformGenerator.h"

#include <algorithm>
#include <cmath>

namespace onda {

WaveformData WaveformGenerator::generate(const float* samples,
                                         int64_t numFrames,
                                         int channels, double sampleRate) {
    WaveformData w;
    w.channels = channels;
    w.numFrames = numFrames;
    w.sampleRate = sampleRate;
    if (samples == nullptr || numFrames <= 0 || channels < 1) return w;

    const int64_t nDet = (numFrames + WaveformData::kDetailFrames - 1)
                         / WaveformData::kDetailFrames;
    w.detail.resize((size_t)nDet * (size_t)channels);
    w.overview.resize((size_t)WaveformData::kOverviewBins * (size_t)channels);

    // ---- detail pass ----
    for (int64_t b = 0; b < nDet; ++b) {
        const int64_t s = b * WaveformData::kDetailFrames;
        const int64_t e = std::min<int64_t>(s + WaveformData::kDetailFrames, numFrames);
        for (int c = 0; c < channels; ++c) {
            WaveformBin& bin = w.detail[(size_t)b * (size_t)channels + (size_t)c];
            bin.min = 1.0f; bin.max = -1.0f;
            double acc = 0.0;
            for (int64_t i = s; i < e; ++i) {
                const float v = samples[(size_t)i * (size_t)channels + (size_t)c];
                bin.min = std::min(bin.min, v);
                bin.max = std::max(bin.max, v);
                acc += (double)v * v;
            }
            bin.rms = e > s ? (float)std::sqrt(acc / (double)(e - s)) : 0.0f;
        }
    }

    // ---- mid level: aggregate detail 4:1 (1024-frame bins) ----
    const int64_t detPerMid =
        WaveformData::kMidFrames / WaveformData::kDetailFrames;
    const int64_t nMid = (nDet + detPerMid - 1) / detPerMid;
    w.mid.resize((size_t)nMid * (size_t)channels);
    for (int64_t b = 0; b < nMid; ++b) {
        const int64_t d0 = b * detPerMid;
        const int64_t d1 = std::min<int64_t>(nDet, d0 + detPerMid);
        for (int c = 0; c < channels; ++c) {
            WaveformBin& mb = w.mid[(size_t)b * (size_t)channels + (size_t)c];
            mb.min = 1.0f; mb.max = -1.0f;
            double acc = 0.0; int cnt = 0;
            for (int64_t i = d0; i < d1; ++i) {
                const WaveformBin& db = w.detail[(size_t)i * (size_t)channels + (size_t)c];
                mb.min = std::min(mb.min, db.min);
                mb.max = std::max(mb.max, db.max);
                acc += (double)db.rms * db.rms; ++cnt;
            }
            mb.rms = cnt > 0 ? (float)std::sqrt(acc / cnt) : 0.0f;
        }
    }

    // ---- overview: aggregate detail bins ----
    const double detPerOv = (double)numFrames /
                            (WaveformData::kOverviewBins * WaveformData::kDetailFrames);
    for (int b = 0; b < WaveformData::kOverviewBins; ++b) {
        const int64_t d0 = (int64_t)(b * detPerOv);
        const int64_t d1 = std::min<int64_t>(nDet, (int64_t)((b + 1) * detPerOv + 0.999));
        for (int c = 0; c < channels; ++c) {
            WaveformBin& ob = w.overview[(size_t)b * (size_t)channels + (size_t)c];
            ob.min = 1.0f; ob.max = -1.0f;
            double acc = 0.0; int cnt = 0;
            for (int64_t i = d0; i < d1; ++i) {
                const WaveformBin& db = w.detail[(size_t)i * (size_t)channels + (size_t)c];
                ob.min = std::min(ob.min, db.min);
                ob.max = std::max(ob.max, db.max);
                acc += (double)db.rms * db.rms; ++cnt;
            }
            ob.rms = cnt > 0 ? (float)std::sqrt(acc / cnt) : 0.0f;
        }
    }
    return w;
}

} // namespace onda
