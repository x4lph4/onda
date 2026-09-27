#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "RecordingView.h"
#include "controls/StateButton.h"
#include "ui/UiState.h"

namespace onda::ui {

/// Full-width status strip: brand, master tempo/deck, audio device +
/// rate/buffer/latency, DSP CPU, xrun/clip counters, settings button.
/// Critical indicators change COLOR + TEXT so they read without hue.
class TopBar : public juce::Component {
public:
    TopBar(const ThemeEngine& te, CommandManager& cm)
        : theme_(&te),
          settings_(te, cm, cmd::AppToggleSettings, "SET"),
          browser_(te, cm, cmd::BrowserTogglePanel, "LIB"),
          fx_(te, cm, cmd::AppToggleFx, "FX"),
          help_(te, cm, cmd::AppToggleHelp, "?"),
          rec_(te, cm) {
        addAndMakeVisible(settings_);
        addAndMakeVisible(browser_);
        addAndMakeVisible(fx_);
        addAndMakeVisible(help_);
        addAndMakeVisible(rec_);
        settings_.setState(ControlState::Off);
        browser_.setState(ControlState::On);
        fx_.setState(ControlState::On);
    }

    StateButton& settingsButton() { return settings_; }
    StateButton& browserButton()  { return browser_; }
    StateButton& fxButton()       { return fx_; }
    StateButton& helpButton()     { return help_; }

    void setSnapshot(const UiSnapshot& s) {
        snap_ = &s; rec_.setSnapshot(s); repaint();
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds();
        g.setColour(col(p.surface));
        g.fillRect(b);
        g.setColour(col(p.borderSoft));
        g.drawHorizontalLine(b.getBottom() - 1,
                             (float)b.getX(), (float)b.getRight());

        // brand
        g.setColour(col(theme_->theme().accentColor()));
        g.setFont(juce::Font(juce::FontOptions(
            (float)m.fontXl, juce::Font::bold)));
        g.drawText("ONDA", b.removeFromLeft(kBrandW).reduced(8, 0),
                   juce::Justification::centredLeft);

        if (!snap_) return;
        const auto& s = *snap_;

        // fixed right-side reservations so text never overlaps:
        // [device text][stats][rec strip][buttons]
        b.removeFromRight(kBtns * kBtnW + kGap + kRecW);
        auto stats = b.removeFromRight(kStats * kStatW);

        g.setFont(juce::Font(juce::FontOptions((float)m.fontMd)));
        juce::String tempo;
        if (s.master.masterDeck >= 0 && s.master.masterBpm > 0.0) {
            tempo = juce::String::formatted(
                "MASTER %.1f  [%c]", s.master.masterBpm,
                'A' + s.master.masterDeck);
        } else tempo = "MASTER —";
        g.setColour(col(p.textDim));
        g.drawText(tempo, b.removeFromLeft(kTempoW),
                   juce::Justification::centred);

        // audio device summary, centred
        const double latMs = s.audio.outputLatencySec * 1000.0;
        // NB: String::formatted uses wide printf on Windows — %s expects
        // wchar_t*, so never pass a char* to it (UTF-8 bytes get read as
        // UTF-16 pairs and render as CJK garbage).
        juce::String dev = juce::String(s.audio.deviceName)
            + juce::String::formatted("  %.1f kHz / %d smp  %.1f ms",
                                      s.audio.sampleRate / 1000.0,
                                      s.audio.bufferSize, latMs);
        g.setColour(col(s.audio.running ? p.text : p.warn));
        g.drawText(dev, b, juce::Justification::centred);

        // status cluster in its reserved strip: UI, DSP, XRUN, CLIP
        auto stat = [&](const juce::String& t, uint32_t c) {
            g.setColour(col(c));
            g.drawText(t, stats.removeFromLeft(kStatW),
                       juce::Justification::centred);
        };
        char buf[48];
        std::snprintf(buf, sizeof buf, "UI %.0f",
                      (double)s.audio.uiFps);
        stat(buf, s.audio.uiFps > 0.0f &&
                  s.audio.uiFps < 20.0f ? p.warn : p.textFaint);
        const float cpu = s.audio.cpuLoad * 100.0f;
        std::snprintf(buf, sizeof buf, "DSP %.0f%%", cpu);
        stat(buf, cpu > 80.0f ? p.danger
                  : cpu > 55.0f ? p.warn : p.textDim);
        std::snprintf(buf, sizeof buf, "XRUN %llu",
                      (unsigned long long)s.audio.xruns);
        stat(buf, s.audio.xruns ? p.warn : p.textFaint);
        std::snprintf(buf, sizeof buf, "CLIP %llu",
                      (unsigned long long)s.audio.clips);
        stat(buf, s.audio.clips ? p.danger : p.textFaint);
    }

    void resized() override {
        auto r = getLocalBounds().withTrimmedTop(3).withTrimmedBottom(3);
        auto btns = r.removeFromRight(kBtns * kBtnW);
        help_.setBounds(btns.removeFromRight(kBtnW).reduced(4, 2));
        settings_.setBounds(btns.removeFromRight(kBtnW).reduced(4, 2));
        fx_.setBounds(btns.removeFromRight(kBtnW).reduced(4, 2));
        browser_.setBounds(btns.removeFromRight(kBtnW).reduced(4, 2));
        r.removeFromRight(kGap);
        rec_.setBounds(r.removeFromRight(kRecW).withTrimmedLeft(4));
    }

private:
    static constexpr int kBrandW = 86;   // "ONDA" wordmark
    static constexpr int kTempoW = 150;  // "MASTER 128.0 [A]"
    static constexpr int kStats  = 4;    // UI, DSP, XRUN, CLIP
    static constexpr int kStatW  = 72;
    static constexpr int kRecW   = 190;  // recording strip
    static constexpr int kBtns   = 4;    // LIB, FX, SET, ?
    static constexpr int kBtnW   = 44;
    static constexpr int kGap    = 10;

    const ThemeEngine* theme_;
    const UiSnapshot*  snap_ = nullptr;
    StateButton settings_, browser_, fx_, help_;
    RecordingView rec_;
};

/// Floating toast stack — bottom-center, newest at bottom. Pure
/// rendering of NotificationCenter items + aging handled by tick.
class NotificationOverlay : public juce::Component {
public:
    NotificationOverlay(const ThemeEngine& te,
                        NotificationCenter& notes)
        : theme_(&te), notes_(&notes) {
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        auto& items = notes_->items();
        if (items.empty()) return;
        const int w = juce::jmin(360, getWidth() - 40);
        const int h = 26, gap = 4;
        int y = getHeight() - h - 6;
        for (auto it = items.rbegin(); it != items.rend() && y > 0; ++it) {
            const auto b = juce::Rectangle<int>(
                (getWidth() - w) / 2, y, w, h).toFloat();
            const uint32_t edge =
                it->level == Notification::Level::Error   ? p.danger  :
                it->level == Notification::Level::Warning ? p.warn    :
                it->level == Notification::Level::Success ? p.ok
                                                          : p.accent;
            // fade by remaining ttl
            const float alpha = juce::jlimit(0.0f, 1.0f,
                                             (float)(it->ttlSec / 0.5));
            g.setColour(col(p.surfaceRaised).withAlpha(0.92f * alpha));
            g.fillRoundedRectangle(b, 5.0f);
            g.setColour(col(edge).withAlpha(alpha));
            g.fillRect(b.getX(), b.getY(), 3.0f, b.getHeight());
            g.setColour(col(p.text).withAlpha(alpha));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            juce::String t = it->text;
            if (!it->detail.empty())
                t += "  —  " + juce::String(it->detail);
            g.drawText(t, b.reduced(8, 0), juce::Justification::centredLeft,
                       true);
            y -= h + gap;
        }
    }

private:
    const ThemeEngine* theme_;
    NotificationCenter* notes_;
};

} // namespace onda::ui
