#include "MainComponent.h"
#include "OndaLookAndFeel.h"

namespace onda {

/// Top bar: product identity + live audio status. Repaints itself at 20 Hz
/// from the atomic AudioStats — never touches RT structures directly.
class HeaderBar : public juce::Component, private juce::Timer {
public:
    explicit HeaderBar(audio::AudioEngine& engine) : engine_(engine) {
        startTimerHz(20);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(OndaLookAndFeel::kSurface);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawLine(0.0f, (float)getHeight() - 0.5f, (float)getWidth(), (float)getHeight() - 0.5f);

        g.setColour(OndaLookAndFeel::kAccent);
        g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
        g.drawText("ONDA", 18, 0, 110, getHeight(), juce::Justification::centredLeft);

        const auto& s = engine_.stats();
        auto* dev = engine_.deviceManager().getCurrentAudioDevice();

        juce::String info;
        info << (dev != nullptr ? dev->getName() : "no device")
             << "   |   " << juce::String(s.sampleRate.load() / 1000.0, 1) << " kHz"
             << " / " << s.bufferSize.load() << " smp"
             << "   |   DSP " << juce::String(s.cpuLoad.load() * 100.0f, 1) << "%"
             << "   |   XRUN " << juce::String((int)s.xrunCount.load());

        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::plain)));
        g.drawText(info, getWidth() - 620, 0, 604, getHeight(),
                   juce::Justification::centredRight);
    }

private:
    void timerCallback() override { repaint(); }
    audio::AudioEngine& engine_;
};

MainComponent::MainComponent(audio::AudioEngine& engine)
    : engine_(engine),
      headerBar_(std::make_unique<HeaderBar>(engine)),
      deviceSelector_(engine.deviceManager(), 0, 0, 1, 4,
                      false, false, true, false) {
    addAndMakeVisible(*headerBar_);
    addAndMakeVisible(deviceSelector_);

    auto styleKnob = [](juce::Slider& s) {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s.setRange(0.0, 1.0, 0.001);
    };

    styleKnob(masterGain_);
    masterGain_.setValue(1.0, juce::dontSendNotification);
    masterGain_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetMasterGain{ (float)masterGain_.getValue() });
    };
    masterLabel_.setText("MASTER", juce::dontSendNotification);
    masterLabel_.setJustificationType(juce::Justification::centred);

    styleKnob(cueMix_);
    cueMix_.setValue(0.5, juce::dontSendNotification);
    cueMix_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetCueMix{ (float)cueMix_.getValue() });
    };
    cueLabel_.setText("CUE MIX", juce::dontSendNotification);
    cueLabel_.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(masterGain_);
    addAndMakeVisible(masterLabel_);
    addAndMakeVisible(cueMix_);
    addAndMakeVisible(cueLabel_);

    setSize(1180, 760);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(OndaLookAndFeel::kBackground);
}

void MainComponent::resized() {
    auto area = getLocalBounds();
    headerBar_->setBounds(area.removeFromTop(46));

    auto right = area.removeFromRight(180).reduced(12);
    auto strip = right.withSizeKeepingCentre(140, 2 * 110);
    masterGain_.setBounds(strip.removeFromTop(80));
    masterLabel_.setBounds(strip.removeFromTop(20));
    strip.removeFromTop(10);
    cueMix_.setBounds(strip.removeFromTop(80));
    cueLabel_.setBounds(strip.removeFromTop(20));

    deviceSelector_.setBounds(area.reduced(14));
}

} // namespace onda
