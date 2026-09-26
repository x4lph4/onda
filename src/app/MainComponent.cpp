#include "MainComponent.h"
#include "OndaLookAndFeel.h"
#include "core/dsp/CrossfaderCurves.h"

namespace onda {

static juce::String deckStateName(int s) {
    switch ((DeckState)s) {
        case DeckState::Empty:   return "EMPTY";
        case DeckState::Loading: return "LOADING";
        case DeckState::Ready:   return "READY";
        case DeckState::Playing: return "PLAYING";
        case DeckState::Paused:  return "PAUSED";
        case DeckState::Seeking: return "SEEKING";
        case DeckState::Error:   return "ERROR";
        default:                 return "?";
    }
}

static juce::String formatTime(double sec) {
    if (sec < 0) sec = 0;
    const int m = (int)(sec / 60.0), s = (int)sec % 60;
    return juce::String::formatted("%d:%02d", m, s);
}

// ---------------------------------------------------------------- HeaderBar
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
             << "  |  " << juce::String(s.sampleRate.load() / 1000.0, 1) << " kHz"
             << " / " << s.bufferSize.load() << " smp"
             << "  |  DSP " << juce::String(s.cpuLoad.load() * 100.0f, 1)
             << "% (max " << juce::String(s.maxCallbackMs.load(), 2) << "ms)"
             << "  |  XRUN " << juce::String((int)s.xrunCount.load())
             << "  |  CLIP " << juce::String((int)s.clipCount.load());
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText(info, getWidth() - 760, 0, 744, getHeight(),
                   juce::Justification::centredRight);
    }
private:
    void timerCallback() override { repaint(); }
    audio::AudioEngine& engine_;
};

// ---------------------------------------------------------------- DeckStrip
class DeckStrip : public juce::Component, private juce::Timer {
public:
    DeckStrip(const juce::String& name, int index, audio::AudioEngine& engine,
              AnalysisService& analysis)
        : engine_(engine), analysis_(analysis), deck_(index) {
        auto L = [this](juce::Label& l, const juce::String& t) {
            l.setText(t, juce::dontSendNotification);
            addAndMakeVisible(l);
        };
        L(title_, name);
        title_.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        title_.setColour(juce::Label::textColourId, OndaLookAndFeel::kAccent);
        L(trackLabel_, "no track");
        L(stateLabel_, "EMPTY");

        auto btn = [this](juce::TextButton& b, const juce::String& t) {
            b.setButtonText(t);
            addAndMakeVisible(b);
            return &b;
        };
        btn(loadBtn_, "LOAD")->onClick = [this] { chooseFile(); };
        btn(playBtn_, "PLAY")->onClick = [this] {
            const bool playing = engine_.deckStats(deck_).state.load() == (int)DeckState::Playing;
            engine_.commands().tryPush(rt::DeckTransport{(uint8_t)deck_, (uint8_t)(playing ? 0 : 1)});
        };
        btn(stopBtn_, "STOP")->onClick = [this] {
            engine_.commands().tryPush(rt::DeckTransport{(uint8_t)deck_, 2});
        };
        btn(revBtn_, "REV")->onClick = [this] {
            const bool on = engine_.deckStats(deck_).reverse.load();
            engine_.commands().tryPush(rt::SetReverse{(uint8_t)deck_, !on});
        };
        btn(pflBtn_, "PFL")->onClick = [this] {
            pfl_ = !pfl_;
            engine_.commands().tryPush(rt::SetPfl{(uint8_t)deck_, pfl_});
            pflBtn_.setColour(juce::TextButton::buttonColourId,
                              pfl_ ? OndaLookAndFeel::kAmber : OndaLookAndFeel::kSurface);
        };
        btn(keylockBtn_, "KEY")->onClick = [this] {
            const bool on = engine_.deckStats(deck_).keylock.load();
            engine_.commands().tryPush(rt::SetKeyLock{(uint8_t)deck_, !on});
        };
        btn(bendDownBtn_, "-")->onStateChange = [this] {
            engine_.commands().tryPush(rt::SetPitchBend{(uint8_t)deck_,
                bendDownBtn_.isDown() ? -4.0f : 0.0f});
        };
        btn(bendUpBtn_, "+")->onStateChange = [this] {
            engine_.commands().tryPush(rt::SetPitchBend{(uint8_t)deck_,
                bendUpBtn_.isDown() ? 4.0f : 0.0f});
        };
        btn(loopInBtn_, "IN")->onClick = [this] {
            loopInSec_ = engine_.deckStats(deck_).positionSec.load();
            if (loopOutSec_ <= loopInSec_) loopOutSec_ = loopInSec_ + 4.0;
            engine_.commands().tryPush(rt::SetLoop{(uint8_t)deck_, loopInSec_, loopOutSec_});
        };
        btn(loopOutBtn_, "OUT")->onClick = [this] {
            loopOutSec_ = engine_.deckStats(deck_).positionSec.load();
            engine_.commands().tryPush(rt::SetLoop{(uint8_t)deck_, loopInSec_, loopOutSec_});
        };
        btn(loopOnBtn_, "LOOP")->onClick = [this] {
            const bool on = engine_.deckStats(deck_).loopOn.load();
            engine_.commands().tryPush(rt::SetLoopOn{(uint8_t)deck_, !on});
        };
        btn(loopHalfBtn_, "/2")->onClick = [this] {
            const double len = engine_.deckStats(deck_).loopOutSec.load()
                             - engine_.deckStats(deck_).loopInSec.load();
            engine_.commands().tryPush(rt::SetLoopSize{(uint8_t)deck_, len * 0.5});
        };
        btn(loopDblBtn_, "x2")->onClick = [this] {
            const double len = engine_.deckStats(deck_).loopOutSec.load()
                             - engine_.deckStats(deck_).loopInSec.load();
            engine_.commands().tryPush(rt::SetLoopSize{(uint8_t)deck_, len * 2.0});
        };

        auto slider = [this](juce::Slider& s, double lo, double hi) {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setRange(lo, hi, 0.001);
            s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            addAndMakeVisible(s);
        };
        slider(seekSlider_, 0.0, 1.0);
        seekSlider_.onValueChange = [this] {
            if (seekSlider_.isMouseButtonDown())
                engine_.commands().tryPush(rt::DeckSeek{(uint8_t)deck_, seekSlider_.getValue()});
        };
        slider(pitchSlider_, -8.0, 8.0);
        pitchSlider_.setValue(0.0, juce::dontSendNotification);
        pitchSlider_.onValueChange = [this] {
            engine_.commands().tryPush(rt::SetPitch{(uint8_t)deck_, (float)pitchSlider_.getValue()});
        };

        pitchRange_.addItem("8%", 1); pitchRange_.addItem("16%", 2); pitchRange_.addItem("50%", 3);
        pitchRange_.setSelectedId(1, juce::dontSendNotification);
        pitchRange_.onChange = [this] {
            const float r = pitchRange_.getSelectedId() == 3 ? 50.0f
                          : pitchRange_.getSelectedId() == 2 ? 16.0f : 8.0f;
            pitchSlider_.setRange(-r, r, 0.001);
            engine_.commands().tryPush(rt::SetPitchRange{(uint8_t)deck_, r});
        };
        addAndMakeVisible(pitchRange_);

        auto knob = [this](juce::Slider& s, double lo, double hi, double v) {
            s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
            s.setRange(lo, hi, 0.01);
            s.setValue(v, juce::dontSendNotification);
            s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            addAndMakeVisible(s);
        };
        knob(eqLow_,  -26.0, 6.0, 0.0);
        knob(eqMid_,  -26.0, 6.0, 0.0);
        knob(eqHigh_, -26.0, 6.0, 0.0);
        knob(filter_,  -1.0, 1.0, 0.0);
        knob(trim_,     0.0, 2.0, 1.0);
        eqLow_.onValueChange  = [this] { engine_.commands().tryPush(rt::SetEqBand{(uint8_t)deck_, 0, (float)eqLow_.getValue()}); };
        eqMid_.onValueChange  = [this] { engine_.commands().tryPush(rt::SetEqBand{(uint8_t)deck_, 1, (float)eqMid_.getValue()}); };
        eqHigh_.onValueChange = [this] { engine_.commands().tryPush(rt::SetEqBand{(uint8_t)deck_, 2, (float)eqHigh_.getValue()}); };
        filter_.onValueChange = [this] { engine_.commands().tryPush(rt::SetFilter{(uint8_t)deck_, (float)filter_.getValue()}); };
        trim_.onValueChange   = [this] { engine_.commands().tryPush(rt::SetDeckGain{(uint8_t)deck_, (float)trim_.getValue()}); };

        fader_.setSliderStyle(juce::Slider::LinearVertical);
        fader_.setRange(0.0, 1.0, 0.001);
        fader_.setValue(1.0, juce::dontSendNotification);
        fader_.onValueChange = [this] {
            engine_.commands().tryPush(rt::SetFader{(uint8_t)deck_, (float)fader_.getValue()});
        };
        addAndMakeVisible(fader_);

        startTimerHz(15);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(OndaLookAndFeel::kSurface);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    }

    void resized() override {
        auto a = getLocalBounds().reduced(10);
        auto top = a.removeFromTop(22);
        title_.setBounds(top.removeFromLeft(80));
        trackLabel_.setBounds(top);
        stateLabel_.setBounds(a.removeFromTop(18));
        seekSlider_.setBounds(a.removeFromTop(26));
        a.removeFromTop(4);

        auto row1 = a.removeFromTop(28);
        for (auto* b : { &loadBtn_, &playBtn_, &stopBtn_, &revBtn_, &pflBtn_ })
            b->setBounds(row1.removeFromLeft(row1.getWidth() / 5).reduced(2));

        auto row2 = a.removeFromTop(28);
        pitchSlider_.setBounds(row2.removeFromLeft(150));
        pitchRange_.setBounds(row2.removeFromLeft(60).reduced(2));
        bendDownBtn_.setBounds(row2.removeFromLeft(34).reduced(2));
        bendUpBtn_.setBounds(row2.removeFromLeft(34).reduced(2));
        keylockBtn_.setBounds(row2.removeFromLeft(50).reduced(2));

        auto row3 = a.removeFromTop(28);
        for (auto* b : { &loopInBtn_, &loopOutBtn_, &loopOnBtn_, &loopHalfBtn_, &loopDblBtn_ })
            b->setBounds(row3.removeFromLeft(row3.getWidth() / 5).reduced(2));

        auto row4 = a.removeFromTop(70);
        fader_.setBounds(row4.removeFromRight(50));
        auto k = row4.getWidth() / 5;
        for (auto* s : { &eqLow_, &eqMid_, &eqHigh_, &filter_, &trim_ })
            s->setBounds(row4.removeFromLeft(k).withSizeKeepingCentre(56, 64));
    }

private:
    void chooseFile() {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Load track",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.mp3;*.wav;*.flac;*.m4a;*.ogg;*.opus;*.aif;*.aiff;*.aac");
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                const auto f = fc.getResult();
                if (f.existsAsFile()) {
                    trackPath_ = f.getFullPathName();
                    engine_.loadTrack(deck_, trackPath_);
                    // Background analysis — workers at low priority.
                    analysis_.analyzeTrack(trackPath_.toStdString());
                    trackLabel_.setText(f.getFileName(), juce::dontSendNotification);
                    seekSlider_.setRange(0.0, 1.0, 0.0001);
                }
            });
    }

    void timerCallback() override {
        const auto& s = engine_.deckStats(deck_);
        const int state = s.state.load();
        const double dur = s.durationSec.load();
        juce::String info = deckStateName(state) + "  " +
            formatTime(s.positionSec.load()) + " / " + formatTime(dur) +
            "  bufUR " + juce::String((int)s.underruns.load()) +
            (s.loopOn.load() ? "  [LOOP]" : "");
        if (trackPath_.isNotEmpty()) {
            switch (analysis_.stateFor(trackPath_.toStdString())) {
                case AnalysisState::Analyzed: {
                    const double bpm = analysis_.getBpm(trackPath_.toStdString());
                    const auto key = analysis_.getKey(trackPath_.toStdString());
                    info << "  |  " << juce::String(bpm, 1) << " BPM"
                         << "  " << juce::String(key.standard);
                    break;
                }
                case AnalysisState::Analyzing: info << "  |  analyzing…"; break;
                case AnalysisState::Failed:    info << "  |  analysis failed"; break;
                case AnalysisState::Outdated:  info << "  |  reanalyzing…";
                    analysis_.analyzeTrack(trackPath_.toStdString()); break;
                default: break;
            }
        }
        stateLabel_.setText(info, juce::dontSendNotification);
        if (dur > 0.0 && !seekSlider_.isMouseButtonDown())
            seekSlider_.setRange(0.0, dur, juce::dontSendNotification);
        if (dur > 0.0 && !seekSlider_.isMouseButtonDown())
            seekSlider_.setValue(s.positionSec.load(), juce::dontSendNotification);
        playBtn_.setButtonText(state == (int)DeckState::Playing ? "PAUSE" : "PLAY");
    }

    audio::AudioEngine& engine_;
    AnalysisService& analysis_;
    int deck_;
    bool pfl_ { false };
    juce::String trackPath_;
    double loopInSec_ { 0.0 }, loopOutSec_ { 0.0 };
    juce::Label title_, trackLabel_, stateLabel_;
    juce::TextButton loadBtn_, playBtn_, stopBtn_, revBtn_, pflBtn_, keylockBtn_;
    juce::TextButton bendDownBtn_, bendUpBtn_;
    juce::TextButton loopInBtn_, loopOutBtn_, loopOnBtn_, loopHalfBtn_, loopDblBtn_;
    juce::Slider seekSlider_, pitchSlider_, eqLow_, eqMid_, eqHigh_, filter_, trim_, fader_;
    juce::ComboBox pitchRange_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

// ---------------------------------------------------------------- DevicePanel
class DevicePanel : public juce::Component {
public:
    explicit DevicePanel(audio::AudioEngine& e)
        : selector_(e.deviceManager(), 0, 0, 1, 8, false, false, true, false) {
        addAndMakeVisible(selector_);
    }
    void resized() override { selector_.setBounds(getLocalBounds().reduced(10)); }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff101318));
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.drawRect(getLocalBounds());
    }
private:
    juce::AudioDeviceSelectorComponent selector_;
};

// ---------------------------------------------------------------- MainComponent
MainComponent::MainComponent(audio::AudioEngine& engine,
                             AnalysisService& analysis)
    : engine_(engine), analysis_(analysis),
      headerBar_(std::make_unique<HeaderBar>(engine)),
      devicePanel_(std::make_unique<DevicePanel>(engine)) {
    addAndMakeVisible(*headerBar_);
    addChildComponent(*devicePanel_);
    devicePanel_->setVisible(false);

    decks_.push_back(std::make_unique<DeckStrip>("DECK A", 0, engine, analysis));
    decks_.push_back(std::make_unique<DeckStrip>("DECK B", 1, engine, analysis));
    for (auto& d : decks_) addAndMakeVisible(*d);

    settingsBtn_.setButtonText("SETTINGS");
    settingsBtn_.onClick = [this] { devicePanel_->setVisible(!devicePanel_->isVisible()); };
    addAndMakeVisible(settingsBtn_);

    auto knob = [this](juce::Slider& s, juce::Label& l, const juce::String& t,
                       double lo, double hi, double v) {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setRange(lo, hi, 0.01);
        s.setValue(v, juce::dontSendNotification);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        l.setText(t, juce::dontSendNotification);
        l.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
    };
    knob(masterGain_, masterLabel_, "MASTER", 0.0, 1.0, 1.0);
    masterGain_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetMasterGain{(float)masterGain_.getValue()});
    };
    knob(cueVol_, cueVolLabel_, "CUE VOL", 0.0, 1.0, 0.8);
    cueVol_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetCueVol{(float)cueVol_.getValue()});
    };
    knob(cueMix_, cueMixLabel_, "CUE MIX", 0.0, 1.0, 0.5);
    cueMix_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetCueMix{(float)cueMix_.getValue()});
    };

    crossfader_.setSliderStyle(juce::Slider::LinearHorizontal);
    crossfader_.setRange(-1.0, 1.0, 0.001);
    crossfader_.setValue(0.0, juce::dontSendNotification);
    crossfader_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    crossfader_.onValueChange = [this] {
        engine_.commands().tryPush(rt::SetCrossfader{(float)crossfader_.getValue()});
    };
    addAndMakeVisible(crossfader_);
    xfCurve_.addItem("POWER", (int)XfCurve::EqualPower + 1);
    xfCurve_.addItem("LINEAR", (int)XfCurve::Linear + 1);
    xfCurve_.addItem("CUT", (int)XfCurve::Cut + 1);
    xfCurve_.setSelectedId((int)XfCurve::EqualPower + 1, juce::dontSendNotification);
    xfCurve_.onChange = [this] {
        engine_.commands().tryPush(rt::SetXfCurve{(uint8_t)(xfCurve_.getSelectedId() - 1)});
    };
    addAndMakeVisible(xfCurve_);
    xfLabel_.setText("X-FADER", juce::dontSendNotification);
    xfLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(xfLabel_);
    addAndMakeVisible(meterLabel_);
    meterLabel_.setText("-", juce::dontSendNotification);

    setSize(1280, 840);
    startTimerHz(20);
}

void MainComponent::timerCallback() {
    const auto& m = engine_.masterStats();
    meterLabel_.setText(
        "peak " + juce::String(m.peakL.load(), 2) + "/" + juce::String(m.peakR.load(), 2)
        + "  rms " + juce::String(m.rmsL.load(), 2) + "/" + juce::String(m.rmsR.load(), 2)
        + "  lim " + juce::String(m.limiterGain.load(), 2)
        + (m.cueAvailable.load() ? "  CUE:OK" : "  CUE:N/A"),
        juce::dontSendNotification);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(OndaLookAndFeel::kBackground);
}

void MainComponent::resized() {
    auto area = getLocalBounds();
    headerBar_->setBounds(area.removeFromTop(46));

    auto bottom = area.removeFromBottom(90);
    settingsBtn_.setBounds(bottom.removeFromLeft(90).reduced(6));
    auto m = bottom.removeFromRight(320);
    auto k = m.getWidth() / 3;
    auto mk = [&](juce::Slider& s, juce::Label& l) {
        auto cell = m.removeFromLeft(k);
        s.setBounds(cell.removeFromTop(60));
        l.setBounds(cell);
    };
    mk(masterGain_, masterLabel_);
    mk(cueVol_, cueVolLabel_);
    mk(cueMix_, cueMixLabel_);

    auto xf = bottom.reduced(10);
    xfCurve_.setBounds(xf.removeFromTop(24).removeFromLeft(90));
    xfLabel_.setBounds(xf.removeFromLeft(90).removeFromTop(16));
    crossfader_.setBounds(xf.removeFromTop(26));
    meterLabel_.setBounds(xf);

    devicePanel_->setBounds(area.removeFromRight(560).withTrimmedTop(0));

    auto a = area.reduced(6);
    const int w = (a.getWidth() - 8) / 2;
    decks_[0]->setBounds(a.removeFromLeft(w));
    a.removeFromLeft(8);
    decks_[1]->setBounds(a.removeFromLeft(w));
}

} // namespace onda
