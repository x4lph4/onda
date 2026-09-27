#pragma once

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "controls/StateButton.h"
#include "core/stems/StemTypes.h"
#include "ui/CommandIds.h"

namespace onda::ui {

/// §23 — per-deck stems strip: one column per stem (VOC/DRUM/BASS/
/// INST) with mute + solo buttons, a draggable gain bar and a status
/// glyph (—/QUEUED/ANALYZING/LOADING/READY/PARTIAL). Pure view: every
/// interaction dispatches a CommandManager command; musical state
/// arrives via setState() from the snapshot.
class StemsPanel : public juce::Component {
public:
    StemsPanel(const ThemeEngine& te, CommandManager& cm, int deck)
        : theme_(&te), cmds_(&cm), deck_(deck) {
        static const char* kName[4] = { "VOC", "DRM", "BAS", "INS" };
        for (int s = 0; s < 4; ++s) {
            auto& c = cols_[(size_t)s];
            c.name = kName[s];
            c.mute = std::make_unique<StateButton>(
                te, cm,
                cmd::forDeck(cmd::DeckStemBase, deck) + s * 8 + 0, "M");
            c.solo = std::make_unique<StateButton>(
                te, cm,
                cmd::forDeck(cmd::DeckStemBase, deck) + s * 8 + 1, "S");
            addAndMakeVisible(*c.mute);
            addAndMakeVisible(*c.solo);
        }
        mode_ = std::make_unique<StateButton>(
            te, cm, cmd::forDeck(cmd::DeckStemModeTgl, deck), "STEMS");
        addAndMakeVisible(*mode_);
    }

    /// Snapshot feed — plain data only.
    void setState(int availability, int mode,
                  const std::array<uint8_t, 8>& stemFlags,
                  const std::array<float, 4>& gains) {
        avail_ = availability; modeSt_ = mode;
        flags_ = stemFlags; gains_ = gains;
        mode_->setState(mode == 1 ? ControlState::On : ControlState::Off);
        for (int s = 0; s < 4; ++s) {
            const uint8_t f = flags_[(size_t)s];
            const bool present = (f & 0x01) != 0;
            const bool loading = (f & 0x08) != 0;
            cols_[(size_t)s].mute->setState(
                !present ? ControlState::Disabled
              : (f & 0x02) ? ControlState::On : ControlState::Off);
            cols_[(size_t)s].solo->setState(
                !present ? ControlState::Disabled
              : (f & 0x04) ? ControlState::On : ControlState::Off);
            (void)loading;
        }
        repaint();
    }

    int desiredHeight() const { return 44; }

    void mouseDown(const juce::MouseEvent& e) override {
        for (int s = 0; s < 4; ++s)
            if (gainRect_[(size_t)s].contains(e.getPosition())) {
                dragStem_ = s; applyDrag(e); return;
            }
        Component::mouseDown(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (dragStem_ >= 0) { applyDrag(e); return; }
        Component::mouseDrag(e);
    }
    void mouseUp(const juce::MouseEvent&) override { dragStem_ = -1; }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds();
        const int cw = (b.getWidth() - 40) / 4;   // right: STEMS mode btn
        for (int s = 0; s < 4; ++s) {
            const uint8_t f = flags_[(size_t)s];
            auto cb = juce::Rectangle<int>(b.getX() + s * cw,
                                           b.getY(), cw, b.getHeight());
            // status + name
            g.setColour(col((f & 0x01) ? p.textDim : p.textFaint));
            g.setFont(juce::Font(juce::FontOptions(
                (float)m.fontSm * 0.8f, juce::Font::bold)));
            const char* st = !(f & 0x01)
                ? (f & 0x08 ? "···" : "·") : "●";
            g.drawText(juce::String(cols_[(size_t)s].name) + " " + st,
                       cb.removeFromTop(12).reduced(2, 0),
                       juce::Justification::centredLeft);
            // gain bar (bottom strip — bounds set in resized)
            const auto gr = gainRect_[(size_t)s].toFloat();
            g.setColour(col(p.surfaceRaised));
            g.fillRoundedRectangle(gr, 2.0f);
            const float w = gr.getWidth()
                          * juce::jlimit(0.0f, 1.0f, gains_[(size_t)s]);
            g.setColour(col((f & 0x02) ? p.textFaint : p.accent)
                        .withAlpha((f & 0x01) ? 1.0f : 0.35f));
            g.fillRoundedRectangle(gr.withWidth(w), 2.0f);
        }
        if (avail_ == (int)stems::StemAvailability::Unavailable ||
            avail_ == (int)stems::StemAvailability::Queued ||
            avail_ == (int)stems::StemAvailability::Separating) {
            g.setColour(col(p.textFaint));
            g.setFont(juce::Font(juce::FontOptions(
                (float)m.fontSm * 0.75f)));
            g.drawText(stems::stemAvailName(
                           (stems::StemAvailability)avail_),
                       getLocalBounds().removeFromTop(10),
                       juce::Justification::centredRight);
        }
    }

    void resized() override {
        auto b = getLocalBounds();
        const int modeW = 40;
        mode_->setBounds(b.removeFromRight(modeW).reduced(2));
        const int cw = b.getWidth() / 4;
        for (int s = 0; s < 4; ++s) {
            auto cb = b.removeFromLeft(cw);
            cb.removeFromTop(12);                    // name/status row
            auto row = cb.removeFromTop(16);
            cols_[(size_t)s].mute->setBounds(
                row.removeFromLeft(cw / 2).reduced(1));
            cols_[(size_t)s].solo->setBounds(row.reduced(1));
            gainRect_[(size_t)s] = cb.removeFromTop(8)
                .reduced(3, 0);
        }
    }

private:
    void applyDrag(const juce::MouseEvent& e) {
        const auto& r = gainRect_[(size_t)dragStem_];
        const float v = juce::jlimit(0.0f, 1.0f,
            (float)(e.x - r.getX()) / juce::jmax(1, r.getWidth()));
        if (cmds_) cmds_->invoke(
            cmd::forDeck(cmd::DeckStemBase, deck_) + dragStem_ * 8 + 7,
            CommandSource::Ui, v);                      // gainSet 0..1
    }

    struct Col {
        const char* name { "" };
        std::unique_ptr<StateButton> mute, solo;
    };
    const ThemeEngine* theme_;
    CommandManager*    cmds_;
    int                deck_;
    std::array<Col, 4> cols_;
    std::unique_ptr<StateButton> mode_;
    std::array<uint8_t, 8> flags_ {};
    std::array<float, 4>   gains_ { 0.5f, 0.5f, 0.5f, 0.5f };
    std::array<juce::Rectangle<int>, 4> gainRect_ {};
    int avail_ { 0 }, modeSt_ { 0 }, dragStem_ { -1 };
};

} // namespace onda::ui
