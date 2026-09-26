#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "controls/MeterBar.h"
#include "core/rec/RecordingTypes.h"
#include "ui/CommandIds.h"
#include "ui/CommandManager.h"
#include "ui/UiState.h"

namespace onda::ui {

/// Recording strip for the top bar: REC button (toggles quick
/// record), elapsed time, state label, mini stereo meter, CLIP flag,
/// dropped-frame counter and disk space. All actions go through
/// CommandManager (rec.toggle / rec.split) — never the engine.
class RecordingView : public juce::Component {
public:
    RecordingView(const ThemeEngine& te, CommandManager& cm)
        : theme_(&te), cm_(&cm), meter_(te, /*vertical*/ false) {
        addAndMakeVisible(meter_);
    }

    void setSnapshot(const UiSnapshot& s) {
        snap_ = &s;
        meter_.set(s.rec.peakL, s.rec.peakR, 1.0f / 30.0f);
        ++blink_;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m = theme_->metrics();
        auto b = getLocalBounds();
        if (!snap_) {
            g.setColour(col(p.textFaint));
            g.setFont(juce::Font(juce::FontOptions((float)m.fontSm)));
            g.drawText("REC —", b, juce::Justification::centred);
            return;
        }
        const auto& r = snap_->rec;
        const auto st = (rec::State)r.state;

        // REC button — left; shape + text, never hue alone
        auto btn = b.removeFromLeft(b.getHeight() - 6)
                     .reduced(2).toFloat();
        const bool recording = st == rec::State::Recording;
        const bool paused    = st == rec::State::Paused;
        const bool failed    = st == rec::State::Failed
                            || r.diskError;
        const bool armed     = st == rec::State::Armed;
        const bool busy      = st == rec::State::Stopping
                            || st == rec::State::Finalizing;
        // paused blinks slowly via the 30 Hz tick
        const bool lit = recording || (paused && (blink_ / 15) % 2 == 0);
        g.setColour(col(failed  ? p.danger
                      : lit     ? p.meterRed
                      : armed   ? p.warn
                      :           p.surfaceRaised));
        g.fillEllipse(btn);
        g.setColour(col(p.textFaint));
        g.drawEllipse(btn, 1.0f);
        if (!lit && !failed) {
            g.setColour(col(armed ? p.warn : p.textDim));
            g.fillEllipse(btn.reduced(btn.getWidth() * 0.30f));
        }

        // state + elapsed, two lines
        auto txt = b.removeFromLeft(86).reduced(6, 0);
        const char* label =
            failed    ? "ERROR"     :
            recording ? "REC"       :
            paused    ? "PAUSED"    :
            armed     ? "ARMED"     :
            busy      ? "FINALIZING":
            st == rec::State::Completed   ? "DONE" :
            st == rec::State::Recoverable ? "RECOVER" : "READY";
        char tbuf[32];
        const long es = (long)r.elapsedSec;
        std::snprintf(tbuf, sizeof tbuf, "%02ld:%02ld:%02ld",
                      es / 3600, (es / 60) % 60, es % 60);
        g.setFont(juce::Font(juce::FontOptions(
            (float)m.fontSm, juce::Font::bold)));
        g.setColour(col(failed ? p.danger
                      : recording ? p.meterRed
                      : paused ? p.warn : p.textDim));
        g.drawText(juce::String(label) + " " + juce::String(tbuf),
                   txt.removeFromTop(txt.getHeight() / 2),
                   juce::Justification::centredLeft);
        g.setFont(juce::Font(juce::FontOptions((float)m.fontSm)));
        g.setColour(col(r.diskWarn || r.diskError ? p.warn
                                                : p.textFaint));
        juce::String sub = r.clips      ? "CLIP " + juce::String(r.clips)
                         : r.droppedFrames
                                        ? "DROP " + juce::String(
                                            (juce::int64)r.droppedFrames)
                         : r.diskFreeBytes > 0
                                        ? juce::String::formatted(
                                              "%.1f GB free",
                                              (double)r.diskFreeBytes
                                                  / 1e9)
                                        : juce::String(r.fileName);
        g.drawText(sub, txt, juce::Justification::centredLeft);

        meter_.setBounds(b.reduced(4, 0)
                         .withHeight(juce::jmin(14, b.getHeight() - 6))
                         .withY((b.getHeight()
                                 - juce::jmin(14, b.getHeight() - 6))
                                / 2));
    }

    void resized() override { repaint(); }

    void mouseDown(const juce::MouseEvent& e) override {
        auto btn = getLocalBounds()
                       .removeFromLeft(getHeight() - 6).toFloat();
        if (e.mods.isRightButtonDown())
            cm_->invoke(cmd::RecSplit, CommandSource::Ui);
        else if (btn.contains(e.position))
            cm_->invoke(cmd::RecToggle, CommandSource::Ui);
    }

    /// Tooltip-free discoverability: right-click splits the session.
    juce::String getTooltip() {
        return snap_ && !snap_->rec.fileName.empty()
             ? juce::String(snap_->rec.fileName)
             : "REC — click start/stop, right-click split";
    }

private:
    const ThemeEngine* theme_;
    CommandManager*    cm_;
    const UiSnapshot*  snap_ = nullptr;
    MeterBar           meter_;
    int                blink_ = 0;
};

} // namespace onda::ui
