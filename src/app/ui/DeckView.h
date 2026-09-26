#pragma once

#include <array>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "controls/Knob.h"
#include "controls/MeterBar.h"
#include "controls/StateButton.h"
#include "ui/CommandIds.h"
#include "ui/UiState.h"
#include "core/model/DeckStats.h"

namespace onda::ui {

/// One deck control block — title/artist/BPM/key/time readout,
/// transport (PLAY/CUE/REV), performance toggles (SYNC/QNT/KEY/SLIP),
/// pitch fader + range selector, loop row, 8 hot-cue pads, meters.
/// Renders only from DeckSnapshot; actions go through CommandManager.
class DeckView : public juce::Component {
public:
    DeckView(const ThemeEngine& te, CommandManager& cm, int deck)
        : theme_(&te), cmds_(&cm), deck_(deck),
          play_(te, cm, cmd::forDeck(cmd::DeckPlayToggle, deck), "PLAY"),
          cue_(te, cm, cmd::forDeck(cmd::DeckCueTrigger, deck), "CUE"),
          sync_(te, cm, cmd::forDeck(cmd::DeckSyncToggle, deck), "SYNC"),
          qnt_(te, cm, cmd::forDeck(cmd::DeckQuantizeTgl, deck), "QNT"),
          key_(te, cm, cmd::forDeck(cmd::DeckKeyLockTgl, deck), "KEY"),
          slip_(te, cm, cmd::forDeck(cmd::DeckSlipToggle, deck), "SLIP"),
          rev_(te, cm, cmd::forDeck(cmd::DeckReverseTgl, deck), "REV"),
          loopT_(te, cm, cmd::forDeck(cmd::DeckLoopToggle, deck), "LOOP"),
          loopIn_(te, cm, cmd::forDeck(cmd::DeckLoopIn, deck), "IN"),
          loopOut_(te, cm, cmd::forDeck(cmd::DeckLoopOut, deck), "OUT"),
          loopH_(te, cm, cmd::forDeck(cmd::DeckLoopHalve, deck), "/2"),
          loopD_(te, cm, cmd::forDeck(cmd::DeckLoopDouble, deck), "x2"),
          jl_(te, cm, cmd::forDeck(cmd::DeckJumpBack, deck), "<<"),
          jr_(te, cm, cmd::forDeck(cmd::DeckJumpFwd, deck), ">>"),
          bendDn_(te, cm, cmd::forDeck(cmd::DeckBendDown, deck), "-"),
          bendUp_(te, cm, cmd::forDeck(cmd::DeckBendUp, deck), "+"),
          pfl_(te, cm, cmd::forDeck(cmd::DeckPflToggle, deck), "PFL"),
          pitch_(te, "PITCH", -16.0, 16.0, 0.0),
          meters_(te) {
        cue_.setMomentary(true);
        bendDn_.setMomentary(true); bendUp_.setMomentary(true);
        for (auto* b : { &play_, &cue_, &sync_, &qnt_, &key_, &slip_,
                         &rev_, &loopT_, &loopIn_, &loopOut_, &loopH_,
                         &loopD_, &jl_, &jr_, &bendDn_, &bendUp_, &pfl_ })
            addAndMakeVisible(*b);
        addAndMakeVisible(pitch_);
        addAndMakeVisible(meters_);

        range_.addItemList({ "+/-8%", "+/-16%", "+/-50%" }, 1);
        range_.setSelectedId(1, juce::dontSendNotification);
        range_.onChange = [this] {
            static const float kR[] = { 8.0f, 16.0f, 50.0f };
            const int i = range_.getSelectedId() - 1;
            if (i >= 0 && i < 3 && onPitchRange)
                onPitchRange(kR[i]);
        };
        addAndMakeVisible(range_);

        // hot-cue pads — trigger on press, set on right-click,
        // delete on Ctrl+right (mirrors hardware semantics)
        for (int s = 0; s < 8; ++s) {
            pads_[s] = std::make_unique<StateButton>(
                te, cm, cmd::forHotCue(deck, s, 0),
                juce::String(s + 1));
            addAndMakeVisible(*pads_[s]);
        }
        setWantsKeyboardFocus(false); // shortcuts live at the shell
    }

    /// pitch slider → rt command, range combo → rt command. Wired by
    /// DeckView's host (needs the CommandQueue).
    std::function<void(float)> onPitch;
    std::function<void(float)> onPitchRange;

    void bindPitch() {
        pitch_.onValue = [this](double v) {
            if (onPitch) onPitch((float)v);
        };
    }

    void setSnapshot(const DeckSnapshot& s) {
        snap_ = s;
        const auto st = (DeckState)s.state;
        play_.setState(st == DeckState::Playing ? ControlState::Active
                                                : ControlState::Off);
        sync_.setState(s.syncMode != 0 ? ControlState::On
                                       : ControlState::Off);
        qnt_.setState(s.quantize ? ControlState::On : ControlState::Off);
        key_.setState(s.keylock ? ControlState::On : ControlState::Off);
        slip_.setState(s.slip ? ControlState::On : ControlState::Off);
        rev_.setState(s.reverse ? ControlState::On : ControlState::Off);
        loopT_.setState(s.loopOn ? ControlState::On : ControlState::Off);
        pfl_.setState(s.pfl ? ControlState::On : ControlState::Off);
        if (std::abs(pitch_.value() - s.pitchPct) > 0.4)
            pitch_.setValue(s.pitchPct); // external change (sync etc.)
        meters_.set(s.levelL, s.levelR, 1.0f / 30.0f);
        repaint();
    }

    /// Hot-cue colors/labels from perf engine cues.
    void setCuePads(const std::array<uint32_t, 8>& colors,
                    const std::array<bool, 8>& set) {
        for (int i = 0; i < 8; ++i) {
            pads_[(size_t)i]->setAccent(set[(size_t)i]
                                        ? colors[(size_t)i] : 0);
            pads_[(size_t)i]->setState(set[(size_t)i]
                                       ? ControlState::Active
                                       : ControlState::Off);
        }
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds();
        g.setColour(col(p.surface));
        g.fillRect(b);
        g.setColour(col(theme_->theme().waveColor(deck_)));
        g.fillRect(b.removeFromTop(3));

        // ---- track meta ----
        auto meta = b.removeFromTop(m.gap6 + m.fontLg + m.fontMd * 2);
        meta.reduce(m.gap3, m.gap2);
        g.setColour(col(theme_->theme().waveColor(deck_)));
        g.setFont(juce::Font(juce::FontOptions(
            (float)m.fontLg, juce::Font::bold)));
        g.drawText(juce::String::charToString((juce::juce_wchar)('A' + deck_)),
                   meta.removeFromLeft(m.gap6), juce::Justification::centredLeft);
        g.setColour(col(p.text));
        g.drawText(snap_.title.empty() ? "(no track)" : snap_.title,
                   meta.removeFromTop(m.fontLg + 2),
                   juce::Justification::centredLeft, true);
        g.setColour(col(p.textDim));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontMd)));
        g.drawText(snap_.artist, meta.removeFromTop(m.fontMd + 2),
                   juce::Justification::centredLeft, true);

        // ---- badges row: bpm / key / pitch% / time ----
        auto badges = b.removeFromTop(m.fontMd + m.gap3 * 2);
        badges.reduce(m.gap3, 0);
        const auto badge = [&](juce::Rectangle<int>& r,
                               const juce::String& txt,
                               uint32_t c) {
            auto rr = r.removeFromLeft(
                juce::jmax(48, (int)txt.length() * 8 + 14));
            g.setColour(col(p.surfaceRaised));
            g.fillRoundedRectangle(rr.reduced(1).toFloat(), 3.0f);
            g.setColour(col(c));
            g.drawRoundedRectangle(rr.reduced(1).toFloat(), 3.0f, 1.0f);
            g.drawText(txt, rr, juce::Justification::centred);
            r.removeFromLeft(m.gap2);
        };
        char num[64];
        std::snprintf(num, sizeof num, "%.2f BPM",
                      snap_.bpm > 0.0 ? snap_.bpm : 0.0);
        badge(badges, num, p.accent);
        if (!snap_.keyText.empty())
            badge(badges, snap_.keyText, p.deckColor(deck_));
        std::snprintf(num, sizeof num, "%+.2f%%", snap_.pitchPct);
        badge(badges, num, p.textDim);
        // state/error badge
        const auto st = (DeckState)snap_.state;
        if (st == DeckState::Loading)
            badge(badges, "LOADING", p.warn);
        else if (st == DeckState::Error)
            badge(badges, "ERROR", p.danger);
        else if (snap_.syncLimited)
            badge(badges, "SYNC LIMITED", p.warn);
        // Phase 14 §31: unanalyzed tracks advertise it — sync/quantize
        // wait for real data instead of trusting nothing.
        if (!snap_.path.empty() && !snap_.analyzed &&
            st != DeckState::Loading && st != DeckState::Error)
            badge(badges, "ANALYSIS PENDING", p.warn);

        // ---- time readout (elapsed / remaining) ----
        auto tr = b.removeFromTop(m.fontXl + m.gap4);
        tr.reduce(m.gap3, 0);
        const auto tstr = [](double s) {
            if (s < 0) s = 0;
            char t[16]; std::snprintf(t, sizeof t, "%d:%05.2f",
                                      (int)(s / 60.0), std::fmod(s, 60.0));
            return juce::String(t);
        };
        g.setColour(col(p.text));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontXl,
                                             juce::Font::bold)));
        g.drawText(tstr(snap_.positionSec),
                   tr.removeFromLeft(tr.getWidth() / 2),
                   juce::Justification::centredLeft);
        g.setColour(col(p.warn));
        g.drawText("-" + tstr(snap_.durationSec - snap_.positionSec),
                   tr, juce::Justification::centredRight);
    }

    void resized() override {
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds().reduced(m.gap2);
        b.removeFromTop(3 + m.gap6 + m.fontLg + m.fontMd * 2
                        + m.fontMd + m.gap3 * 2 + m.fontXl + m.gap4);

        // transport row: PLAY CUE | SYNC QNT | KEY SLIP REV | PFL
        auto row = b.removeFromTop(m.transportH - m.gap3);
        const int bw = row.getWidth() / 8;
        play_.setBounds(row.removeFromLeft(bw));
        cue_.setBounds(row.removeFromLeft(bw));
        sync_.setBounds(row.removeFromLeft(bw).reduced(1));
        qnt_.setBounds(row.removeFromLeft(bw).reduced(1));
        key_.setBounds(row.removeFromLeft(bw).reduced(1));
        slip_.setBounds(row.removeFromLeft(bw).reduced(1));
        rev_.setBounds(row.removeFromLeft(bw).reduced(1));
        pfl_.setBounds(row.removeFromLeft(bw).reduced(1));

        b.removeFromTop(m.gap2);
        // loop row: LOOP IN OUT /2 x2 | << >> | bend - +
        auto row2 = b.removeFromTop(30);
        const int bw2 = row2.getWidth() / 10;
        loopT_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        loopIn_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        loopOut_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        loopH_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        loopD_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        jl_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        jr_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        bendDn_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        bendUp_.setBounds(row2.removeFromLeft(bw2).reduced(1));
        row2.removeFromLeft(bw2); // spacer

        // hot-cue pads
        auto row3 = b.removeFromTop(34);
        const int pw = row3.getWidth() / 8;
        for (int s = 0; s < 8; ++s)
            pads_[(size_t)s]->setBounds(
                row3.removeFromLeft(pw).reduced(2));

        // pitch column right side + meters left of it
        auto bottom = b;
        auto right = bottom.removeFromRight(
            juce::jmin(96, bottom.getWidth() / 4));
        range_.setBounds(right.removeFromTop(22));
        pitch_.setBounds(right);
        meters_.setBounds(bottom.removeFromLeft(14));
    }

private:
    const ThemeEngine* theme_;
    CommandManager*    cmds_;
    int                deck_;
    DeckSnapshot       snap_{};

    StateButton play_, cue_, sync_, qnt_, key_, slip_, rev_;
    StateButton loopT_, loopIn_, loopOut_, loopH_, loopD_, jl_, jr_;
    StateButton bendDn_, bendUp_, pfl_;
    Fader      pitch_;
    MeterBar   meters_;
    juce::ComboBox range_;
    std::array<std::unique_ptr<StateButton>, 8> pads_;
};

} // namespace onda::ui
