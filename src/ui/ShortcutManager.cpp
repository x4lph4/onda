#include "ShortcutManager.h"
#include "CommandManager.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace onda::ui {

namespace {
struct Named { const char* name; uint32_t code; };
constexpr Named kNamed[] = {
    { "space", key::Space }, { "return", key::Return },
    { "enter", key::Return }, { "escape", key::Escape },
    { "esc", key::Escape }, { "tab", key::Tab },
    { "left", key::Left }, { "right", key::Right },
    { "up", key::Up }, { "down", key::Down },
    { "home", key::Home }, { "end", key::End },
    { "pageup", key::PageUp }, { "pagedown", key::PageDown },
    { "delete", key::Delete }, { "backspace", key::Backspace },
    { "plus", key::Plus }, { "minus", key::Minus },
    { "comma", key::Comma }, { "period", key::Period },
    { "slash", key::Slash },
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
} // namespace

KeyDesc parseKey(const std::string& text) {
    KeyDesc out;
    std::string tok;
    std::istringstream in(text);
    while (std::getline(in, tok, '+')) {
        tok = lower(tok);
        // trim spaces
        tok.erase(std::remove_if(tok.begin(), tok.end(),
                                 [](unsigned char c) { return c == ' '; }),
                  tok.end());
        if (tok.empty()) continue;
        if (tok == "ctrl" || tok == "control" || tok == "cmd") out.mods |= key::Ctrl;
        else if (tok == "shift")  out.mods |= key::Shift;
        else if (tok == "alt" || tok == "option") out.mods |= key::Alt;
        else if (tok.size() == 1) {
            const char c = tok[0];
            out.code = (uint32_t)std::toupper((unsigned char)c);
        } else if (tok.size() >= 2 && tok[0] == 'f' &&
                   std::all_of(tok.begin() + 1, tok.end(), ::isdigit)) {
            const int n = std::stoi(tok.substr(1));
            if (n < 1 || n > 24) { out.code = 0; return out; }
            out.code = key::F1 + (uint32_t)(n - 1);
        } else {
            bool found = false;
            for (const auto& n : kNamed)
                if (tok == n.name) { out.code = n.code; found = true; break; }
            if (!found) { out.code = 0; return out; }
        }
    }
    return out;
}

std::string formatKey(const KeyDesc& k) {
    std::string s;
    if (k.mods & key::Ctrl)  s += "Ctrl+";
    if (k.mods & key::Shift) s += "Shift+";
    if (k.mods & key::Alt)   s += "Alt+";
    if (k.code >= 32 && k.code < 127)
        return s + std::string(1, (char)k.code);
    if (k.code >= key::F1 && k.code < key::F1 + 24)
        return s + "F" + std::to_string(k.code - key::F1 + 1);
    for (const auto& n : kNamed)
        if (k.code == n.code) {
            std::string name = n.name;
            name[0] = (char)std::toupper((unsigned char)name[0]);
            return s + name;
        }
    return s + "?";
}

std::string ShortcutManager::serialize(const CommandManager& cm) const {
    std::ostringstream o;
    for (auto& [k, id] : map_) {
        const Command* c = cm.find(id);
        if (c) o << formatKey(k) << "=" << c->name << "\n";
    }
    return o.str();
}

bool ShortcutManager::deserialize(const std::string& text,
                                  const CommandManager& cm) {
    std::unordered_map<KeyDesc, uint32_t, Hash> next;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const KeyDesc k = parseKey(line.substr(0, eq));
        const uint32_t id = cm.idFor(line.substr(eq + 1));
        if (k.code == 0 || id == cmd::Invalid) continue;
        next[k] = id;
    }
    map_.swap(next);
    return !map_.empty() || text.empty();
}

ShortcutManager ShortcutManager::defaultProfile() {
    using namespace cmd;
    ShortcutManager m;
    auto b = [&m](const char* keyText, uint32_t id) {
        m.bind(parseKey(keyText), id);
    };
    // ---- deck A (left hand home row) ----
    b("Q",        forDeck(DeckPlayToggle, 0));
    b("W",        forDeck(DeckCueTrigger, 0));
    b("S",        forDeck(DeckSyncToggle, 0));
    b("Shift+Q",  forDeck(DeckQuantizeTgl, 0));
    b("E",        forDeck(DeckKeyLockTgl, 0));
    b("Shift+W",  forDeck(DeckSlipToggle, 0));
    b("R",        forDeck(DeckReverseTgl, 0));
    b("A",        forDeck(DeckLoopToggle, 0));
    b("Shift+A",  forDeck(DeckLoopIn, 0));
    b("Shift+S",  forDeck(DeckLoopOut, 0));
    b("Z",        forDeck(DeckLoopHalve, 0));
    b("X",        forDeck(DeckLoopDouble, 0));
    b("Left",     forDeck(DeckJumpBack, 0));
    b("Right",    forDeck(DeckJumpFwd, 0));
    b("Shift+Left",  forDeck(DeckLoopMoveL, 0));
    b("Shift+Right", forDeck(DeckLoopMoveR, 0));
    for (int s = 0; s < 8; ++s) {
        b(("F" + std::to_string(s + 1)).c_str(),
          forHotCue(0, s, 0));
        b(("Shift+F" + std::to_string(s + 1)).c_str(),
          forHotCue(0, s, 1));
    }
    b("Ctrl+Q",   forDeck(DeckLoad, 0));

    // ---- deck B (right hand) ----
    b("P",        forDeck(DeckPlayToggle, 1));
    b("O",        forDeck(DeckCueTrigger, 1));
    b("L",        forDeck(DeckSyncToggle, 1));
    b("Shift+P",  forDeck(DeckQuantizeTgl, 1));
    b("I",        forDeck(DeckKeyLockTgl, 1));
    b("Shift+O",  forDeck(DeckSlipToggle, 1));
    b("U",        forDeck(DeckReverseTgl, 1));
    b("K",        forDeck(DeckLoopToggle, 1));
    b("Shift+K",  forDeck(DeckLoopIn, 1));
    b("Shift+L",  forDeck(DeckLoopOut, 1));
    b("N",        forDeck(DeckLoopHalve, 1));
    b("M",        forDeck(DeckLoopDouble, 1));
    b("Comma",    forDeck(DeckJumpBack, 1));
    b("Period",   forDeck(DeckJumpFwd, 1));
    b("Shift+Comma",  forDeck(DeckLoopMoveL, 1));
    b("Shift+Period", forDeck(DeckLoopMoveR, 1));
    b("Ctrl+P",   forDeck(DeckLoad, 1));

    // ---- mixer / browser / app ----
    b("Ctrl+Up",   MixMasterGainUp);
    b("Ctrl+Down", MixMasterGainDn);
    b("Ctrl+Left", MixXfToA);
    b("Ctrl+Right",MixXfToB);
    b("Ctrl+0",    MixXfCenter);
    b("Ctrl+F",    BrowserFocusSearch);
    b("Ctrl+Return",BrowserLoadDeckA);
    b("Ctrl+Shift+Return", BrowserLoadDeckB);
    b("Ctrl+B",    BrowserTogglePanel);
    b("Ctrl+Comma",AppToggleSettings);
    b("Ctrl+T",    AppLayoutNext);
    b("Escape",    AppPanic);
    return m;
}

} // namespace onda::ui
