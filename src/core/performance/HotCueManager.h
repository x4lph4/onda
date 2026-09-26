#pragma once

#include <vector>

#include "CueEngine.h"

namespace onda {

/// §2-6 — named per-deck hot-cue manager. Thin typed facade over
/// CueEngine restricted to type == Hot (+ Loop cues stored in the hot
/// slots — a loop cue is fired FROM a pad, so it lives in pad space).
/// Memory cues are handled by MemoryCueManager; navigation-only types
/// never appear here.
///
/// Storage is unbounded — kMaxHotCues is only the DISPLAY/pad
/// convention (8 pads), not a model limit (§2).
class HotCueManager {
public:
    static constexpr int kPads = 8; // display convention, not a cap

    explicit HotCueManager(CueEngine& e) : e_(e) {}

    const Cue* get(int slot) const {
        const Cue* c = e_.get(slot);
        return (c && c->type != CueType::Memory) ? c : nullptr;
    }
    std::vector<const Cue*> list() const {
        std::vector<const Cue*> out;
        for (auto& c : e_.cues())
            if (c.type != CueType::Memory) out.push_back(&c);
        return out;
    }
    int count() const { return (int)list().size(); }

    /// First pad slot not occupied by a hot/loop cue; -1 when all 8
    /// display slots are used (model still accepts higher slots).
    int firstFreePad() const {
        for (int s = 0; s < kPads; ++s)
            if (!get(s)) return s;
        return -1;
    }

    /// §4 actions — CRUD over the engine. `kind` applies to Hot cues
    /// (Instant vs Cue behaviour); Loop cues ignore it.
    Cue set(int slot, CueType type, int64_t frame, const BeatGrid* g,
            int64_t frame2 = -1, uint32_t argb = 0xFFFF4444,
            std::string label = {}, HotKind kind = HotKind::Instant,
            int64_t createdAt = 0) {
        Cue c = e_.setCue(slot, type, frame, g, frame2, argb,
                          std::move(label), createdAt);
        c.kind = kind;
        e_.setKind(slot, kind);
        return e_.get(slot) ? *e_.get(slot) : c;
    }
    bool remove(int slot)         { return get(slot) && e_.deleteCue(slot); }
    bool move(int slot, int64_t f, const BeatGrid* g) {
        return get(slot) && e_.moveCue(slot, f, g);
    }
    bool rename(int slot, const std::string& label) {
        const Cue* c = get(slot);
        return c && e_.updateCue(slot, c->argb, label);
    }
    bool recolor(int slot, uint32_t argb) {
        const Cue* c = get(slot);
        return c && e_.updateCue(slot, argb, c->label);
    }
    bool setEnabled(int slot, bool on) {
        return get(slot) && e_.setEnabled(slot, on);
    }

private:
    CueEngine& e_;
};

} // namespace onda
