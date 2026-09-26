#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "AnalysisOverride.h"
#include "AnalysisTypes.h"
#include "BeatGrid.h"
#include "WaveformData.h"

namespace onda {

class IDecoder;
using DecoderFactory = std::function<std::unique_ptr<IDecoder>()>;

/// One complete analysis output. `overrides` is populated by the
/// store/service layer (never the worker) — detected data above,
/// user corrections below, strictly separated (§25).
struct AnalysisBundle {
    AnalysisResult   result;
    BeatGrid         grid;         // detected grid (may be empty)
    WaveformData     waveform;
    AnalysisOverride overrides;    // user edits, if any
    int              version { kAnalysisVersion };

    /// Effective grid for playback/sync/UI.
    BeatGrid effectiveGrid() const {
        return overrides.effectiveGrid(grid);
    }
    double effectiveBpm() const {
        return overrides.effectiveBpm(result.bpm.bpm);
    }
};

/// Orchestrates the analysis pipeline over a fully-decoded track.
/// Runs on caller's thread (queue workers or tests) — never on RT.
///
/// Stage callbacks report progress 0..1; the cancel token is polled
/// between stages and inside the long STFT loops via the analyzers'
/// own chunked processing.
class MusicAnalysisEngine {
public:
    explicit MusicAnalysisEngine(DecoderFactory factory)
        : decoderFactory_(std::move(factory)) {}

    using CancelToken = std::shared_ptr<std::atomic<bool>>;
    using ProgressCb  = std::function<void(float)>;

    /// Decode + analyze `path`. Returns nullptr only on cancel;
    /// failures return a bundle with result.state == Failed + error.
    std::shared_ptr<AnalysisBundle> analyzeTrack(
        const std::string& path,
        CancelToken cancel = nullptr,
        ProgressCb progress = nullptr);

    /// Direct buffer API (tests / future streaming sources).
    std::shared_ptr<AnalysisBundle> analyzeBuffer(
        const float* samples, int64_t numFrames, int channels,
        double sampleRate, CancelToken cancel = nullptr);

private:
    bool cancelled(const CancelToken& c) const {
        return c != nullptr && c->load();
    }
    void silenceScan(AnalysisResult& r, const float* samples,
                     int64_t numFrames, int channels) const;

    DecoderFactory decoderFactory_;
};

} // namespace onda
