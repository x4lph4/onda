#include "FeedbackEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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

/// Blink rate in Hz for each blink pattern (§42). Phase is computed
/// from wall-clock so the rate stays exact regardless of tick cadence.
static double blinkHz(FeedbackEngine::LedPattern p) {
    using LP = FeedbackEngine::LedPattern;
    switch (p) {
        case LP::Blink:     return 2.0;
        case LP::SlowBlink: return 0.5;
        case LP::FastBlink: return 4.0;
        default:            return 0.0;
    }
}

int FeedbackEngine::tick(double nowSec, bool force) {
    int emitted = 0;
    for (const Watch& w : watches_) {
        // resolve current source value → output byte
        uint8_t v;
        bool srcOn = false;      // for blink patterns
        if (w.followValue && ports_.paramState) {
            // note: paramId 0 is a *valid* id (channel 0 gain)
            const float span = w.srcMax - w.srcMin;
            const float x = std::clamp(
                (ports_.paramState(w.paramId) - w.srcMin) /
                    (span != 0.0f ? span : 1.0f),
                0.0f, 1.0f);
            srcOn = x > 0.5f;
            v = (uint8_t)std::clamp<int>(
                (int)(w.offValue + x * (w.onValue - w.offValue)),
                0, 127);
        } else if (w.cmd != 0 && ports_.commandState) {
            const int s = ports_.commandState(w.cmd);
            srcOn = s > 0;
            v = srcOn ? w.onValue : w.offValue;
        } else {
            continue;
        }

        // §42 blink: while the source is on, the output alternates
        // between onValue/offValue at the pattern rate. Off = solid.
        if (srcOn) {
            const double hz = blinkHz(w.pattern);
            if (hz > 0.0) {
                const double phase =
                    nowSec * hz - std::floor(nowSec * hz);
                v = phase < 0.5 ? w.onValue : w.offValue;
            }
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
