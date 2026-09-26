#pragma once

#include <vector>

#include "CueEngine.h"

namespace onda {

/// §7-8 — named per-deck memory-cue manager. Memory cues are
/// permanent NAVIGATION markers: they never auto-play on trigger,
/// carry a free-form comment, and render distinctly from hot cues
/// (hollow flags in the waveform). Storage shares the cue table;
/// `type` is forced to Memory so a hot-cue slot can never silently
/// turn into a memory cue or vice-versa.
class MemoryCueManager {
public:
    explicit MemoryCueManager(CueEngine& e) : e_(e) {}

    const Cue* get(int slot) const {
        const Cue* c = e_.get(slot);
        return (c && c->type == CueType::Memory) ? c : nullptr;
    }
    std::vector<const Cue*> list() const {
        std::vector<const Cue*> out;
        for (auto& c : e_.cues())
            if (c.type == CueType::Memory) out.push_back(&c);
        return out;
    }
    int count() const { return (int)list().size(); }
    int firstFree() const {
        for (int s = 0; s < 32; ++s)
            if (!get(s)) return s;
        return -1;
    }

    /// §7 fields: position + name + color + comment.
    Cue set(int slot, int64_t frame, const BeatGrid* g,
            uint32_t argb = 0xFF33B5E5, std::string label = {},
            std::string comment = {}, int64_t createdAt = 0) {
        Cue c = e_.setCue(slot, CueType::Memory, frame, g, -1, argb,
                          std::move(label), createdAt);
        e_.setComment(slot, std::move(comment));
        return e_.get(slot) ? *e_.get(slot) : c;
    }
    bool remove(int slot) {
        const Cue* c = get(slot);
        return c && e_.deleteCue(slot);
    }
    bool move(int slot, int64_t f, const BeatGrid* g) {
        return get(slot) && e_.moveCue(slot, f, g);
    }
    bool setComment(int slot, const std::string& text) {
        return get(slot) && e_.setComment(slot, text);
    }
    bool recolor(int slot, uint32_t argb) {
        const Cue* c = get(slot);
        return c && e_.updateCue(slot, argb, c->label);
    }
    bool rename(int slot, const std::string& label) {
        const Cue* c = get(slot);
        return c && e_.updateCue(slot, c->argb, label);
    }

private:
    CueEngine& e_;
};

} // namespace onda
