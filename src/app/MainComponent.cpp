#include "MainComponent.h"
#include "OndaLookAndFeel.h"

namespace onda {

static juce::String deckStateName(int s) {
    switch ((DeckState)s) {
        case DeckState::Empty:   return "EMPTY";
        case DeckState::Loading: return "LOADING";
        case DeckState::Ready:   return "READY";
        case DeckState::Playing: return "PLAYING";
        case DeckState::Paused:  return "PAUSED";
        case DeckState::Error:   return "ERROR";
        default:                 return "?";
    }
}

static juce::String formatTime(double sec) {
    if (sec < 0) sec = 0;
    const int m = (int)(sec / 60.0);
    const int s = (int)sec % 60;
    return juce::String::formatted("%d:%02d", m, s);
}

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
             << "   |   XRUN " << juce::String((int)s.xrunCount.load())
             << "   |   CLIP " << juce::String((int)s.clipCount.load());

        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::plain)));
        g.drawText(info, getWidth() - 680, 0, 664, getHeight(),
                   juce::Justification::centredRight);
    }

private:
    void timerCallback() override { repaint(); }
    audio::AudioEngine& engine_;
};

/// Minimal deck transport strip (F3): load/play/gain + live status.
/// Full deck UI (waveform, cues, loops) lands in F5.
class DeckStrip : public juce::Component, private juce::Timer {
public:
    DeckStrip(const juce::String& name, int index, audio::AudioEngine& engine)
        : engine_(engine), deckIndex_(index) {
        title_.setText(name, juce::dontSendNotification);
        title_.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        title_.setColour(juce::Label::textColourId, OndaLookAndFeel::kAccent);
        addAndMakeVisible(title_);

        trackLabel_.setText("no track", juce::dontSendNotification);
        addAndMakeVisible(trackLabel_);
        statusLabel_.setText("EMPTY", juce::dontSendNotification);
        addAndMakeVisible(statusLabel_);

        loadBtn_.setButtonText("LOAD");
        loadBtn_.onClick = [this] { chooseFile(); };
        addAndMakeVisible(loadBtn_);

        playBtn_.setButtonText("PLAY");
        playBtn_.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playBtn_);

        gain_.setSliderStyle(juce::Slider::LinearHorizontal);
        gain_.setRange(0.0, 1.0, 0.001);
        gain_.setValue(1.0, juce::dontSendNotification);
        gain_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
        gain_.onValueChange = [this] {
            engine_.commands().tryPush(
                rt::SetDeckGain{(uint8_t)deckIndex_, (float)gain_.getValue()});
        };
        addAndMakeVisible(gain_);

        startTimerHz(15);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(OndaLookAndFeel::kSurface);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    }

    void resized() override {
        auto a = getLocalBounds().reduced(10);
        title_.setBounds(a.removeFromTop(22));
        trackLabel_.setBounds(a.removeFromTop(18));
        statusLabel_.setBounds(a.removeFromTop(18));
        auto row = a.removeFromTop(30);
        loadBtn_.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2));
        playBtn_.setBounds(row.reduced(2));
        a.removeFromTop(6);
        gain_.setBounds(a.removeFromTop(24));
    }

private:
    void chooseFile() {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Load track to deck",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.mp3;*.wav;*.flac;*.m4a;*.ogg;*.opus;*.aif;*.aiff;*.aac");
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                const auto f = fc.getResult();
                if (f.existsAsFile()) {
                    engine_.loadTrack(deckIndex_, f.getFullPathName());
                    trackLabel_.setText(f.getFileName(), juce::dontSendNotification);
                }
            });
    }

    void togglePlay() {
        const int state = engine_.deckStats(deckIndex_).state.load();
        const bool nowPlaying = state == (int)DeckState::Playing;
        engine_.commands().tryPush(
            rt::DeckTransport{(uint8_t)deckIndex_, !nowPlaying});
    }

    void timerCallback() override {
        const auto& s = engine_.deckStats(deckIndex_);
        const int state = s.state.load();
        statusLabel_.setText(
            deckStateName(state) + "  " +
            formatTime(s.positionSec.load()) + " / " +
            formatTime(s.durationSec.load()) +
            "  bufUnderrun " + juce::String((int)s.underruns.load()),
            juce::dontSendNotification);
        playBtn_.setButtonText(state == (int)DeckState::Playing ? "PAUSE" : "PLAY");
    }

    audio::AudioEngine& engine_;
    int deckIndex_;
    juce::Label title_, trackLabel_, statusLabel_;
    juce::TextButton loadBtn_, playBtn_;
    juce::Slider gain_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

MainComponent::MainComponent(audio::AudioEngine& engine)
    : engine_(engine),
      headerBar_(std::make_unique<HeaderBar>(engine)),
      deviceSelector_(engine.deviceManager(), 0, 0, 1, 4,
                      false, false, true, false) {
    addAndMakeVisible(*headerBar_);
    addAndMakeVisible(deviceSelector_);

    decks_.push_back(std::make_unique<DeckStrip>("DECK A", 0, engine));
    decks_.push_back(std::make_unique<DeckStrip>("DECK B", 1, engine));
    for (auto& d : decks_)
        addAndMakeVisible(*d);

    auto styleKnob = [](juce::Slider& s) {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s.setRange(0.0, 1.0, 0.001);
    };

    styleKnob(masterGain_);
    masterGain_.setValue(1.0, juce::dontSendNotification);
    masterGain_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetMasterGain{(float)masterGain_.getValue()});
    };
    masterLabel_.setText("MASTER", juce::dontSendNotification);
    masterLabel_.setJustificationType(juce::Justification::centred);

    styleKnob(cueMix_);
    cueMix_.setValue(0.5, juce::dontSendNotification);
    cueMix_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetCueMix{(float)cueMix_.getValue()});
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

    auto right = area.removeFromRight(230).reduced(8);
    for (auto& d : decks_)
        d->setBounds(right.removeFromTop(190).reduced(4));

    auto strip = right.withSizeKeepingCentre(150, 2 * 110);
    masterGain_.setBounds(strip.removeFromTop(80));
    masterLabel_.setBounds(strip.removeFromTop(20));
    strip.removeFromTop(10);
    cueMix_.setBounds(strip.removeFromTop(80));
    cueLabel_.setBounds(strip.removeFromTop(20));

    deviceSelector_.setBounds(area.reduced(14));
}

} // namespace onda
