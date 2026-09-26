#include "MusicAnalysisEngine.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../decode/IDecoder.h"
#include "BeatGridEngine.h"
#include "BpmAnalyzer.h"
#include "KeyAnalyzer.h"
#include "OnsetDetector.h"
#include "WaveformGenerator.h"

namespace onda {

std::shared_ptr<AnalysisBundle> MusicAnalysisEngine::analyzeTrack(
    const std::string& path, CancelToken cancel, ProgressCb progress) {

    if (cancelled(cancel)) return nullptr;

    auto dec = decoderFactory_ ? decoderFactory_() : nullptr;
    if (!dec) {
        auto b = std::make_shared<AnalysisBundle>();
        b->result.state = AnalysisState::Failed;
        b->result.error = "no decoder available";
        return b;
    }
    // Decode at 48 kHz reference — analysis works on any rate but
    // normalizing keeps bin/hop tables fixed across tracks.
    constexpr double kRate = 48000.0;
    if (!dec->open(path, kRate)) {
        auto b = std::make_shared<AnalysisBundle>();
        b->result.state = AnalysisState::Failed;
        b->result.error = "cannot open or decode: " + path;
        return b;
    }
    if (progress) progress(0.02f);

    std::vector<float> pcm;
    std::vector<float> chunk(8192 * 2);
    while (true) {
        if (cancelled(cancel)) return nullptr;
        const size_t got = dec->readChunk(chunk.data(), 8192);
        if (got == 0) break;
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + (ptrdiff_t)(got * 2));
        if (progress && (pcm.size() % (8192 * 2 * 64)) == 0) progress(0.05f);
    }
    dec->close();
    if (pcm.empty()) {
        auto b = std::make_shared<AnalysisBundle>();
        b->result.state = AnalysisState::Failed;
        b->result.error = "decoded zero frames: " + path;
        return b;
    }
    if (progress) progress(0.15f);
    return analyzeBuffer(pcm.data(), (int64_t)pcm.size() / 2, 2, kRate,
                         cancel);
}

void MusicAnalysisEngine::silenceScan(AnalysisResult& r, const float* samples,
                                      int64_t numFrames, int channels) const {
    // RMS in 10 ms windows; silence = below -60 dBFS for >=0.5 s.
    const double rate = r.sampleRate;
    const int64_t win = (int64_t)(rate * 0.010);
    const int64_t minLen = (int64_t)(rate * 0.5);
    const float thresh = 0.001f; // -60 dB

    int64_t runStart = -1;
    int64_t i = 0;
    for (; i + win <= numFrames; i += win) {
        double acc = 0.0;
        for (int64_t j = i; j < i + win; ++j) {
            float s = 0.0f;
            for (int c = 0; c < channels; ++c)
                s += samples[(size_t)j * (size_t)channels + (size_t)c];
            acc += (double)s * s;
        }
        const float rms = (float)std::sqrt(acc / ((double)win * channels));
        if (rms < thresh) {
            if (runStart < 0) runStart = i;
        } else {
            if (runStart >= 0 && i - runStart >= minLen)
                r.silences.push_back({ runStart, i });
            runStart = -1;
        }
    }
    if (runStart >= 0 && i - runStart >= minLen)
        r.silences.push_back({ runStart, i });

    // Leading/trailing edges: a silence touching the track edge trims it.
    if (r.silences.empty() || r.silences.front().startFrame > win)
        r.firstSoundFrame = 0;
    else
        r.firstSoundFrame = r.silences.front().endFrame;

    r.lastSoundFrame = (!r.silences.empty() &&
                        r.silences.back().endFrame >= numFrames - win)
                           ? r.silences.back().startFrame
                           : numFrames;
}

std::shared_ptr<AnalysisBundle> MusicAnalysisEngine::analyzeBuffer(
    const float* samples, int64_t numFrames, int channels,
    double sampleRate, CancelToken cancel) {

    auto b = std::make_shared<AnalysisBundle>();
    auto& r = b->result;
    r.state = AnalysisState::Analyzing;
    r.numFrames = numFrames;
    r.sampleRate = sampleRate;
    r.durationSec = numFrames / sampleRate;

    if (cancelled(cancel)) return nullptr;

    // ---- onsets + BPM ----
    OnsetDetector od;
    const auto onsets = od.detect(samples, numFrames, channels, sampleRate);
    if (cancelled(cancel)) return nullptr;

    BpmAnalyzer bpm;
    r.bpm = bpm.analyze(onsets);
    if (cancelled(cancel)) return nullptr;

    // ---- key ----
    KeyAnalyzer key;
    r.key = key.analyze(samples, numFrames, channels, sampleRate);
    if (cancelled(cancel)) return nullptr;

    // ---- silence + waveform ----
    silenceScan(r, samples, numFrames, channels);
    WaveformGenerator wg;
    b->waveform = wg.generate(samples, numFrames, channels, sampleRate);
    if (cancelled(cancel)) return nullptr;

    // ---- beatgrid ----
    BeatGridEngine ge;
    b->grid = ge.build(sampleRate, r.bpm, onsets, numFrames);

    r.state = AnalysisState::Analyzed;
    return b;
}

} // namespace onda
