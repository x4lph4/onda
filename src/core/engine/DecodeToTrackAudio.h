#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../audio/TrackAudio.h"
#include "TrackLoader.h"   // DecoderFactory

namespace onda {

/// Decode a whole file into a TrackAudio (device-rate interleaved
/// stereo) — the same IDecoder path TrackLoader uses, factored so
/// non-deck consumers (stem separation, offline analysis, tests)
/// share it instead of duplicating the chunk loop.
inline std::unique_ptr<TrackAudio> decodeToTrackAudio(
    const std::string& path, double deviceRate,
    const DecoderFactory& factory) {
    auto dec = factory();
    if (!dec || !dec->open(path, deviceRate)) return nullptr;
    const auto info = dec->info();

    auto track = std::make_unique<TrackAudio>();
    track->path = path;
    track->title = info.title;
    track->artist = info.artist;
    track->fileSampleRate = info.sampleRate;
    track->deviceRate = deviceRate;
    track->channels = 2;
    if (info.totalFrames > 0) {
        const double secs = info.totalFrames / info.sampleRate;
        track->samples.reserve((size_t)(secs * deviceRate * 2.0 * 1.05));
    }

    constexpr size_t kChunk = 16384;
    std::vector<float> scratch(kChunk * 2);
    int64_t frames = 0;
    for (;;) {
        const size_t got = dec->readChunk(scratch.data(), kChunk);
        if (got == 0) break;
        track->samples.insert(track->samples.end(), scratch.begin(),
                              scratch.begin() + (ptrdiff_t)got * 2);
        frames += (int64_t)got;
    }
    track->numFrames = frames;
    return frames > 0 ? std::move(track) : nullptr;
}

} // namespace onda
