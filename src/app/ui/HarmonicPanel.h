#pragma once

#include <array>
#include <functional>
#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "core/harmonic/CamelotWheel.h"
#include "core/harmonic/Compatibility.h"
#include "core/harmonic/KeyNormalizer.h"

namespace onda::ui {

/// ------------------------------------------------------------------
/// §12 — Camelot wheel renderer. Pure view: paint-time model derived
/// from `setKey()` (canonical index) + `setCandidate()`. Clicking a
/// position reports it via `onPick` — the owner decides what to do
/// (filter library, preview a relation); the widget itself never
/// triggers mix actions (§27).
/// ------------------------------------------------------------------
class CamelotWheelView : public juce::Component {
public:
    explicit CamelotWheelView(const ThemeEngine& te) : theme_(&te) {}
    std::function<void(int keyIndex)> onPick;   // canonical index or -1

    void setKey(int keyIndex) {
        current_ = keyIndex;
        harmonic::CamelotWheel::related(keyIndex, rel_);
        repaint();
    }
    void setCandidate(int keyIndex) { candidate_ = keyIndex; repaint(); }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const auto b = getLocalBounds().toFloat();
        const auto c = b.getCentre();
        const float rOuter = juce::jmin(b.getWidth(), b.getHeight()) * 0.48f;
        const float rInner = rOuter * 0.62f;

        for (int n = 0; n < 12; ++n) {
            drawSlot(g, c, rInner, rOuter, n, 'B', p);        // major ring
            drawSlot(g, c, rInner * 0.60f, rInner, n, 'A', p);// minor ring
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (!onPick) return;
        const auto c = getLocalBounds().toFloat().getCentre();
        const float rOuter = juce::jmin((float)getWidth(),
                                        (float)getHeight()) * 0.48f;
        const float dx = (float)e.x - c.x, dy = (float)e.y - c.y;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float rInner = rOuter * 0.62f;
        if (r > rOuter || r < rInner * 0.60f) { onPick(-1); return; }
        const bool minor = r < rInner;
        float a = std::atan2(dy, dx) + juce::MathConstants<float>::halfPi;
        if (a < 0) a += juce::MathConstants<float>::twoPi;
        const int n = (int)(a / juce::MathConstants<float>::twoPi
                            * 12.0f) % 12;
        onPick(harmonic::CamelotConverter::indexOf(n + 1,
                                                 minor ? 'A' : 'B'));
    }

private:
    void drawSlot(juce::Graphics& g, juce::Point<float> c,
                  float r0, float r1, int n, char side,
                  const Palette& p) {
        using harmonic::CamelotWheel;
        using harmonic::CamelotConverter;
        const int idx = CamelotConverter::indexOf(n + 1, side);
        const int pos = idx < 0 ? -1
                                : CamelotConverter::fromIndex(idx).pos();
        const auto rel = pos >= 0 ? rel_[(size_t)pos]
                                  : CamelotWheel::Rel::None;
        const bool isCurrent   = idx == current_   && current_   >= 0;
        const bool isCandidate = idx == candidate_ && candidate_ >= 0;

        const float a0 = -juce::MathConstants<float>::halfPi
                       + (float)n * juce::MathConstants<float>::twoPi / 12.0f;
        const float a1 = a0 + juce::MathConstants<float>::twoPi / 12.0f;
        const float mid = (a0 + a1) * 0.5f;
        const float rLab = (r0 + r1) * 0.5f;
        const auto lc = c + juce::Point<float>(std::cos(mid) * rLab,
                                               std::sin(mid) * rLab);

        // §30 — color codes the relation AND the label weight changes;
        // never color as the sole channel.
        juce::Colour fill = col(p.surfaceRaised);
        juce::Colour txt  = col(p.textDim);
        switch (rel) {
            case CamelotWheel::Rel::Same:
                fill = col(p.accent); txt = col(p.textOnAccent); break;
            case CamelotWheel::Rel::Relative:
                fill = col(p.accentDim); break;
            case CamelotWheel::Rel::Adjacent:
                fill = col(p.surfaceRaised).brighter(0.35f); break;
            case CamelotWheel::Rel::RelativeAdjacent:
                fill = col(p.warn).withAlpha(0.35f); break;
            default: break;
        }
        juce::Path wedge;
        const float gap = 0.015f;
        wedge.addPieSegment(c.x - r1, c.y - r1, r1 * 2, r1 * 2,
                            a0 + gap, a1 - gap, 1.0f - r0 / r1);
        g.setColour(fill.withAlpha(rel == CamelotWheel::Rel::None
                                       && !isCandidate ? 0.55f : 0.95f));
        g.fillPath(wedge);
        if (isCurrent) {
            g.setColour(col(p.text));
            g.strokePath(wedge, juce::PathStrokeType(1.4f));
        } else if (isCandidate) {
            g.setColour(col(p.warn));
            g.strokePath(wedge, juce::PathStrokeType(1.4f));
        }
        g.setColour(txt);
        g.setFont(juce::Font(juce::FontOptions(
            juce::jmax(8.0f, r1 * 0.12f), juce::Font::bold)));
        const juce::String label = juce::String(n + 1)
                                 + juce::String::charToString(side);
        g.drawText(label,
                   juce::Rectangle<float>(lc.x - 13, lc.y - 6, 26, 12),
                   juce::Justification::centred);
    }

    const ThemeEngine* theme_;
    std::array<harmonic::CamelotWheel::Rel, 24> rel_ {};
    int current_ { -1 }, candidate_ { -1 };
};

/// ------------------------------------------------------------------
/// §49 — harmonic mixing panel. Shows the reference deck's tonal
/// state + an optional candidate (browser selection) with the
/// described relationship. Pure view — `setState` feeds plain values;
/// no engine calls, no decisions.
/// ------------------------------------------------------------------
class HarmonicPanel : public juce::Component {
public:
    explicit HarmonicPanel(const ThemeEngine& te)
        : theme_(&te), wheel_(te) {
        addAndMakeVisible(wheel_);
    }

    struct State {
        std::string refTitle;
        int    refKey   { -1 };      // canonical index
        float  refConf  { 0.0f };
        bool   refAmbiguous { false };
        double refBpm   { 0.0 };
        int    candKey  { -1 };
        float  candConf { 0.0f };
        double candBpm  { 0.0 };
        std::string candTitle;
        std::string relation;        // engine-provided explanation
        bool   mixable  { false };
        library::KeyNotation notation { library::KeyNotation::Camelot };
    };

    void setState(const State& s) {
        st_ = s;
        wheel_.setKey(s.refKey);
        wheel_.setCandidate(s.candKey);
        repaint();
    }
    CamelotWheelView& wheel() { return wheel_; }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m  = theme_->metrics();
        auto b = getLocalBounds();
        auto top = b.removeFromTop(b.getHeight() / 2).reduced(m.gap3);
        auto bot = b.reduced(m.gap3);

        // ---- reference block
        g.setColour(col(p.textFaint));
        g.setFont((float)m.fontSm);
        g.drawText("CURRENT", top.removeFromTop(12),
                   juce::Justification::centredLeft);
        g.setColour(col(p.text));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontMd,
                                             juce::Font::bold)));
        juce::String refLine = keyLabel(st_.refKey, st_.notation);
        if (st_.refBpm > 0.0)
            refLine += "  " + juce::String(st_.refBpm, 1) + " BPM";
        g.drawText(refLine, top.removeFromTop(18),
                   juce::Justification::centredLeft);
        // §7 — confidence/ambiguity is explicit text, not just color
        g.setColour(st_.refAmbiguous ? col(p.warn) : col(p.textFaint));
        g.setFont((float)m.fontSm);
        juce::String confLine = st_.refKey >= 0
            ? juce::String(st_.refKey >= 12 ? "minor" : "major")
              + " · conf " + juce::String((int)(st_.refConf * 100)) + "%"
              + (st_.refAmbiguous ? " · AMBIGUOUS" : "")
            : "no key data";
        g.drawText(confLine, top.removeFromTop(14),
                   juce::Justification::centredLeft);

        // ---- candidate block
        g.setColour(col(p.textFaint));
        g.drawText("CANDIDATE", bot.removeFromTop(12),
                   juce::Justification::centredLeft);
        g.setColour(col(st_.candKey >= 0 ? p.text : p.textFaint));
        g.setFont(juce::Font(juce::FontOptions((float)m.fontMd,
                                             juce::Font::bold)));
        juce::String candLine = st_.candKey >= 0
            ? keyLabel(st_.candKey, st_.notation) : "—";
        if (st_.candBpm > 0.0)
            candLine += "  " + juce::String(st_.candBpm, 1) + " BPM";
        g.drawText(candLine, bot.removeFromTop(18),
                   juce::Justification::centredLeft);
        g.setColour(st_.mixable ? col(p.ok) : col(p.textDim));
        g.setFont((float)m.fontSm);
        g.drawFittedText(st_.relation.empty()
                             ? juce::String("no comparison")
                             : juce::String(st_.relation),
                         bot, juce::Justification::centredLeft, 3);
    }

    void resized() override {
        auto b = getLocalBounds();
        wheel_.setBounds(b.removeFromRight(juce::jmin(b.getWidth(),
                                                      b.getHeight()))
                             .reduced(4));
    }

private:
    static juce::String keyLabel(int idx, library::KeyNotation n) {
        return juce::String(harmonic::KeyNormalizer::display(idx, n));
    }

    const ThemeEngine* theme_;
    CamelotWheelView   wheel_;
    State              st_;
};

} // namespace onda::ui
