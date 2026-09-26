#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../OndaTheme.h"
#include "ui/CommandManager.h"
#include "ui/ThemeEngine.h"

namespace onda::ui {

/// Theme-driven momentary/toggle button with the five control states —
/// each state changes COLOR + SHAPE/LABEL-SUFFIX, never color alone
/// (accessibility). Dispatches through CommandManager by id when set,
/// or falls back to onInvoke.
class StateButton : public juce::Component {
public:
    StateButton(const ThemeEngine& te, CommandManager& cm,
                uint32_t commandId, const juce::String& label)
        : theme_(&te), cmds_(&cm), id_(commandId), label_(label) {}

    /// Non-command variant (view-local actions).
    StateButton(const ThemeEngine& te, const juce::String& label)
        : theme_(&te), id_(0), label_(label) {}

    void setLabel(const juce::String& l) { label_ = l; repaint(); }
    void setState(ControlState s) {
        if (state_ != s) { state_ = s; repaint(); }
    }
    ControlState state() const { return state_; }
    void setAccent(uint32_t a) { accent_ = a; repaint(); }
    /// Momentary mode: fires value=1 on press, 0 on release.
    void setMomentary(bool m) { momentary_ = m; }

    std::function<void()> onInvoke;   // non-command clicks

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const auto b = getLocalBounds().toFloat().reduced(1.0f);
        const bool down = pressed_;
        const uint32_t acc = accent_ ? accent_
                                     : theme_->theme().accentColor();

        uint32_t bg = p.surfaceRaised, fg = p.text, edge = p.borderSoft;
        switch (state_) {
            case ControlState::On:       bg = acc; fg = p.textOnAccent;
                                         edge = acc; break;
            case ControlState::Active:   bg = p.surfaceRaised;
                                         fg = acc; edge = acc; break;
            case ControlState::Disabled: bg = p.surface;
                                         fg = p.textFaint;
                                         edge = p.borderSoft; break;
            case ControlState::Error:    bg = p.danger; fg = p.textOnAccent;
                                         edge = p.danger; break;
            default: break;
        }
        if (down && state_ != ControlState::Disabled) {
            bg = p.textFaint;
        }
        g.setColour(col(bg));
        g.fillRoundedRectangle(b, 3.0f);
        // shape difference per state:
        if (state_ == ControlState::Active) {       // dashed edge
            const float dashes[] = { 3.0f, 2.0f };
            juce::Path r; r.addRoundedRectangle(b, 3.0f);
            juce::PathStrokeType(1.2f).createDashedStroke(
                r, r, dashes, 2);
            g.setColour(col(edge)); g.fillPath(r);
        } else {
            g.setColour(col(edge));
            g.drawRoundedRectangle(b, 3.0f, 1.0f);
        }
        if (state_ == ControlState::Error) {         // diagonal mark
            g.setColour(col(p.textOnAccent));
            g.drawLine(b.getX() + 3, b.getCentreY(),
                       b.getRight() - 3, b.getCentreY(), 1.6f);
        }
        g.setColour(col(fg));
        g.setFont(juce::Font(juce::FontOptions(
            juce::jmin(13.0f, b.getHeight() * 0.42f),
            state_ == ControlState::On ? juce::Font::bold
                                       : juce::Font::plain)));
        juce::String text = label_;
        if (state_ == ControlState::Error) text += " !";
        g.drawText(text, b, juce::Justification::centred);
    }

    void mouseDown(const juce::MouseEvent&) override {
        if (state_ == ControlState::Disabled) return;
        pressed_ = true; repaint();
        if (momentary_ && id_ && cmds_)
            cmds_->invoke(id_, CommandSource::Ui, 1.0f);
    }
    void mouseUp(const juce::MouseEvent& e) override {
        const bool was = pressed_;
        pressed_ = false; repaint();
        if (!was) return;
        if (momentary_ && id_ && cmds_) {
            cmds_->invoke(id_, CommandSource::Ui, 0.0f);
            return;
        }
        if (state_ == ControlState::Disabled) return;
        if (!e.mouseWasDraggedSinceMouseDown()
            && getLocalBounds().contains(e.getPosition())) {
            if (id_ && cmds_) cmds_->invoke(id_, CommandSource::Ui, 1.0f);
            else if (onInvoke) onInvoke();
        }
    }
    void mouseExit(const juce::MouseEvent&) override {
        if (pressed_) { pressed_ = false; repaint(); }
    }

private:
    const ThemeEngine* theme_;
    CommandManager*    cmds_ = nullptr;
    uint32_t           id_   = 0;
    juce::String       label_;
    ControlState       state_ = ControlState::Off;
    uint32_t           accent_ = 0;
    bool               pressed_ = false, momentary_ = false;
};

} // namespace onda::ui
