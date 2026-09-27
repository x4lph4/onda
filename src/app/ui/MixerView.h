#pragma once

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "controls/Knob.h"
#include "controls/MeterBar.h"
#include "controls/StateButton.h"
#include "core/mixer/MixerController.h"
#include "core/mixer/MixerTypes.h"
#include "ui/UiState.h"

namespace onda::ui {

/// Channel strip — trim, H/M/L EQ, filter, sends, cue/mute, xf assign,
/// fader + meter. Writes go through MixerController (validated params);
/// reads come from ChannelSnapshot.
class ChannelStrip : public juce::Component {
public:
    ChannelStrip(const ThemeEngine& te, mixer::MixerController& mc,
                 int ch)
        : theme_(&te), ch_(ch),
          gain_(te, "GAIN", 0.0, 2.0, 1.0),
          high_(te, "HI",  -26.0, 6.0, 0.0),
          mid_(te,  "MID", -26.0, 6.0, 0.0),
          low_(te,  "LOW", -26.0, 6.0, 0.0),
          filt_(te, "FILT", -1.0, 1.0, 0.0),
          pan_(te,  "PAN",  -1.0, 1.0, 0.0),
          sendA_(te, "A", 0.0, 1.0, 0.0),
          sendB_(te, "B", 0.0, 1.0, 0.0),
          fader_(te, "", 0.0, 1.0, 1.0),
          meter_(te),
          cueBtn_(te, "CUE"), muteBtn_(te, "M"), xfBtn_(te, "THRU") {
        using R = mixer::ParameterRegistry;
        auto bind = [&](Knob& k, uint16_t id, bool norm = false) {
            k.onValue = [&mc, id](double v) { mc.setParam(id, (float)v); };
        };
        bind(gain_,  R::channelGain(ch));
        bind(high_,  R::channelHigh(ch));
        bind(mid_,   R::channelMid(ch));
        bind(low_,   R::channelLow(ch));
        bind(filt_,  R::channelFilter(ch));
        bind(pan_,   R::channelPan(ch));
        bind(sendA_, R::channelSendA(ch));
        bind(sendB_, R::channelSendB(ch));
        fader_.onValue = [&mc, ch](double v) {
            mc.channelFader(ch, (float)v);
        };
        cueBtn_.onInvoke = [&mc, ch] {
            using R = mixer::ParameterRegistry;
            mc.channelCue(ch, mc.value(R::channelCue(ch)) < 0.5f);
        };
        muteBtn_.onInvoke = [&mc, ch] {
            using R = mixer::ParameterRegistry;
            mc.channelMute(ch, mc.value(R::channelMute(ch)) < 0.5f);
        };
        xfBtn_.onInvoke = [this, &mc, ch] {
            xf_ = (xf_ + 1) % 3;                 // A → THRU → B cycle
            mc.channelXfAssign(ch, (mixer::XfAssign)xf_);
            xfBtn_.setLabel(xf_ == 0 ? "A" : xf_ == 2 ? "B" : "THRU");
        };
        for (auto* c : { (juce::Component*)&gain_, (juce::Component*)&high_,
                         (juce::Component*)&mid_, (juce::Component*)&low_,
                         (juce::Component*)&filt_,
                         (juce::Component*)&pan_,
                         (juce::Component*)&sendA_,
                         (juce::Component*)&sendB_,
                         (juce::Component*)&fader_,
                         (juce::Component*)&meter_,
                         (juce::Component*)&cueBtn_,
                         (juce::Component*)&muteBtn_,
                         (juce::Component*)&xfBtn_ })
            addAndMakeVisible(*c);
    }

    void setSnapshot(const ChannelSnapshot& s, float dt) {
        // don't fight the user mid-drag: only pull external changes
        auto pull = [](juce::Slider& sl, double v) {
            if (!sl.isMouseButtonDown()
                && std::abs(sl.getValue() - v) > 0.02)
                sl.setValue(v, juce::dontSendNotification);
        };
        pull(gain_.slider(),  s.gain);
        pull(high_.slider(),  s.high);
        pull(mid_.slider(),   s.mid);
        pull(low_.slider(),   s.low);
        pull(filt_.slider(),  s.filter);
        pull(pan_.slider(),   s.pan);
        pull(sendA_.slider(), s.sendA);
        pull(sendB_.slider(), s.sendB);
        pull(fader_.slider(), s.fader);
        cueBtn_.setState(s.cue ? ControlState::On : ControlState::Off);
        muteBtn_.setState(s.mute ? ControlState::Error
                                 : ControlState::Off);
        xf_ = s.xfAssign;
        xfBtn_.setLabel(xf_ == 0 ? "A" : xf_ == 2 ? "B" : "THRU");
        meter_.set(s.levelL, s.levelR, dt);
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        g.setColour(col(p.surface));
        g.fillRect(getLocalBounds());
        g.setColour(col(theme_->theme().waveColor(ch_)));
        g.fillRect(0, 0, getWidth(), 2);
        g.setColour(col(p.borderSoft));
        g.drawRect(getLocalBounds(), 1);
        g.setColour(col(p.textDim));
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText(juce::String::charToString((juce::juce_wchar)('A' + ch_)), 4, 2, 20, 14,
                   juce::Justification::left);
    }

    void resized() override {
        auto b = getLocalBounds().reduced(3);
        b.removeFromTop(16);
        const int kh = juce::jmin(52, b.getHeight() / 7);
        auto row2 = [&](Knob& a, Knob& bb) {
            auto r = b.removeFromTop(kh);
            a.setBounds(r.removeFromLeft(r.getWidth() / 2).reduced(1));
            bb.setBounds(r.reduced(1));
        };
        auto row3 = [&](Knob& a, Knob& bb, Knob& c) {
            auto r = b.removeFromTop(kh);
            const int w = r.getWidth() / 3;
            a.setBounds(r.removeFromLeft(w).reduced(1));
            bb.setBounds(r.removeFromLeft(w).reduced(1));
            c.setBounds(r.reduced(1));
        };
        row3(gain_, filt_, pan_);
        auto r = b.removeFromTop(kh);
        high_.setBounds(r);
        r = b.removeFromTop(kh); mid_.setBounds(r);
        r = b.removeFromTop(kh); low_.setBounds(r);
        row2(sendA_, sendB_);
        auto btns = b.removeFromTop(22);
        cueBtn_.setBounds(btns.removeFromLeft(btns.getWidth() / 2)
                              .reduced(1));
        auto r2 = btns;
        muteBtn_.setBounds(r2.removeFromLeft(r2.getWidth() / 2)
                               .reduced(1));
        xfBtn_.setBounds(r2.reduced(1));
        meter_.setBounds(b.removeFromLeft(10).withTrimmedBottom(4));
        fader_.setBounds(b);
    }

private:
    const ThemeEngine* theme_;
    int ch_, xf_ = 1;
    Knob gain_, high_, mid_, low_, filt_, pan_, sendA_, sendB_;
    Fader fader_;
    MeterBar meter_;
    StateButton cueBtn_, muteBtn_, xfBtn_;
};

/// Mixer column: 4 channel strips + master (meters, gain, cue mix/vol,
/// limiter) + crossfader.
class MixerView : public juce::Component {
public:
    MixerView(const ThemeEngine& te, mixer::MixerController& mc)
        : theme_(&te),
          masterGain_(te, "MASTER", 0.0, 2.0, 1.0),
          cueVol_(te, "CUE VOL", 0.0, 1.0, 0.8),
          cueMix_(te, "CUE MIX", 0.0, 1.0, 0.0),
          booth_(te, "BOOTH", 0.0, 2.0, 1.0),
          masterMeter_(te, false),
          limBtn_(te, "LIM"),
          mCueBtn_(te, "M.CUE"),
          xfCurveBox_() {
        for (int c = 0; c < 4; ++c)
            strips_[(size_t)c] =
                std::make_unique<ChannelStrip>(te, mc, c),
            addAndMakeVisible(*strips_[(size_t)c]);

        using R = mixer::ParameterRegistry;
        masterGain_.onValue = [&mc](double v) {
            mc.masterGain((float)v);
        };
        cueVol_.onValue = [&mc](double v) { mc.cueVolume((float)v); };
        cueMix_.onValue = [&mc](double v) { mc.cueMix((float)v); };
        booth_.onValue = [&mc](double v) { mc.boothGain((float)v); };
        limBtn_.onInvoke = [&mc] {
            mc.limiterOn(mc.value(R::LIMITER_ON) < 0.5f);
        };
        mCueBtn_.onInvoke = [&mc] {
            mc.masterCue(mc.value(R::MASTER_CUE) < 0.5f);
        };
        xf_.setSliderStyle(juce::Slider::LinearHorizontal);
        xf_.setRange(-1.0, 1.0, 0.0);
        xf_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        xf_.onValueChange = [&mc, this] {
            mc.crossfader((float)xf_.getValue());
        };
        xfCurveBox_.addItemList({ "LIN", "EQP", "CUT", "CUS" }, 1);
        xfCurveBox_.setSelectedId(1, juce::dontSendNotification);
        xfCurveBox_.onChange = [&mc, this] {
            mc.xfCurve(xfCurveBox_.getSelectedId() - 1);
        };
        for (auto* c : { (juce::Component*)&masterGain_,
                         (juce::Component*)&cueVol_,
                         (juce::Component*)&cueMix_,
                         (juce::Component*)&booth_,
                         (juce::Component*)&masterMeter_,
                         (juce::Component*)&limBtn_,
                         (juce::Component*)&mCueBtn_,
                         (juce::Component*)&xf_,
                         (juce::Component*)&xfCurveBox_ })
            addAndMakeVisible(*c);
    }

    /// §32/§33 — visible deck channels (2-deck mode hides C/D strips;
    /// the backend keeps running — this is layout only).
    void setChannelCount(int n) {
        visibleChans_ = juce::jlimit(1, 4, n);
        for (int c = 0; c < 4; ++c)
            strips_[(size_t)c]->setVisible(c < visibleChans_);
        resized();
        repaint();
    }
    int channelCount() const noexcept { return visibleChans_; }

    void setSnapshot(const MasterSnapshot& s, float dt) {
        auto pull = [](juce::Slider& sl, double v) {
            if (!sl.isMouseButtonDown()
                && std::abs(sl.getValue() - v) > 0.02)
                sl.setValue(v, juce::dontSendNotification);
        };
        pull(masterGain_.slider(), s.gain);
        pull(cueVol_.slider(), s.cueVol);
        pull(cueMix_.slider(), s.cueMix);
        pull(booth_.slider(), s.boothGain);
        pull(xf_, s.crossfader);
        limBtn_.setState(s.limiterOn ? ControlState::On
                                     : ControlState::Off);
        mCueBtn_.setState(s.masterCue ? ControlState::On
                                      : ControlState::Off);
        masterMeter_.set(s.peakL, s.peakR, dt);
        if (s.clip) masterMeter_.repaint();
    }

    void setChannelSnapshot(int ch, const ChannelSnapshot& s, float dt) {
        strips_[(size_t)ch]->setSnapshot(s, dt);
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        g.setColour(col(p.background));
        g.fillRect(getLocalBounds());
    }

    void resized() override {
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds().reduced(m.gap2);
        auto xfRow = b.removeFromBottom(30);
        xfCurveBox_.setBounds(xfRow.removeFromLeft(44).reduced(1));
        xf_.setBounds(xfRow.reduced(1));

        const int masterW = juce::jmax(64, b.getWidth() / 6);
        auto master = b.removeFromRight(masterW).reduced(1);
        const int chW = b.getWidth() / visibleChans_;
        for (int c = 0; c < visibleChans_; ++c)
            strips_[(size_t)c]->setBounds(
                b.removeFromLeft(chW).reduced(1));

        // master column: meters, gain fader, cue vol/mix, limiter, mcue
        masterMeter_.setBounds(master.removeFromTop(
            juce::jmin(20, master.getHeight() / 12)));
        auto limRow = master.removeFromTop(20);
        limBtn_.setBounds(limRow.removeFromLeft(limRow.getWidth() / 2)
                              .reduced(1));
        mCueBtn_.setBounds(limRow.reduced(1));
        auto row3 = master.removeFromTop(
            juce::jmin(52, master.getHeight() / 5));
        cueVol_.setBounds(row3.removeFromLeft(row3.getWidth() / 3)
                              .reduced(1));
        cueMix_.setBounds(row3.removeFromLeft(row3.getWidth() / 2)
                              .reduced(1));
        booth_.setBounds(row3.reduced(1));
        masterGain_.setBounds(master);
    }

private:
    const ThemeEngine* theme_;
    std::array<std::unique_ptr<ChannelStrip>, 4> strips_;
    Fader      masterGain_;
    Knob       cueVol_, cueMix_, booth_;
    MeterBar   masterMeter_;      // horizontal master meter
    StateButton limBtn_, mCueBtn_;
    int        visibleChans_ = 4;
    juce::Slider    xf_;
    juce::ComboBox  xfCurveBox_;
};

} // namespace onda::ui
