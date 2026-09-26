#include "BeatGridEngine.h"

#include <algorithm>
#include <cmath>

namespace onda {

BeatGrid BeatGridEngine::build(double rate, const BpmResult& bpm,
                               const OnsetDetector::Result& onsets,
                               int64_t numFrames) const {
    BeatGrid g;
    g.setRate(rate);
    if (bpm.bpm <= 0.0 || numFrames <= 0) return g;

    // ---- tempo segments ----
    if (bpm.variable && !bpm.tempoMap.empty()) {
        g.segments = bpm.tempoMap;
        // segments from the analyser carry startFrame but not the
        // aligned beat numbers — compute cumulative beat indices.
        int64_t beats = 0;
        for (size_t i = 0; i + 1 < g.segments.size(); ++i) {
            auto& s = g.segments[i];
            s.startBeat = beats;
            const double secs = (g.segments[i + 1].startFrame - s.startFrame) / rate;
            beats += (int64_t)llround(secs * s.bpm / 60.0);
        }
        g.segments.back().startBeat = beats;
    } else {
        TempoSegment s; s.startFrame = 0; s.startBeat = 0; s.bpm = bpm.bpm;
        g.segments = { s };
    }

    // ---- anchor: snap grid start to the first strong onset ----
    const double fpb = g.framesPerBeat(g.segments.front());
    int64_t anchor = g.segments.front().startFrame;
    if (!onsets.onsets.empty()) {
        const int64_t cand = onsets.onsets.front().frame;
        // keep anchor within one beat of the nominal start
        if (cand >= 0 && cand < anchor + (int64_t)fpb * 2)
            anchor = cand;
    }
    g.firstBeatFrame = anchor;
    // rebase segments so beat 0 == anchor
    for (auto& s : g.segments) s.startFrame -= anchor;
    g.firstBeatFrame = 0;
    for (auto& s : g.segments) s.startFrame += anchor;

    // ---- downbeat phase: strongest beat position wins ----
    if (!onsets.onsets.empty()) {
        const int bpb = g.beatsPerBar;
        std::vector<double> score((size_t)bpb, 0.0);
        for (const auto& o : onsets.onsets) {
            const double b = g.frameToBeat(o.frame);
            if (b < 0.0) continue;
            const int idx = (int)llround(b) % bpb;
            score[(size_t)(idx < 0 ? idx + bpb : idx)] += o.strength;
        }
        int best = 0;
        for (int i = 1; i < bpb; ++i)
            if (score[(size_t)i] > score[(size_t)best]) best = i;
        g.downbeatOffset = best;
    }
    return g;
}

void BeatGridEngine::setFirstBeat(BeatGrid& g, int64_t frame) {
    moveBeatGrid(g, frame - (g.segments.empty() ? 0 : g.segments.front().startFrame));
}

void BeatGridEngine::moveBeatGrid(BeatGrid& g, int64_t deltaFrames) {
    g.firstBeatFrame += deltaFrames;
    for (auto& s : g.segments) s.startFrame += deltaFrames;
}

void BeatGridEngine::nudgeBeatGrid(BeatGrid& g, double deltaMs) {
    moveBeatGrid(g, (int64_t)llround(deltaMs * 0.001 * g.rate()));
}

void BeatGridEngine::adjustBpm(BeatGrid& g, double newBpm) {
    if (g.segments.empty() || newBpm <= 0.0) return;
    const double ratio = g.segments.front().bpm / newBpm;
    // constant-tempo grid: scale all frame offsets around beat 0.
    if (g.segments.size() == 1) {
        g.segments.front().bpm = newBpm;
        return;
    }
    // multi-segment: scale each segment's start and keep beat indices.
    const int64_t base = g.firstBeatFrame;
    for (auto& s : g.segments) {
        s.startFrame = base + (int64_t)llround((s.startFrame - base) * ratio);
        s.bpm = newBpm;
    }
}

void BeatGridEngine::setBeatsPerBar(BeatGrid& g, int bpb) {
    if (bpb >= 1 && bpb <= 16) g.beatsPerBar = bpb;
}

void BeatGridEngine::setDownbeat(BeatGrid& g, int beatIndex) {
    g.downbeatOffset = beatIndex;
}

void BeatGridEngine::setTempoSegment(BeatGrid& g, size_t index, double bpm) {
    if (index >= g.segments.size() || bpm <= 0.0) return;
    const double oldBpm = g.segments[index].bpm;
    g.segments[index].bpm = bpm;
    if (index + 1 < g.segments.size() && oldBpm > 0.0) {
        // re-derive subsequent startFrames from the new tempo
        const double fpbOld = g.rate() * 60.0 / oldBpm;
        const double fpbNew = g.rate() * 60.0 / bpm;
        for (size_t i = index + 1; i < g.segments.size(); ++i) {
            auto& s = g.segments[i];
            const double beatsBack = s.startBeat - g.segments[i - 1].startBeat;
            s.startFrame = g.segments[i - 1].startFrame +
                           (int64_t)llround(beatsBack * fpbNew);
        }
        (void)fpbOld;
    }
}

bool BeatGridEngine::addTempoChange(BeatGrid& g, int64_t frame, double bpm) {
    if (bpm <= 0.0 || g.empty()) return false;
    // insert sorted by frame; beat index = grid's musical pos at frame
    const double beat = g.frameToBeat(frame);
    if (beat < 0.0) return false;
    TempoSegment seg;
    seg.startFrame = frame;
    seg.startBeat  = (int64_t)llround(beat);
    seg.bpm        = bpm;
    auto it = std::find_if(g.segments.begin(), g.segments.end(),
                           [&](const TempoSegment& s) { return s.startFrame > frame; });
    g.segments.insert(it, seg);
    return true;
}

bool BeatGridEngine::removeTempoChange(BeatGrid& g, size_t index) {
    if (index == 0 || index >= g.segments.size()) return false; // keep base
    g.segments.erase(g.segments.begin() + (ptrdiff_t)index);
    return true;
}

void BeatGridEngine::resetTo(BeatGrid& g, const BeatGrid& source) {
    g = source;
}

} // namespace onda
