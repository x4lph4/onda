#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../OndaTheme.h"

namespace onda::ui {

/// Stereo level meter — segmented green/amber/red bars with peak-hold
/// line and a persistent clip marker until cleared. Decay is applied
/// per set() call so the widget is frame-rate independent:
///   level *= exp(-dt / tau) — we pass dt from the 30 Hz snapshot tick.
class MeterBar : public juce::Component {
public:
    explicit MeterBar(const ThemeEngine& te, bool vertical = true)
        : theme_(&te), vert_(vertical) {}

    /// lin values 0..~1.2 (over-FS clips at 1.0). Call on each UI tick.
    void set(float l, float r, float dtSec) {
        const float decay = std::exp(-dtSec / 0.85f);
        holdL_ = std::max(holdL_ * decay, l);
        holdR_ = std::max(holdR_ * decay, r);
        const float fall = std::exp(-dtSec / 0.06f);
        lvlL_ = std::max(l, lvlL_ * fall);
        lvlR_ = std::max(r, lvlR_ * fall);
        clip_ = clip_ || l >= 1.0f || r >= 1.0f;
        repaint();
    }
    void clearClip() { clip_ = false; repaint(); }
    bool clipped() const { return clip_; }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const auto b = getLocalBounds().toFloat();
        g.setColour(col(p.meterBack));
        g.fillRect(b);
        paintHalf(g, b, vert_ ? b.withTrimmedBottom(b.getHeight() * 0.5f)
                              : b.withTrimmedRight(b.getWidth() * 0.5f),
                  lvlL_, holdL_);
        paintHalf(g, b, vert_ ? b.withTrimmedTop(b.getHeight() * 0.5f)
                              : b.withTrimmedLeft(b.getWidth() * 0.5f),
                  lvlR_, holdR_);
        if (clip_) {   // clip flag: filled corner square (shape, not hue)
            g.setColour(col(p.danger));
            g.fillRect(b.getRight() - 5.0f, b.getY() + 1.0f, 4.0f, 4.0f);
        }
    }

private:
    void paintHalf(juce::Graphics& g, juce::Rectangle<float> all,
                   juce::Rectangle<float> half,
                   float lvl, float hold) {
        const Palette& p = theme_->theme().palette;
        MeterBands bands;
        const bool v = vert_;
        const float span = v ? half.getHeight() : half.getWidth();
        const float filled = juce::jlimit(0.0f, 1.0f, lvl) * span;

        // three bands — bottom green, mid amber, top red
        auto bandRect = [&](float a, float bnd) {
            const float lo = a * span, hi = bnd * span;
            if (v) return juce::Rectangle<float>(
                half.getX(), half.getBottom() - hi,
                half.getWidth(), hi - lo);
            return juce::Rectangle<float>(
                half.getX() + lo, half.getY(),
                hi - lo, half.getHeight());
        };
        const struct { float a, bnd; uint32_t c; } seg[3] = {
            { 0.0f,          bands.greenTo, p.meterGreen },
            { bands.greenTo, bands.amberTo, p.meterAmber },
            { bands.amberTo, 1.0f,          p.meterRed   },
        };
        for (auto& s : seg) {
            auto r = bandRect(s.a, s.bnd);
            g.setColour(col(s.c).withAlpha(0.22f));
            g.fillRect(r);
            // lit part
            if (v) {
                const float litBot = std::max(
                    half.getBottom() - filled, r.getY());
                auto lit = r.withTop(std::max(litBot, r.getY()));
                g.setColour(col(s.c));
                g.fillRect(lit);
            } else {
                auto lit = r.withRight(std::min(
                    half.getX() + filled, r.getRight()));
                g.setColour(col(s.c));
                g.fillRect(lit);
            }
        }
        // peak hold tick
        const float hp = juce::jlimit(0.0f, 1.0f, hold) * span;
        g.setColour(col(p.text));
        if (v)
            g.fillRect(half.getX(), half.getBottom() - hp,
                       half.getWidth(), 1.5f);
        else
            g.fillRect(half.getX() + hp, half.getY(),
                       1.5f, half.getHeight());
    }

    const ThemeEngine* theme_;
    bool vert_ = true;
    float lvlL_ = 0.0f, lvlR_ = 0.0f, holdL_ = 0.0f, holdR_ = 0.0f;
    bool clip_ = false;
};

} // namespace onda::ui
