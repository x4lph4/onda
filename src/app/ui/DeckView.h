#pragma once

#include <array>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "PerformancePanel.h"
#include "StemsPanel.h"
#include "controls/Knob.h"
#include "controls/MeterBar.h"
#include "controls/StateButton.h"
#include "ui/CommandIds.h"
#include "ui/TimeDisplay.h"
#include "ui/UiState.h"
#include "core/library/KeyNotation.h"
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
          pfl_(te, cm, cmd::forDeck(cmd::DeckPflToggle, deck), "PFL"),
          pitch_(te, "PITCH", -16.0, 16.0, 0.0),
          meters_(te),
          perfPanel_(te, cm, deck),
          stemsPanel_(te, cm, deck) {
        cue_.setMomentary(true);
        for (auto* b : { &play_, &cue_, &sync_, &qnt_, &key_, &slip_,
                         &rev_, &pfl_ })
            addAndMakeVisible(*b);
        addAndMakeVisible(pitch_);
        addAndMakeVisible(meters_);
        addAndMakeVisible(perfPanel_);
        addAndMakeVisible(stemsPanel_);

        range_.addItemList({ "+/-8%", "+/-16%", "+/-50%" }, 1);
        range_.setSelectedId(1, juce::dontSendNotification);
        range_.onChange = [this] {
            static const float kR[] = { 8.0f, 16.0f, 50.0f };
            const int i = range_.getSelectedId() - 1;
            if (i >= 0 && i < 3 && onPitchRange)
                onPitchRange(kR[i]);
        };
        addAndMakeVisible(range_);
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
        pfl_.setState(s.pfl ? ControlState::On : ControlState::Off);
        perfPanel_.setLoopOn(s.loopOn);
        stemsPanel_.setState(s.stemsAvail, s.stemMode,
                             s.stemFlags, s.stemGain);
        if (std::abs(pitch_.value() - s.pitchPct) > 0.4)
            pitch_.setValue(s.pitchPct); // external change (sync etc.)
        meters_.set(s.levelL, s.levelR, 1.0f / 30.0f);
        repaint();
    }

    /// Hot-cue colors/labels from perf engine cues (§37 — panel owns
    /// the performance cluster now; DeckView forwards).
    void setCuePads(const std::array<uint32_t, 8>& colors,
                    const std::array<bool, 8>& set) {
        perfPanel_.setCuePads(colors, set);
    }
    /// Direct panel access for the app shell — pad lights, memory
    /// cues, size readouts (all fed from snapshots/perf, no engines).
    PerformancePanel& perfPanel() { return perfPanel_; }
    /// Phase 21 — stems strip access (state feeds via setState).
    StemsPanel& stemsPanel() { return stemsPanel_; }

    // ---- Phase 19: display preferences (view-local state) ----
    /// §24 — time readout mode; click on the time row or the
    /// deck.timeMode command cycles it.
    enum class TimeMode : int { Elapsed = 0, Remaining = 1, Total = 2 };
    void cycleTimeMode() {
        timeMode_ = (TimeMode)(((int)timeMode_ + 1) % 3);
        repaint();
    }
    TimeMode timeMode() const { return timeMode_; }

    /// §27 — key notation preference (Standard / Camelot / OpenKey).
    void setKeyNotation(int n) {
        keyNotation_ = (library::KeyNotation)std::clamp(n, 0, 4);
        repaint();
    }

    /// §37 — artwork thumbnail (host loads it off-thread and passes
    /// a scaled image; the view keeps a copy, empty clears).
    void setArtwork(const juce::Image& img) {
        art_ = img; repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override {
        const Metrics m = theme_->metrics();
        // time row band — click cycles the display mode (§24)
        const int timeY = 3 + m.gap6 + m.fontLg + m.fontMd * 2
                          + m.fontMd + m.gap3 * 2;
        if (e.y >= timeY && e.y <= timeY + m.fontXl + m.gap4) {
            cycleTimeMode();
            return;
        }
        Component::mouseDown(e);
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
        // §37 artwork — square thumb left of the title when loaded
        if (art_.isValid()) {
            const int aw = meta.getHeight();
            auto ar = meta.removeFromLeft(aw + m.gap2)
                          .withSizeKeepingCentre(aw, aw).toFloat();
            g.drawImage(art_, ar, juce::RectanglePlacement::centred
                        | juce::RectanglePlacement::onlyReduceInSize);
        }
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
        // §25-26 — show effective/target BPM when they differ from
        // the grid BPM (sync active); raw grid BPM otherwise.
        if (snap_.syncMode != 0 && snap_.targetBpm > 0.0 &&
            std::abs(snap_.effBpm - snap_.bpm) > 0.05) {
            std::snprintf(num, sizeof num, "%.2f→%.2f",
                          snap_.effBpm, snap_.targetBpm);
            badge(badges, num, p.accent);
        } else {
            std::snprintf(num, sizeof num, "%.2f BPM",
                          snap_.effBpm > 0.0 ? snap_.effBpm
                                             : snap_.bpm);
            badge(badges, num, p.accent);
        }
        // §27 — key in the configured notation; keyText fallback.
        // Phase 22 §28: shifted/effective key shows "orig → eff";
        // ambiguity gets an explicit "?" marker (not color alone).
        if (snap_.keyIndex >= 0) {
            const char* kn = library::KeyNotationConverter::name(
                snap_.keyIndex, keyNotation_);
            char buf[64];
            if (snap_.keyEffectiveIndex >= 0) {
                std::snprintf(buf, sizeof buf, "%s→%s", kn,
                    library::KeyNotationConverter::name(
                        snap_.keyEffectiveIndex, keyNotation_));
            } else {
                std::snprintf(buf, sizeof buf, "%s%s", kn,
                              snap_.keyAmbiguous ? "?" : "");
            }
            badge(badges, buf, snap_.keyAmbiguous ? p.warn
                                                  : p.deckColor(deck_));
        } else if (!snap_.keyText.empty()) {
            badge(badges, snap_.keyText, p.deckColor(deck_));
        }
        std::snprintf(num, sizeof num, "%+.2f%%", snap_.pitchPct);
        badge(badges, num, p.textDim);
        // Phase 15 §28/§30: master role + uncertain-sync surface —
        // state comes from PerformanceEngine, never recomputed here.
        if (snap_.isMaster)
            badge(badges, "MASTER", p.accent);
        else if (snap_.syncMode != 0 && snap_.syncConf > 0.0f &&
                 snap_.syncConf < 0.45f)
            badge(badges, "SYNC UNCERTAIN", p.warn);
        // state/error badge (§38-39 — non-color indicators)
        const auto st = (DeckState)snap_.state;
        if (st == DeckState::Loading)
            badge(badges, "LOADING", p.warn);
        else if (st == DeckState::Error)
            badge(badges, "ERROR", p.danger);
        else if (st == DeckState::Paused)
            badge(badges, "PAUSED", p.textDim);
        else if (st == DeckState::Ready)
            badge(badges, "READY", p.textDim);
        else if (st == DeckState::Seeking)
            badge(badges, "SEEK", p.accent);
        else if (snap_.syncLimited)
            badge(badges, "SYNC LIMITED", p.warn);
        if (snap_.pfl) badge(badges, "PFL", p.accent);
        // Phase 14 §31: unanalyzed tracks advertise it — sync/quantize
        // wait for real data instead of trusting nothing.
        if (!snap_.path.empty() && !snap_.analyzed &&
            st != DeckState::Loading && st != DeckState::Error)
            badge(badges, "ANALYSIS PENDING", p.warn);
        // Phase 15 §29: beat indicator — bpb dots, active beat lit
        // (brightness falls with phase), master dot ringed.
        if (snap_.beatPos >= 0.0 && snap_.beatsPerBar > 0 &&
            badges.getWidth() > snap_.beatsPerBar * 10) {
            const int bpb = snap_.beatsPerBar;
            const int cur =
                ((int)std::floor(snap_.beatPos)) % bpb;
            auto dots = badges.removeFromRight(bpb * 10 + 4);
            for (int i = 0; i < bpb; ++i) {
                auto dr = dots.removeFromLeft(10).reduced(2).toFloat();
                const bool lit = i == cur;
                const float ph = lit ? (float)(1.0 - snap_.phase)
                                     : 0.0f;
                g.setColour(col(lit ? p.accent : p.textDim)
                    .withAlpha(lit ? 0.35f + 0.65f * ph : 0.35f));
                if (lit && !snap_.isMaster)
                    g.fillEllipse(dr);
                else
                    g.drawEllipse(dr, 1.0f);
            }
        }

        // ---- time readout — §23-24/§37-38: elapsed / remaining /
        // total via TimeDisplay (M:SS, H:MM:SS over an hour); mode
        // label on the right; click row (mouseDown) cycles ----
        auto tr = b.removeFromTop(m.fontXl + m.gap4);
        tr.reduce(m.gap3, 0);
        const auto tmode = timeMode_ == TimeMode::Elapsed
                               ? TimeDisplay::Mode::Elapsed
                         : timeMode_ == TimeMode::Remaining
                               ? TimeDisplay::Mode::Remaining
                               : TimeDisplay::Mode::Total;
        char tbuf[24];
        TimeDisplay::formatMode(tbuf, sizeof tbuf, tmode,
                                snap_.positionSec, snap_.durationSec);
        g.setColour(col(timeMode_ == TimeMode::Remaining ? p.warn
                                                         : p.text));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontXl,
                                             juce::Font::bold)));
        g.drawText(tbuf, tr.removeFromLeft(tr.getWidth() - 34),
                   juce::Justification::centredLeft);
        g.setColour(col(p.textDim));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontMd)));
        g.drawText(TimeDisplay::tag(tmode), tr,
                   juce::Justification::centredRight);
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
        // §37 performance cluster (loop/jump/sizes/roll/pads/memcues)
        perfPanel_.setBounds(b.removeFromTop(
            juce::jmin(perfPanel_.desiredHeight(),
                       juce::jmax(0, b.getHeight() - 60
                                  - stemsPanel_.desiredHeight()))));
        // Phase 21 §23: stems strip under the performance cluster
        b.removeFromTop(m.gap2);
        stemsPanel_.setBounds(b.removeFromTop(stemsPanel_.desiredHeight()));

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
    StateButton pfl_;
    Fader      pitch_;
    MeterBar   meters_;
    PerformancePanel perfPanel_;
    StemsPanel       stemsPanel_;
    juce::ComboBox range_;
    TimeMode timeMode_ = TimeMode::Elapsed;
    library::KeyNotation keyNotation_ = library::KeyNotation::Camelot;
    juce::Image art_;
};

} // namespace onda::ui
