#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "ui/CommandManager.h"
#include "ui/ShortcutManager.h"

namespace onda::ui {

/// Keyboard-shortcut cheat sheet — a centered overlay generated from
/// the LIVE ShortcutManager map, so it always reflects the real
/// bindings (including user remaps done in Settings). Toggle with the
/// topbar "?" button or the H key; closes on Esc / H / X / outside
/// click.
class HelpView : public juce::Component {
public:
    HelpView(const ThemeEngine& te, CommandManager& cm,
             ShortcutManager& sm)
        : theme_(&te), cmds_(&cm), shortcuts_(&sm) {
        setWantsKeyboardFocus(true);
    }

    std::function<void()> onClose;

    /// Rebuild the row list — call before showing (bindings may have
    /// been edited in Settings since the last open).
    void rebuild() {
        rows_.clear();
        scroll_ = 0;

        // group entries by command category
        std::map<std::string, std::vector<Row>> byCat;
        for (const auto& [k, id] : shortcuts_->entries()) {
            const Command* c = cmds_->find(id);
            if (!c) continue;
            Row r;
            r.key   = formatKey(k);
            r.label = prettify(c->name);
            byCat[c->category].push_back(std::move(r));
        }
        std::vector<std::string> cats;
        for (auto& [cat, _] : byCat) cats.push_back(cat);
        std::sort(cats.begin(), cats.end(), catLess);

        for (const auto& cat : cats) {
            rows_.push_back({ cat, "", true });
            auto& v = byCat[cat];
            std::sort(v.begin(), v.end(),
                      [](const Row& a, const Row& b) {
                          return a.label < b.label; });
            for (auto& r : v) rows_.push_back(std::move(r));
        }
    }

    void setVisible(bool v) override {
        if (v) rebuild();
        juce::Component::setVisible(v);
        if (v) grabKeyboardFocus();
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds();

        g.setColour(col(p.surface).withAlpha(0.98f));
        g.fillRoundedRectangle(b.toFloat(), 8.0f);
        g.setColour(col(p.accent));
        g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 8.0f, 1.5f);

        auto hdr = b.reduced(16).removeFromTop(30);
        g.setFont(juce::Font(juce::FontOptions(
            (float)m.fontLg, juce::Font::bold)));
        g.setColour(col(p.text));
        g.drawText("KEYBOARD SHORTCUTS", hdr,
                   juce::Justification::centredLeft);
        g.setColour(col(p.textFaint));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontSm)));
        g.drawText("H / Esc — close    ?  button in topbar", hdr,
                   juce::Justification::centredRight);

        // close X
        g.setColour(col(p.textDim));
        g.drawText("x", b.removeFromTop(30).removeFromRight(30)
                       .reduced(4),
                   juce::Justification::centred);

        // ---- rows, newspaper columns + shared scroll ----
        auto body = b.reduced(16).withTrimmedTop(34)
                     .withTrimmedBottom(20);
        const int cols = body.getWidth() > 620 ? 2 : 1;
        const int colW = body.getWidth() / cols;
        const int rowH = 20, hdrH = 26;
        const int limit = body.getBottom() + scroll_; // scrolled edge

        g.saveState();
        g.reduceClipRegion(body);
        int i = 0;
        for (int c = 0; c < cols && i < (int)rows_.size(); ++c) {
            int cy = body.getY() - scroll_;
            const int cx = body.getX() + c * colW;
            for (; i < (int)rows_.size(); ++i) {
                const auto& r = rows_[(size_t)i];
                const int need = r.header ? hdrH : rowH;
                // wrap to the next column at the scrolled bottom edge
                if (cy + need > limit && c + 1 < cols) break;
                if (r.header) {
                    g.setColour(col(p.accent));
                    g.setFont(juce::Font(juce::FontOptions(
                        (float)m.fontSm, juce::Font::bold)));
                    g.drawText(juce::String(r.label).toUpperCase(),
                               cx, cy, colW - 12, hdrH,
                               juce::Justification::centredLeft);
                    g.setColour(col(p.borderSoft));
                    g.drawHorizontalLine(cy + hdrH - 7,
                                         (float)cx,
                                         (float)(cx + colW - 24));
                } else {
                    g.setColour(col(p.ok));
                    g.setFont(juce::Font(juce::FontOptions(
                        (float)m.fontSm, juce::Font::bold)));
                    g.drawText(r.key, cx, cy, 92, rowH,
                               juce::Justification::centredRight);
                    g.setColour(col(p.text));
                    g.setFont(juce::Font(juce::FontOptions(
                        (float)m.fontSm)));
                    g.drawText(r.label, cx + 102, cy,
                               colW - 114, rowH,
                               juce::Justification::centredLeft, true);
                }
                cy += need;
            }
        }
        g.restoreState();

        // footer — usage hints
        g.setColour(col(p.textFaint));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontSm)));
        g.drawText("load: double-click fills first empty deck  ·  "
                   "right-click or drag a row to pick the deck",
                   b.reduced(16).removeFromBottom(20),
                   juce::Justification::centredLeft);
    }

    bool keyPressed(const juce::KeyPress&) override {
        if (onClose) onClose();
        return true;
    }
    void mouseDown(const juce::MouseEvent& e) override {
        const auto x = getLocalBounds().removeFromTop(30)
                          .removeFromRight(30);
        if (x.contains(e.getPosition()) && onClose) onClose();
    }
    void mouseWheelMove(const juce::MouseEvent&,
                        const juce::MouseWheelDetails& w) override {
        scroll_ = juce::jmax(0, scroll_ - (int)(w.deltaY * 40.0f));
        repaint();
    }

private:
    struct Row { std::string label, key; bool header = false; };

    /// Category order: decks first, then alphabetical, App last.
    static int catRank(const std::string& c) {
        if (c == "App") return 900;
        if (c.rfind("Deck ", 0) == 0) return c[5] - 'A';
        return 100;
    }
    static bool catLess(const std::string& a, const std::string& b) {
        const int ra = catRank(a), rb = catRank(b);
        return ra != rb ? ra < rb : a < b;
    }

    /// "deckA.playToggle" → "Play Toggle"; strips the dotted prefix.
    static std::string prettify(const std::string& name) {
        std::string s = name;
        if (const auto dot = s.rfind('.'); dot != std::string::npos)
            s = s.substr(dot + 1);
        std::string out;
        out.reserve(s.size() + 6);
        for (size_t i = 0; i < s.size(); ++i) {
            const char ch = s[i];
            if (i > 0 && std::isupper((unsigned char)ch))
                out += ' ';
            out += (i == 0) ? (char)std::toupper((unsigned char)ch) : ch;
        }
        return out;
    }

    const ThemeEngine* theme_;
    CommandManager*    cmds_;
    ShortcutManager*   shortcuts_;
    std::vector<Row>   rows_;
    int                scroll_ = 0;
};

} // namespace onda::ui
