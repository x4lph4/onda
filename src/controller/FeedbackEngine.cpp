#include "FeedbackEngine.h"

#include <algorithm>
#include <chrono>

namespace onda::ctrl {

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FeedbackEngine::setWatches(std::vector<Watch> w) {
    watches_ = std::move(w);
    lastSent_.clear();
    recentOut_.clear();
}

int FeedbackEngine::addWatch(const Watch& w) {
    watches_.push_back(w);
    return (int)watches_.size() - 1;
}

void FeedbackEngine::noteSent(const MidiAddr& a, uint8_t v,
                              int64_t timeUs) {
    recentOut_[a.pack()] = { v, timeUs };
    ++sent_;
}

bool FeedbackEngine::isEcho(const MidiAddr& in, uint8_t value,
                            int64_t atUs) const {
    auto it = recentOut_.find(in.pack());
    if (it == recentOut_.end()) return false;
    const int64_t age = atUs - it->second.us;
    const bool echo =
        age >= 0 && age < (int64_t)(echoWindowSec_ * 1e6) &&
        it->second.v == value;
    if (echo) ++suppressed_;
    return echo;
}

int FeedbackEngine::tick(double nowSec, bool force) {
    int emitted = 0;
    for (const Watch& w : watches_) {
        // resolve current source value → output byte
        uint8_t v;
        if (w.followValue && ports_.paramState) {
            // note: paramId 0 is a *valid* id (channel 0 gain)
            const float span = w.srcMax - w.srcMin;
            const float x = std::clamp(
                (ports_.paramState(w.paramId) - w.srcMin) /
                    (span != 0.0f ? span : 1.0f),
                0.0f, 1.0f);
            v = (uint8_t)std::clamp<int>(
                (int)(w.offValue + x * (w.onValue - w.offValue)),
                0, 127);
        } else if (w.cmd != 0 && ports_.commandState) {
            const int s = ports_.commandState(w.cmd);
            v = s > 0 ? w.onValue : w.offValue;
        } else {
            continue;
        }

        const uint32_t key = w.out.pack();
        Last& l = lastSent_[key];
        const bool changed = v != l.v;
        const bool throttled = nowSec - l.t < w.minIntervalSec;
        if (!force && (!changed || throttled)) continue;

        l.v = v; l.t = nowSec;
        if (ports_.send) {
            ports_.send(w.out, v);
            noteSent(w.out, v, nowUs());
            ++emitted;
        }
    }
    return emitted;
}

} // namespace onda::ctrl
