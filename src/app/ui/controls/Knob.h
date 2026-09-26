#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../OndaTheme.h"

namespace onda::ui {

/// Rotary knob with caption — mouse drag (vertical), wheel, and
/// keyboard (arrows/page/home/end) through the underlying Slider.
/// `onValue(v)` reports native-range values; parameter binding stays
/// outside (MixerController / fx param / command).
class Knob : public juce::Component {
public:
    Knob(const ThemeEngine& te, const juce::String& caption,
         double lo, double hi, double def)
        : theme_(&te) {
        slider_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider_.setRotaryParameters(
            juce::MathConstants<float>::pi * 1.25f,
            juce::MathConstants<float>::pi * 2.75f, true);
        slider_.setRange(lo, hi, 0.0);
        slider_.setValue(def, juce::dontSendNotification);
        slider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider_.setMouseDragSensitivity(160);
        slider_.setDoubleClickReturnValue(true, def);
        slider_.onValueChange = [this] { if (onValue) onValue(value()); };
        addAndMakeVisible(slider_);

        label_.setText(caption, juce::dontSendNotification);
        label_.setJustificationType(juce::Justification::centred);
        label_.setInterceptsMouseClicks(false, false);
        label_.setColour(juce::Label::textColourId,
                         col(theme_->theme().palette.textDim));
        addAndMakeVisible(label_);
    }

    juce::Slider& slider() { return slider_; }
    double value() const { return slider_.getValue(); }
    void   setValue(double v, bool notify = false) {
        slider_.setValue(v, notify ? juce::sendNotification
                                   : juce::dontSendNotification);
    }
    void setAccent(uint32_t) {} // arc color comes from the LookAndFeel

    std::function<void(double)> onValue;

    void resized() override {
        auto b = getLocalBounds();
        label_.setBounds(b.removeFromBottom(
            juce::jmin(14, b.getHeight() / 4)));
        label_.setFont(juce::Font(juce::FontOptions(
            juce::jlimit(9.0f, 13.0f, (float)label_.getHeight() - 2.0f))));
        slider_.setBounds(b);
    }

private:
    const ThemeEngine* theme_;
    juce::Slider slider_;
    juce::Label  label_;
};

/// Vertical fader with caption — same input surface as Knob.
class Fader : public juce::Component {
public:
    Fader(const ThemeEngine& te, const juce::String& caption,
          double lo, double hi, double def)
        : theme_(&te) {
        slider_.setSliderStyle(juce::Slider::LinearVertical);
        slider_.setRange(lo, hi, 0.0);
        slider_.setValue(def, juce::dontSendNotification);
        slider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider_.setDoubleClickReturnValue(true, def);
        slider_.onValueChange = [this] { if (onValue) onValue(value()); };
        addAndMakeVisible(slider_);

        label_.setText(caption, juce::dontSendNotification);
        label_.setJustificationType(juce::Justification::centred);
        label_.setInterceptsMouseClicks(false, false);
        label_.setColour(juce::Label::textColourId,
                         col(theme_->theme().palette.textDim));
        addAndMakeVisible(label_);
    }

    juce::Slider& slider() { return slider_; }
    double value() const { return slider_.getValue(); }
    void   setValue(double v, bool notify = false) {
        slider_.setValue(v, notify ? juce::sendNotification
                                   : juce::dontSendNotification);
    }

    std::function<void(double)> onValue;

    void resized() override {
        auto b = getLocalBounds();
        label_.setBounds(b.removeFromBottom(
            juce::jmin(14, b.getHeight() / 6)));
        label_.setFont(juce::Font(juce::FontOptions(
            juce::jlimit(9.0f, 13.0f, (float)label_.getHeight() - 2.0f))));
        slider_.setBounds(b);
    }

private:
    const ThemeEngine* theme_;
    juce::Slider slider_;
    juce::Label  label_;
};

/// 2D control — reports normalized x/y on drag; hover shows a cross.
class XYPad : public juce::Component {
public:
    explicit XYPad(const ThemeEngine& te) : theme_(&te) {}

    void setPosition(float nx, float ny) {
        x_ = juce::jlimit(0.0f, 1.0f, nx);
        y_ = juce::jlimit(0.0f, 1.0f, ny);
        repaint();
    }
    float x() const { return x_; }
    float y() const { return y_; }

    std::function<void(float, float)> onChange;

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const auto b = getLocalBounds().toFloat();
        g.setColour(col(p.surfaceSunk));
        g.fillRoundedRectangle(b, 4.0f);
        g.setColour(col(p.borderSoft));
        g.drawRoundedRectangle(b, 4.0f, 1.0f);
        // quadrant guides
        g.setColour(col(p.borderSoft));
        g.drawLine(b.getCentreX(), b.getY(), b.getCentreX(),
                   b.getBottom(), 0.6f);
        g.drawLine(b.getX(), b.getCentreY(), b.getRight(),
                   b.getCentreY(), 0.6f);
        const auto pt = juce::Point<float>(
            b.getX() + x_ * b.getWidth(),
            b.getY() + (1.0f - y_) * b.getHeight());
        g.setColour(col(theme_->theme().accentColor()));
        g.drawLine(pt.x - 6, pt.y, pt.x + 6, pt.y, 1.5f);
        g.drawLine(pt.x, pt.y - 6, pt.x, pt.y + 6, 1.5f);
        g.fillEllipse(pt.x - 3, pt.y - 3, 6, 6);
    }

    void mouseDown(const juce::MouseEvent& e) override { drag(e); }
    void mouseDrag(const juce::MouseEvent& e) override { drag(e); }

private:
    void drag(const juce::MouseEvent& e) {
        const auto b = getLocalBounds().toFloat();
        x_ = juce::jlimit(0.0f, 1.0f, e.position.x / b.getWidth());
        y_ = juce::jlimit(0.0f, 1.0f, 1.0f - e.position.y / b.getHeight());
        repaint();
        if (onChange) onChange(x_, y_);
    }

    const ThemeEngine* theme_;
    float x_ = 0.5f, y_ = 0.5f;
};

} // namespace onda::ui
