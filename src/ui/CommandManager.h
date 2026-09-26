#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "CommandIds.h"

namespace onda::ui {

/// Where an invocation came from — recorded and surfaced to listeners
/// (e.g. hardware LED feedback should not re-trigger on its own input).
enum class CommandSource : int {
    Ui = 0, Keyboard, Midi, Hid, Automation, Internal
};

/// One application command. `run` receives a normalized float value
/// (0..1 for set-style commands, ignored by triggers) plus the source.
/// `state()` optionally reports a ControlState-like int for LEDs.
struct Command {
    uint32_t    id       = cmd::Invalid;
    std::string name;                 // "deckA.playToggle" — stable key
    std::string category;             // "Deck A", "Mixer", "Browser"…
    std::string description;
    std::function<void(float, CommandSource)> run;
    std::function<int()>                      state;   // optional
};

/// The single dispatch point for every user/hardware action. Register
/// once at startup; UI buttons, keyboard map, MIDI and HID all call
/// invoke(). No business logic lives in widgets.
class CommandManager {
public:
    using Listener = std::function<void(uint32_t id, CommandSource src)>;

    void add(Command c) {
        commands_[c.id] = std::move(c);
        byName_[commands_[c.id].name] = c.id;
    }
    bool has(uint32_t id)      const { return commands_.count(id) != 0; }
    bool has(const std::string& name) const { return byName_.count(name) != 0; }
    uint32_t idFor(const std::string& name) const {
        auto it = byName_.find(name);
        return it == byName_.end() ? cmd::Invalid : it->second;
    }
    const Command* find(uint32_t id) const {
        auto it = commands_.find(id);
        return it == commands_.end() ? nullptr : &it->second;
    }
    const Command* find(const std::string& name) const {
        return find(idFor(name));
    }

    /// Dispatch. Returns false when the id is unknown — callers can
    /// show "unmapped" feedback instead of silently dropping.
    bool invoke(uint32_t id, CommandSource src,
                float value = 0.0f) {
        auto* c = commands_.count(id) ? &commands_[id] : nullptr;
        if (c == nullptr || !c->run) return false;
        c->run(value, src);
        lastSource_ = src;
        for (auto& l : listeners_) l(id, src);
        return true;
    }
    bool invoke(const std::string& name, CommandSource src,
                float value = 0.0f) {
        return invoke(idFor(name), src, value);
    }

    /// Optional state probe for LEDs/toggle labels (−1 = none).
    int state(uint32_t id) const {
        const Command* c = find(id);
        return (c && c->state) ? c->state() : -1;
    }

    CommandSource lastSource() const { return lastSource_; }
    void addListener(Listener l) { listeners_.push_back(std::move(l)); }

    /// For settings/shortcut editors: stable name→id list by category.
    std::vector<const Command*> list() const {
        std::vector<const Command*> out;
        out.reserve(commands_.size());
        for (auto& [id, c] : commands_) out.push_back(&c);
        return out;
    }

private:
    std::unordered_map<uint32_t, Command>    commands_;
    std::unordered_map<std::string, uint32_t> byName_;
    std::vector<Listener>                    listeners_;
    CommandSource                            lastSource_{CommandSource::Internal};
};

} // namespace onda::ui
