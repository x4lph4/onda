#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace onda {

/// Cached waveform: min/max/RMS per channel in fixed-size frame bins.
/// Two resolutions live in the same object:
///   overview — fixed `kOverviewBins` across the whole track
///   detail   — one bin per `kDetailFrames` source frames
/// Query by region+bin budget; the UI never regenerates raw data.
struct WaveformBin {
    float min { 0.0f }, max { 0.0f }, rms { 0.0f };
};

class WaveformData {
public:
    static constexpr int kOverviewBins = 4096;
    static constexpr int kDetailFrames = 256; // ≈5.3 ms @48 kHz

    int     channels    { 0 };
    int64_t numFrames   { 0 };
    double  sampleRate  { 0.0 };
    std::vector<WaveformBin> overview; // kOverviewBins × channels
    std::vector<WaveformBin> detail;   // nBins     × channels

    bool empty() const noexcept { return overview.empty(); }

    /// Region query: fill `out` with up to `maxBins` bins covering
    /// [startFrame, endFrame). Picks overview or detail depending on the
    /// requested resolution; aggregates adjacent detail bins when the
    /// budget is smaller than the raw count.
    void query(int64_t startFrame, int64_t endFrame, int maxBins,
               std::vector<WaveformBin>& out, int ch = 0) const {
        out.clear();
        if (empty() || maxBins <= 0 || endFrame <= startFrame) return;
        startFrame = std::clamp(startFrame, (int64_t)0, numFrames);
        endFrame   = std::clamp(endFrame, startFrame + 1, numFrames);
        const double span = (double)(endFrame - startFrame);

        // decide level: enough detail bins to satisfy the budget?
        const int64_t detBins = span > 0.0
            ? (int64_t)(span / kDetailFrames + 0.5) : 0;
        const bool useDetail = detBins >= 1 && detBins <= maxBins * 4 &&
                               !detail.empty();

        if (!useDetail) {
            // overview path: bins are evenly spaced across whole track
            for (int b = 0; b < maxBins; ++b) {
                const double s = startFrame + span * b / maxBins;
                const double e = startFrame + span * (b + 1) / maxBins;
                out.push_back(aggregate(overview, s, e, ch));
            }
        } else {
            for (int b = 0; b < maxBins; ++b) {
                const double s = startFrame + span * b / maxBins;
                const double e = startFrame + span * (b + 1) / maxBins;
                out.push_back(aggregate(detail, s, e, ch));
            }
        }
    }

private:
    WaveformBin aggregate(const std::vector<WaveformBin>& bins,
                          double frameStart, double frameEnd, int ch) const {
        WaveformBin r { 0.0f, 0.0f, 0.0f };
        const int64_t nBins = (int64_t)bins.size() / channels;
        if (nBins <= 0) return r;
        const bool isDetail = &bins == &detail;
        // bin index for a frame: detail = frame/kDetailFrames,
        // overview = frame * nBins / numFrames (no rounding drift)
        auto idxOf = [&](double f) -> int64_t {
            return isDetail ? (int64_t)(f / kDetailFrames)
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
