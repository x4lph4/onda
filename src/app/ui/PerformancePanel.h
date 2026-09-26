#pragma once

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "controls/StateButton.h"
#include "core/performance/LoopSizeController.h"
#include "ui/CommandIds.h"
#include "ui/pads/PadFeedbackEngine.h"

namespace onda::ui {

/// §37 — per-deck performance surface: hot-cue pads, memory-cue strip,
/// loop cluster + size stepper, beat-jump stepper + readout, roll
/// gate. Hosts INSIDE DeckView; all buttons dispatch through
/// CommandManager, pad/marker visuals come from PadFeedbackEngine
/// lights + the snapshot — no engine access.
class PerformancePanel : public juce::Component {
public:
    PerformancePanel(const ThemeEngine& te, CommandManager& cm, int deck)
        : theme_(&te), cmds_(&cm), deck_(deck),
          loopT_ (te, cm, cmd::forDeck(cmd::DeckLoopToggle, deck), "LOOP"),
          loopIn_(te, cm, cmd::forDeck(cmd::DeckLoopIn, deck), "IN"),
          loopOut_(te, cm, cmd::forDeck(cmd::DeckLoopOut, deck), "OUT"),
          loopH_ (te, cm, cmd::forDeck(cmd::DeckLoopHalve, deck), "/2"),
          loopD_ (te, cm, cmd::forDeck(cmd::DeckLoopDouble, deck), "x2"),
          lszDn_ (te, cm, cmd::forDeck(cmd::DeckLoopSizeDn, deck), "LP-"),
          lszUp_ (te, cm, cmd::forDeck(cmd::DeckLoopSizeUp, deck), "LP+"),
          jl_    (te, cm, cmd::forDeck(cmd::DeckJumpBack, deck), "<<"),
          jr_    (te, cm, cmd::forDeck(cmd::DeckJumpFwd, deck), ">>"),
          jszDn_ (te, cm, cmd::forDeck(cmd::DeckJumpSizeDn, deck), "J-"),
          jszUp_ (te, cm, cmd::forDeck(cmd::DeckJumpSizeUp, deck), "J+"),
          roll_  (te, cm, cmd::forDeck(cmd::DeckRollGate, deck), "ROLL"),
          bendDn_(te, cm, cmd::forDeck(cmd::DeckBendDown, deck), "-"),
          bendUp_(te, cm, cmd::forDeck(cmd::DeckBendUp, deck), "+") {
        roll_.setMomentary(true);
        bendDn_.setMomentary(true); bendUp_.setMomentary(true);
        for (auto* b : { &loopT_, &loopIn_, &loopOut_, &loopH_, &loopD_,
                         &lszDn_, &lszUp_, &jl_, &jr_, &jszDn_, &jszUp_,
                         &roll_, &bendDn_, &bendUp_ })
            addAndMakeVisible(*b);
        for (int s = 0; s < 8; ++s) {
            pads_[(size_t)s] = std::make_unique<StateButton>(
                te, cm, cmd::forHotCue(deck, s, 0),
                juce::String(s + 1));
            addAndMakeVisible(*pads_[(size_t)s]);
        }
    }

    /// Hot-cue pad state (colors + set mask — §4/§38).
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

    /// §36 — per-pad light states from PadFeedbackEngine (playing/
    /// selected pad gets its shape + accent, not just color).
    void setPadLights(const std::array<pads::PadLight, 8>& ls) {
        for (int i = 0; i < 8; ++i) {
            const auto& l = ls[(size_t)i];
            if (l.state == pads::PadLightState::Playing)
                pads_[(size_t)i]->setState(ControlState::On);
            else if (l.state == pads::PadLightState::Selected)
                pads_[(size_t)i]->setState(ControlState::Active);
            else if (l.state == pads::PadLightState::Dim)
                pads_[(size_t)i]->setState(ControlState::Disabled);
            if (l.argb) pads_[(size_t)i]->setAccent(l.argb);
        }
    }

    /// §8 — memory-cue marker row (colors; empty slots dimmed).
    void setMemCues(const std::array<uint32_t, 8>& colors,
                    const std::array<bool, 8>& set) {
        memCol_ = colors; memSet_ = set; repaint();
    }

    void setLoopOn(bool on)   { loopT_.setState(
        on ? ControlState::On : ControlState::Off); }
    void setLoopBeats(double beats)  { loopBeats_ = beats; repaint(); }
    void setJumpBeats(double beats)  { jumpBeats_ = beats; repaint(); }
    void setLoopPending(bool p)      { loopIn_.setState(
        p ? ControlState::Active : ControlState::Off); }

    void mouseDown(const juce::MouseEvent& e) override {
        // memory-cue strip — left = goto, right = set, ctrl+right = del
        if (!memRect_.contains(e.getPosition())) {
            Component::mouseDown(e); return;
        }
        const int s = juce::jlimit(0, 7,
            (e.x - memRect_.getX()) * 8 / juce::jmax(1, memRect_.getWidth()));
        const bool del = e.mods.isCtrlDown() && e.mods.isRightButtonDown();
        const int action = del ? 2 : e.mods.isRightButtonDown() ? 1 : 0;
        if (cmds_) cmds_->invoke(cmd::forMemCue(deck_, s, action),
                                 CommandSource::Ui, 1.0f);
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds();

        // ---- row 1: loop cluster + size stepper ----
        auto r1 = b.removeFromTop(rowH_);
        paintSizeReadout(g, r1.removeFromRight(readoutW_),
                         loopBeats_, "LP", p.accent);
        // ---- row 2: jump cluster + size stepper ----
        auto r2 = b.removeFromTop(rowH_);
        paintSizeReadout(g, r2.removeFromRight(readoutW_),
                         jumpBeats_, "JMP", p.warn);
        b.removeFromTop(m.gap2);
        // ---- row 3: pads (buttons layout in resized) ----
        b.removeFromTop(padsH_);
        b.removeFromTop(m.gap2);
        // ---- row 4: memory-cue strip (custom hit test) ----
        memRect_ = b.removeFromTop(memH_);
        const int cellW = memRect_.getWidth() / 8;
        for (int i = 0; i < 8; ++i) {
            auto cc = juce::Rectangle<int>(
                memRect_.getX() + i * cellW, memRect_.getY(),
                cellW, memH_).reduced(2).toFloat();
            const bool set = memSet_[(size_t)i];
            g.setColour(col(set ? memCol_[(size_t)i] : p.textFaint)
                .withAlpha(set ? 1.0f : 0.4f));
            if (set) g.fillRoundedRectangle(cc, 2.0f);
            else     g.drawRoundedRectangle(cc, 2.0f, 1.0f); // hollow
            g.setColour(col(set ? memCol_[(size_t)i] : p.textFaint));
            g.setFont(juce::Font(juce::FontOptions(
                (float)m.fontSm * 0.8f)));
            g.drawText("M" + juce::String(i + 1), cc,
                       juce::Justification::centred);
        }
    }

    void resized() override {
        auto b = getLocalBounds();
        auto r1 = b.removeFromTop(rowH_);
        r1.removeFromRight(readoutW_);
        const int bw1 = r1.getWidth() / 7;
        loopT_.setBounds(r1.removeFromLeft(bw1).reduced(1));
        loopIn_.setBounds(r1.removeFromLeft(bw1).reduced(1));
        loopOut_.setBounds(r1.removeFromLeft(bw1).reduced(1));
        loopH_.setBounds(r1.removeFromLeft(bw1).reduced(1));
        loopD_.setBounds(r1.removeFromLeft(bw1).reduced(1));
        lszDn_.setBounds(r1.removeFromLeft(bw1).reduced(1));
        lszUp_.setBounds(r1.removeFromLeft(bw1).reduced(1));

        auto r2 = b.removeFromTop(rowH_);
        r2.removeFromRight(readoutW_);
        const int bw2 = r2.getWidth() / 7;
        jl_.setBounds(r2.removeFromLeft(bw2).reduced(1));
        jr_.setBounds(r2.removeFromLeft(bw2).reduced(1));
        jszDn_.setBounds(r2.removeFromLeft(bw2).reduced(1));
        jszUp_.setBounds(r2.removeFromLeft(bw2).reduced(1));
        roll_.setBounds(r2.removeFromLeft(bw2).reduced(1));
        bendDn_.setBounds(r2.removeFromLeft(bw2).reduced(1));
        bendUp_.setBounds(r2.removeFromLeft(bw2).reduced(1));

        b.removeFromTop(theme_->metrics().gap2);
        auto pr = b.removeFromTop(padsH_);
        const int pw = pr.getWidth() / 8;
        for (int s = 0; s < 8; ++s)
            pads_[(size_t)s]->setBounds(pr.removeFromLeft(pw).reduced(2));
    }

    /// Total height the panel wants (resized uses whatever it gets).
    int desiredHeight() const {
        return rowH_ * 2 + padsH_ + memH_ + theme_->metrics().gap2 * 2;
    }

private:
    void paintSizeReadout(juce::Graphics& g, juce::Rectangle<int> r,
                          double beats, const char* tag, uint32_t c) {
        const Palette& p = theme_->theme().palette;
        char nb[16];
        LoopSizeController::formatBeats(beats, nb, (int)sizeof nb);
        g.setColour(col(p.surfaceRaised));
        g.fillRoundedRectangle(r.reduced(1).toFloat(), 3.0f);
        g.setColour(col(c));
        g.drawRoundedRectangle(r.reduced(1).toFloat(), 3.0f, 1.0f);
        g.setFont(juce::Font(juce::FontOptions(
            (float)theme_->metrics().fontSm, juce::Font::bold)));
        g.drawText(juce::String(tag) + " " + juce::String(nb) + "bt", r,
                   juce::Justification::centred);
    }

    const ThemeEngine* theme_;
    CommandManager*    cmds_;
    int                deck_;
    StateButton loopT_, loopIn_, loopOut_, loopH_, loopD_, lszDn_, lszUp_;
    StateButton jl_, jr_, jszDn_, jszUp_, roll_, bendDn_, bendUp_;
    std::array<std::unique_ptr<StateButton>, 8> pads_;
    std::array<uint32_t, 8> memCol_{};
    std::array<bool, 8>     memSet_{};
    double loopBeats_ = 1.0, jumpBeats_ = 4.0;
    juce::Rectangle<int> memRect_;
    static constexpr int rowH_ = 26, padsH_ = 32, memH_ = 14,
                         readoutW_ = 64;
};

} // namespace onda::ui
