#pragma once

#include <deque>
#include <string>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Callback health (§20-23). Fed once per UI tick with the engine's
/// atomic counters — measures nothing in the RT callback itself.
/// Budget = buffer/rate; a callback over budget is an underrun.
/// Warnings are deduplicated (one per class per 5 s) so the user is
/// informed, not saturated.
class AudioHealthMonitor {
public:
    /// `cbWorstMs`/`xruns`/`cpu` come straight from rt::AudioStats.
    void feed(int64_t nowMs, double rate, int buffer,
              double cbWorstMs, double cbAvgMs, uint64_t xruns,
              float cpu, int64_t underrunStampMs) {
        budgetMs_ = rate > 0.0 ? buffer / rate * 1000.0 : 0.0;
        worstMs_ = cbWorstMs; avgMs_ = cbAvgMs;
        cpu_ = cpu; xruns_ = xruns;
        if (underrunStampMs != lastUnderrunTs_) {
            ++underruns_;
            lastUnderrunTs_ = underrunStampMs;
            push({underrunStampMs, cbWorstMs, budgetMs_, cpu, buffer});
        }
        const bool overload = budgetMs_ > 0.0 && cpu > 0.85f;
        if (overload) warn(nowMs, Warn::Overload);
        if (xruns_ > xrunsWarned_) {
            xrunsWarned_ = xruns_;
            warn(nowMs, Warn::Xrun);
        }
    }

    HealthReport report() const {
        HealthReport r;
        r.underruns = underruns_; r.xruns = xruns_;
        r.cbBudgetMs = budgetMs_; r.cbWorstMs = worstMs_;
        r.cbAvgMs = avgMs_; r.cpu = cpu_;
        r.overload = budgetMs_ > 0.0 && cpu_ > 0.85f;
        for (const auto& e : recent_) r.recent.push_back(e);
        return r;
    }

    /// Pending warning text, once per pull ("" when none).
    std::string popWarning() {
        std::string w = std::move(pending_); pending_.clear(); return w;
    }
    bool hasWarnings() const { return !pending_.empty(); }

private:
    enum class Warn { Overload, Xrun };
    void warn(int64_t now, Warn w) {
        const char* msg = w == Warn::Overload
            ? "DSP load above 85% of the callback budget — increase the "
              "buffer size or reduce active FX"
            : "Audio underrun detected — scheduling gap between "
              "callbacks; increase buffer size if it persists";
        if (now - lastWarnMs_ < 5000 && w == lastWarn_) return;
        lastWarnMs_ = now; lastWarn_ = w;
        pending_ = msg;
    }
    void push(UnderrunEvent e) {
        recent_.push_back(e);
        if (recent_.size() > 32) recent_.pop_front();
    }

    double   budgetMs_ = 0.0, worstMs_ = 0.0, avgMs_ = 0.0;
    float    cpu_ = 0.0f;
    uint64_t underruns_ = 0, xruns_ = 0, xrunsWarned_ = 0;
    int64_t  lastUnderrunTs_ = 0;
    std::deque<UnderrunEvent> recent_;
    std::string pending_;
    int64_t  lastWarnMs_ = 0;
    Warn     lastWarn_ = Warn::Xrun;
};

} // namespace onda::adevice
