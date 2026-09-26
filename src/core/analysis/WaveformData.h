#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace onda {

/// Cached waveform: min/max/RMS per channel in fixed-size frame bins.
/// Three pyramid levels live in the same object (§7-8):
///   overview — fixed `kOverviewBins` across the whole track
///   mid      — one bin per `kMidFrames` source frames (~21 ms)
///   detail   — one bin per `kDetailFrames` source frames (~5.3 ms)
/// Query by region+bin budget; the UI never regenerates raw data.
struct WaveformBin {
    float min { 0.0f }, max { 0.0f }, rms { 0.0f };
};

class WaveformData {
public:
    static constexpr int kOverviewBins = 4096;
    static constexpr int kMidFrames    = 1024; // ≈21 ms @48 kHz
    static constexpr int kDetailFrames = 256;  // ≈5.3 ms @48 kHz

    int     channels    { 0 };
    int64_t numFrames   { 0 };
    double  sampleRate  { 0.0 };
    std::vector<WaveformBin> overview; // kOverviewBins × channels
    std::vector<WaveformBin> mid;      // frame/1024 bins × channels
    std::vector<WaveformBin> detail;   // frame/256  bins × channels

    bool empty() const noexcept { return overview.empty(); }

    /// Region query: fill `out` with up to `maxBins` bins covering
    /// [startFrame, endFrame). Picks the cheapest pyramid level that
    /// satisfies the requested resolution; aggregates adjacent bins
    /// when the budget is smaller than the raw count.
    /// Which pyramid level a query would use: 0 overview, 1 mid,
    /// 2 detail, -1 when the request can't be served (empty/invalid).
    /// Mirrors the selection inside query() — used by the view layer
    /// for diagnostics without running the query itself (Phase 19 §3).
    int queryLevel(int64_t startFrame, int64_t endFrame,
                   int maxBins) const {
        if (empty() || maxBins <= 0) return -1;
        if (endFrame <= startFrame) return -1;
        startFrame = std::clamp(startFrame, (int64_t)0, numFrames);
        endFrame   = std::clamp(endFrame, startFrame + 1, numFrames);
        const double span = (double)(endFrame - startFrame);
        const auto binsAt = [&](int64_t framesPerBin) -> int64_t {
            return span > 0.0 ? (int64_t)(span / framesPerBin + 0.5) : 0;
        };
        if (binsAt(kDetailFrames) >= 1 &&
            binsAt(kDetailFrames) <= maxBins * 4 && !detail.empty())
            return 2;
        if (binsAt(kMidFrames) >= 1 &&
            binsAt(kMidFrames) <= maxBins * 4 && !mid.empty())
            return 1;
        return 0;
    }

    void query(int64_t startFrame, int64_t endFrame, int maxBins,
               std::vector<WaveformBin>& out, int ch = 0) const {
        out.clear();
        if (empty() || maxBins <= 0 || endFrame <= startFrame) return;
        startFrame = std::clamp(startFrame, (int64_t)0, numFrames);
        endFrame   = std::clamp(endFrame, startFrame + 1, numFrames);
        const double span = (double)(endFrame - startFrame);

        // pick level: enough bins at that level to fill the budget?
        const auto binsAt = [&](int64_t framesPerBin) -> int64_t {
            return span > 0.0 ? (int64_t)(span / framesPerBin + 0.5) : 0;
        };
        const std::vector<WaveformBin>* level = &overview;
        double levelBin = 0.0;   // 0 → proportional (overview)
        if (binsAt(kDetailFrames) >= 1 &&
            binsAt(kDetailFrames) <= maxBins * 4 && !detail.empty()) {
            level = &detail; levelBin = kDetailFrames;
        } else if (binsAt(kMidFrames) >= 1 &&
                   binsAt(kMidFrames) <= maxBins * 4 && !mid.empty()) {
            level = &mid; levelBin = kMidFrames;
        }
        for (int b = 0; b < maxBins; ++b) {
            const double s = startFrame + span * b / maxBins;
            const double e = startFrame + span * (b + 1) / maxBins;
            out.push_back(aggregate(*level, s, e, ch, levelBin));
        }
    }

private:
    WaveformBin aggregate(const std::vector<WaveformBin>& bins,
                          double frameStart, double frameEnd, int ch,
                          double framesPerBin) const {
        WaveformBin r { 0.0f, 0.0f, 0.0f };
        const int64_t nBins = (int64_t)bins.size() / channels;
        if (nBins <= 0) return r;
        // bin index for a frame: fixed levels = frame/framesPerBin,
        // overview = frame * nBins / numFrames (no rounding drift)
        auto idxOf = [&](double f) -> int64_t {
            return framesPerBin > 0.0
                ? (int64_t)(f / framesPerBin)
                : (int64_t)(f * (double)nBins / (double)numFrames);
        };
        const int64_t i0 = std::clamp<int64_t>(idxOf(frameStart), 0, nBins - 1);
        const int64_t i1 = std::clamp<int64_t>(
            std::max<int64_t>(idxOf(frameEnd) - 1, i0), 0, nBins - 1);
        double acc = 0.0; int cnt = 0;
        const int c = std::min(ch, channels - 1);
        for (int64_t i = i0; i <= i1; ++i) {
            const WaveformBin& b = bins[(size_t)i * (size_t)channels + (size_t)c];
            r.min = std::min(r.min, b.min);
            r.max = std::max(r.max, b.max);
            acc += (double)b.rms * b.rms; ++cnt;
        }
        r.rms = cnt > 0 ? (float)std::sqrt(acc / cnt) : 0.0f;
        return r;
    }
};

} // namespace onda
