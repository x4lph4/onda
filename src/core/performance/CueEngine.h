#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "../analysis/BeatGrid.h"
#include "PerformanceTypes.h"
#include "QuantizeEngine.h"

namespace onda {

/// Per-deck cue storage + trigger math. Pure logic — the facade turns
/// results into commands. Persistence happens in PerformanceStore.
class CueEngine {
public:
    static constexpr int kMaxHotCues = 8;

    const std::vector<Cue>& cues() const noexcept { return cues_; }

    const Cue* get(int slot) const noexcept {
        for (auto& c : cues_) if (c.slot == slot) return &c;
        return nullptr;
    }

    /// Insert or replace a cue at `slot`. `frame2` = loop-out for loop
    /// cues (-1 otherwise).
    Cue setCue(int slot, CueType type, int64_t frame, const BeatGrid* grid,
               int64_t frame2 = -1, uint32_t argb = 0xFFFF4444,
               std::string label = {}, int64_t createdAt = 0) {
        Cue c;
        c.slot = slot; c.type = type; c.frame = frame; c.frame2 = frame2;
        c.beat  = grid ? grid->frameToBeat(frame) : -1.0;
        c.beat2 = (grid && frame2 >= 0) ? grid->frameToBeat(frame2) : -1.0;
        c.argb = argb; c.label = std::move(label); c.createdAt = createdAt;
        for (auto& e : cues_)
            if (e.slot == slot) { e = c; return c; }
        cues_.push_back(c);
        std::sort(cues_.begin(), cues_.end(),
                  [](const Cue& a, const Cue& b) { return a.slot < b.slot; });
        return c;
    }

    bool deleteCue(int slot) {
        const auto it = std::remove_if(cues_.begin(), cues_.end(),
            [slot](const Cue& c) { return c.slot == slot; });
        const bool removed = it != cues_.end();
        cues_.erase(it, cues_.end());
        return removed;
    }

    bool updateCue(int slot, uint32_t argb, const std::string& label) {
        for (auto& c : cues_)
            if (c.slot == slot) { c.argb = argb; c.label = label; return true; }
        return false;
    }

    /// §4 MOVE — reposition the cue; `beat` re-derives through the grid
    /// so quantize re-resolution stays musical.
    bool moveCue(int slot, int64_t frame, const BeatGrid* grid) {
        for (auto& c : cues_)
            if (c.slot == slot) {
                c.frame = frame;
                c.beat = grid ? grid->frameToBeat(frame) : -1.0;
                return true;
            }
        return false;
    }
    bool setKind(int slot, HotKind kind) {
        for (auto& c : cues_)
            if (c.slot == slot) { c.kind = kind; return true; }
        return false;
    }
    bool setEnabled(int slot, bool on) {
        for (auto& c : cues_)
            if (c.slot == slot) { c.enabled = on; return true; }
        return false;
    }
    bool setQuantized(int slot, bool on) {
        for (auto& c : cues_)
            if (c.slot == slot) { c.quantized = on; return true; }
        return false;
    }
    bool setComment(int slot, std::string text) {
        for (auto& c : cues_)
            if (c.slot == slot) { c.comment = std::move(text); return true; }
        return false;
    }
    /// §2 — first slot not occupied by any cue of `type` (model
    /// unbounded; `limit` is the caller's display convention).
    int firstFreeSlot(CueType type, int limit) const {
        for (int s = 0; s < limit; ++s) {
            bool used = false;
            for (auto& c : cues_) if (c.slot == s && c.type == type)
                { used = true; break; }
            if (!used) return s;
        }
        return -1;
    }

    void clearAll() { cues_.clear(); }
    void load(const std::vector<Cue>& cues) { cues_ = cues; }

    /// Trigger semantics:
    ///  - Hot cue: jump + play (returns target frame).
    ///  - Memory cue: navigate only (returns target, caller decides).
    ///  - Loop cue: returns in-frame; out-frame via `loopOut`.
    /// `quantizeOn` snaps the target to the nearest beat boundary.
    std::optional<int64_t> trigger(int slot, const BeatGrid* grid,
                                   bool quantizeOn,
                                   double quantumBeats = 1.0,
                                   int64_t* loopOut = nullptr) const {
        const Cue* c = get(slot);
        if (!c || !c->enabled) return std::nullopt;
        int64_t target = c->frame;
        // §2 per-cue opt-out: quantized == false skips grid snapping.
        if (quantizeOn && c->quantized && grid && !grid->empty()) {
            // Re-resolve from the stored musical position so the cue
            // stays correct after manual beatgrid edits.
            const double beat = c->beat >= 0.0 ? c->beat
                                               : grid->frameToBeat(c->frame);
            target = QuantizeEngine::snap(*grid, grid->beatToFrame(beat),
                                          quantumBeats);
        }
        if (loopOut && c->type == CueType::Loop) {
            if (quantizeOn && c->quantized && grid && !grid->empty()
                && c->beat2 >= 0.0)
                *loopOut = QuantizeEngine::snap(*grid,
                                                grid->beatToFrame(c->beat2),
                                                quantumBeats);
            else
                *loopOut = c->frame2;
        }
        return target;
    }

private:
    std::vector<Cue> cues_;
};

} // namespace onda
