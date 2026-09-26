#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/ThemeEngine.h"

namespace onda::ui {

/// Bridges ThemeEngine → juce types. Single conversion point: widgets
/// call col(token) / font(scale) and never embed hex values.
inline juce::Colour col(uint32_t argb_) {
    return juce::Colour(argb_);
}

/// Shared LookAndFeel driven by the active theme — flat geometry,
/// theme-colored arcs/sliders, no gradients copied from anywhere.
class OndaLookAndFeel : public juce::LookAndFeel_V4 {
public:
    explicit OndaLookAndFeel(const ThemeEngine& te) : theme_(&te) {
        syncColors();
    }

    void syncColors() {
        const Palette& p = theme_->theme().palette;
        setColour(juce::ResizableWindow::backgroundColourId,
                  col(p.background));
        setColour(juce::Label::textColourId, col(p.text));
        setColour(juce::TextEditor::backgroundColourId,
                  col(p.surfaceSunk));
        setColour(juce::TextEditor::textColourId, col(p.text));
        setColour(juce::TextEditor::outlineColourId, col(p.borderSoft));
        setColour(juce::TextEditor::focusedOutlineColourId,
                  col(theme_->theme().accentColor()));
        setColour(juce::ComboBox::backgroundColourId, col(p.surfaceRaised));
        setColour(juce::ComboBox::textColourId, col(p.text));
        setColour(juce::ComboBox::outlineColourId, col(p.borderSoft));
        setColour(juce::ComboBox::arrowColourId, col(p.textDim));
        setColour(juce::PopupMenu::backgroundColourId,
                  col(p.surfaceRaised));
        setColour(juce::PopupMenu::textColourId, col(p.text));
        setColour(juce::PopupMenu::highlightedBackgroundColourId,
                  col(theme_->theme().accentColor()));
        setColour(juce::PopupMenu::highlightedTextColourId,
                  col(p.textOnAccent));
        setColour(juce::ListBox::backgroundColourId, col(p.surfaceSunk));
        setColour(juce::ScrollBar::thumbColourId, col(p.border));
        setColour(juce::Slider::textBoxTextColourId, col(p.text));
        setColour(juce::Slider::textBoxOutlineColourId,
                  juce::Colours::transparentBlack);
        setColour(juce::TableHeaderComponent::backgroundColourId,
                  col(p.surface));
        setColour(juce::TableHeaderComponent::textColourId,
                  col(p.textDim));
    }

    juce::Font font(const Metrics& m, int scale,
                    int style = juce::Font::plain) const {
        const float h = scale == 0 ? (float)m.fontSm
                      : scale == 2 ? (float)m.fontLg
                      : scale == 3 ? (float)m.fontXl
                      : (float)m.fontMd;
        return juce::Font(juce::FontOptions(h, style));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                          float pos, float start, float end,
                          juce::Slider& s) override {
        const Palette& p = theme_->theme().palette;
        const auto b = juce::Rectangle<float>((float)x + 2, (float)y + 2,
                                              (float)w - 4, (float)h - 4);
        const float radius = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f;
        const auto  c = b.getCentre();
        const float angle = start + pos * (end - start);

        // body
        g.setColour(col(p.surfaceRaised));
        g.fillEllipse(b);
        g.setColour(col(p.borderSoft));
        g.drawEllipse(b, 1.0f);

        // active arc
        juce::Path arc;
        arc.addCentredArc(c.x, c.y, radius - 2.5f, radius - 2.5f, 0.0f,
                          start, angle, true);
        g.setColour(s.isEnabled() ? col(theme_->theme().accentColor())
                                  : col(p.textFaint));
        g.strokePath(arc, juce::PathStrokeType(2.6f));

        // pointer notch (state readable without color)
        juce::Path needle;
        needle.addRectangle(-1.3f, -radius + 4.0f, 2.6f, radius * 0.42f);
        needle.applyTransform(juce::AffineTransform::rotation(angle)
                              .translated(c.x, c.y));
        g.setColour(col(p.text));
        g.fillPath(needle);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                          float pos, float min, float max,
                          juce::Slider::SliderStyle st,
                          juce::Slider& s) override {
        const Palette& p = theme_->theme().palette;
        const bool vert = st == juce::Slider::LinearVertical;
        const auto track = vert
            ? juce::Rectangle<float>((float)x + w * 0.5f - 2.0f,
                                     (float)y + 4.0f, 4.0f, (float)h - 8.0f)
            : juce::Rectangle<float>((float)x + 4.0f,
                                     (float)y + h * 0.5f - 2.0f,
                                     (float)w - 8.0f, 4.0f);
        g.setColour(col(p.surfaceSunk));
        g.fillRect(track);
        g.setColour(col(p.borderSoft));
        g.drawRect(track, 0.5f);

        // filled portion up to handle pos
        auto fill = track;
        if (vert) fill.setTop((float)pos);
        else      fill.setRight((float)pos);
        g.setColour(col(theme_->theme().accentColor()).withAlpha(0.55f));
        g.fillRect(fill);

        // handle: bright bar, square shape — position reads without hue
        const float th = vert ? 10.0f : 10.0f;
        auto handle = vert
            ? juce::Rectangle<float>((float)x + 1.0f, (float)pos - th * 0.5f,
                                     (float)w - 2.0f, th)
            : juce::Rectangle<float>((float)pos - th * 0.5f, (float)y + 1.0f,
                                     th, (float)h - 2.0f);
        g.setColour(col(p.text));
        g.fillRoundedRectangle(handle, 2.0f);
        g.setColour(col(p.textOnAccent));
        g.drawLine(handle.getCentreX() - (vert ? 4.0f : 0.0f),
                   handle.getCentreY() - (vert ? 0.0f : 4.0f),
                   handle.getCentreX() + (vert ? 4.0f : 0.0f),
                   handle.getCentreY() + (vert ? 0.0f : 4.0f), 1.6f);
    }

private:
    const ThemeEngine* theme_;
};

} // namespace onda::ui
