#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace onda {

/// Onda visual identity. Dark performance UI: near-black surfaces,
/// teal accent for active state, amber for warnings/meters.
/// Palette is ours; do not imitate third-party DJ product themes.
class OndaLookAndFeel : public juce::LookAndFeel_V4 {
public:
    static inline const juce::Colour kBackground { 0xff0e1013 };
    static inline const juce::Colour kSurface    { 0xff161a20 };
    static inline const juce::Colour kAccent     { 0xff2ec4b6 };
    static inline const juce::Colour kAmber      { 0xffff9f1c };
    static inline const juce::Colour kTextDim    { 0xff8a93a0 };

    OndaLookAndFeel() {
        setColourScheme(getMidnightColourScheme());
        setColour(juce::ResizableWindow::backgroundColourId, kBackground);
        setColour(juce::DocumentWindow::textColourId, juce::Colours::white);
        setColour(juce::Slider::thumbColourId, kAccent);
        setColour(juce::Slider::trackColourId, kAccent.withAlpha(0.35f));
        setColour(juce::Slider::backgroundColourId, kSurface);
        setColour(juce::Label::textColourId, kTextDim);
        setColour(juce::ComboBox::backgroundColourId, kSurface);
        setColour(juce::TextButton::buttonColourId, kSurface);
        setColour(juce::ListBox::backgroundColourId, kSurface);
    }
};

} // namespace onda
