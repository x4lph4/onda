#pragma once

#include <string>
#include <vector>

#include "../dsp/CrossfaderCurves.h"
#include "MixerScene.h"

namespace onda::mixer {

/// §47 — named mixer presets: a MixerScene plus a label. Factory
/// presets encode a complete *mixer* configuration (EQ defaults, fader
/// and crossfader curves, routing, cue defaults, limiter behavior) —
/// never audio, only parameters. User scenes are saved/captured through
/// MixerController and can be added to the same list.
class MixerPresetManager {
public:
    struct Entry { std::string name; MixerScene scene; };

    int count() const noexcept { return (int)entries_.size(); }
    const std::string& name(int i) const { return entries_[(size_t)i].name; }
    const MixerScene&  scene(int i) const { return entries_[(size_t)i].scene; }
    int  current() const noexcept { return current_; }

    /// Cycle selection (hardware-friendly); -1 when list empty.
    int select(int delta) noexcept {
        if (entries_.empty()) return -1;
        const int n = (int)entries_.size();
        current_ = ((current_ + delta) % n + n) % n;
        return current_;
    }
    int selectIndex(int i) noexcept {
        if (i < 0 || i >= (int)entries_.size()) return -1;
        return current_ = i;
    }
    const MixerScene* currentScene() const noexcept {
        return current_ >= 0 && current_ < (int)entries_.size()
             ? &entries_[(size_t)current_].scene : nullptr;
    }

    /// Register a user-captured scene (name deduped by replacement).
    void store(const std::string& name, const MixerScene& s) {
        for (auto& e : entries_)
            if (e.name == name) { e.scene = s; return; }
        entries_.push_back({ name, s });
    }

    /// §27-equivalent for the mixer: the factory configuration set.
    static std::vector<Entry> factoryPresets() {
        std::vector<Entry> v;

        { // Neutral default — unity everywhere, EQP crossfader.
            MixerScene s;
            Entry e{ "Flat", s };
            v.push_back(std::move(e));
        }
        { // Scratch: cut curve, tight faders, Thru assignments off the xf.
            MixerScene s;
            s.xfCurve = (float)(int)XfCurve::Cut;
            for (auto& c : s.ch) c.faderCurve = (float)(int)FaderCurve::AudioTaper;
            v.push_back({ "Scratch", s });
        }
        { // Broadcast: limiter on, conservative master, linear faders.
            MixerScene s;
            s.limiterOn = 1.0f; s.limiterThr = 0.89f;
            s.masterGain = 0.9f;
            v.push_back({ "Broadcast", s });
        }
        { // Booth: master cue on so phones carry the program; PFL-free.
            MixerScene s;
            s.masterCue = 1.0f; s.cueMix = 0.0f; s.cueVol = 1.0f;
            v.push_back({ "Booth Monitor", s });
        }
        { // Dub sends: both FX returns hot, sends staged on every strip.
            MixerScene s;
            for (auto& c : s.ch) { c.sendA = 0.5f; c.sendB = 0.25f; }
            s.fxRetA = 1.4f; s.fxRetB = 1.2f;
            v.push_back({ "Dub Sends", s });
        }
        { // Two-deck rehearsal mode: C/D unmapped, everything else flat.
            MixerScene s;
            s.deckMode = 2;
            v.push_back({ "Two Deck", s });
        }
        return v;
    }

    void loadFactory() { entries_ = factoryPresets(); current_ = 0; }

private:
    std::vector<Entry> entries_ = factoryPresets();
    int current_ { 0 };
};

} // namespace onda::mixer
