#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "CommandIds.h"

namespace onda::ui {

/// Platform-neutral key descriptor. `code` is OUR key space (ASCII for
/// printable, named constants for the rest) — the JUCE bridge maps a
/// juce::KeyPress to this and the MIDI layer never needs it.
/// mods bitfield: 1=Ctrl 2=Shift 4=Alt.
struct KeyDesc {
    uint32_t code = 0;
    uint32_t mods = 0;
    bool operator==(const KeyDesc& o) const {
        return code == o.code && mods == o.mods;
    }
};

namespace key {
    constexpr uint32_t Ctrl = 1, Shift = 2, Alt = 4;
    // named keys (printable use their ASCII code)
    constexpr uint32_t Space = 0x1000, Return = 0x1001,
        Escape = 0x1002, Tab = 0x1003,
        Left = 0x1004, Right = 0x1005, Up = 0x1006, Down = 0x1007,
        Home = 0x1008, End = 0x1009, PageUp = 0x100A,
        PageDown = 0x100B,
        Delete = 0x100C, Backspace = 0x100D, Plus = 0x100E,
        Minus = 0x100F, Comma = 0x1010, Period = 0x1011,
        Slash = 0x1012,
        F1 = 0x2000; // F1..F24 = F1+n-1
}

/// Parse/format helpers: "Ctrl+Shift+A", "Space", "F5", "Alt+Left".
/// Unknown text parses to code 0 (invalid) — never throws.
KeyDesc     parseKey(const std::string& text);
std::string formatKey(const KeyDesc& k);

/// Key → command binding table. Multiple profiles later (per-layout
/// switching); today one editable map + a shipped default profile.
class ShortcutManager {
public:
    void bind(const KeyDesc& k, uint32_t commandId)   { map_[k] = commandId; }
    void unbind(const KeyDesc& k)                     { map_.erase(k); }
    void clear()                                      { map_.clear(); }

    /// command id for a key, or cmd::Invalid.
    uint32_t lookup(const KeyDesc& k) const {
        auto it = map_.find(k);
        return it == map_.end() ? cmd::Invalid : it->second;
    }
    uint32_t dispatch(const KeyDesc& k) const { return lookup(k); }

    /// Reverse lookup for the settings UI (first binding wins).
    KeyDesc keyFor(uint32_t commandId) const {
        for (auto& [k, id] : map_) if (id == commandId) return k;
        return {};
    }
    size_t size() const { return map_.size(); }

    // ---- persistence ("<key>=<commandName>" per line) ----
    std::string serialize(const class CommandManager& cm) const;
    bool        deserialize(const std::string& text,
                            const class CommandManager& cm);

    /// Shipped defaults — performance-first (transport on letters,
    /// hot cues on digits, mixer on shifted keys). Documented in
    /// ADR-0008; users can rebind anything.
    static ShortcutManager defaultProfile();

    std::vector<std::pair<KeyDesc, uint32_t>> entries() const {
        return { map_.begin(), map_.end() };
    }

private:
    struct Hash {
        size_t operator()(const KeyDesc& k) const {
            return (size_t)k.code * 1315423911u ^ (size_t)k.mods;
        }
    };
    std::unordered_map<KeyDesc, uint32_t, Hash> map_;
};

} // namespace onda::ui
