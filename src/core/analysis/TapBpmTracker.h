#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace onda {

/// §24/§56 — manual tap-tempo. Each `tap(nowMs)` records a
/// timestamp; the estimator reports the median inter-tap interval
/// converted to BPM (median is robust against one sloppy tap).
///
/// A gap longer than `resetGapMs` restarts the measurement — a user
/// who stops tapping and resumes later is clearly starting over.
/// Pure state, thread-agnostic — callers feed UI/controller clocks.
class TapBpmTracker {
public:
    explicit TapBpmTracker(int64_t resetGapMs = 2000)
        : resetGapMs_(resetGapMs) {}

    /// Record one tap. Returns the current estimate (0 until ≥2 taps).
    double tap(int64_t nowMs) {
        if (last_ >= 0 && nowMs - last_ > resetGapMs_) {
            taps_.clear();
            last_ = -1;   // re-anchor: the gap itself is not an interval
        }
        if (last_ >= 0) taps_.push_back(nowMs - last_);
        last_ = nowMs;
        if (taps_.size() > kWindow) taps_.erase(taps_.begin());
        return bpm();
    }

    /// Median-interval BPM estimate; 0 when fewer than 2 taps.
    double bpm() const {
        if (taps_.empty()) return 0.0;
        auto v = taps_;
        std::sort(v.begin(), v.end());
        const double med = v[v.size() / 2];
        return med > 0.0 ? 60000.0 / med : 0.0;
    }

    int  tapCount() const { return (int)taps_.size() + (last_ >= 0); }
    void reset() { taps_.clear(); last_ = -1; }

private:
    static constexpr size_t kWindow = 8;
    int64_t resetGapMs_;
    int64_t last_ { -1 };
    std::vector<int64_t> taps_;
};

} // namespace onda
