#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "core/analysis/AnalysisTypes.h"
#include "core/analysis/BeatGrid.h"
#include "core/analysis/WaveformData.h"
#include "ui/UiState.h"
#include "ui/WaveformMapping.h"

namespace onda::ui {

/// Deck waveform — renders from a cached WaveformData pointer (never
/// decodes audio) plus a BeatGrid pointer for markers.
///
/// Two stacked modes inside one component:
///   Detail   — scrollable zoomed strip, playhead centred, beat/bar/
///              downbeat ticks, hot-cue + loop markers, click = seek
///              (drag for scrub-seek), wheel = zoom at cursor.
///   Overview — whole track, region box = current detail viewport,
///              click = jump.
/// All sec↔px math goes through WaveformMapping (tested headless).
class WaveformView : public juce::Component {
public:
    WaveformView(const ThemeEngine& te, int deck)
        : theme_(&te), deck_(deck) {
        detail_.pxPerSec = 160.0;   // sensible DJ zoom default
    }

    // ---- data (non-owning pointers; views re-point per track) ----
    void setWaveform(const WaveformData* wf) { wf_ = wf; repaint(); }
    void setGrid(const BeatGrid* g)          { grid_ = g; repaint(); }
    void setMarkers(std::vector<CueMarker> cues,
                    std::vector<LoopRegion> loops) {
        cues_ = std::move(cues); loops_ = std::move(loops); repaint();
    }
    void setDeckColor(uint32_t argb_) { deckCol_ = argb_; repaint(); }

    /// Feed the latest deck state (position, playing, loop state).
    void setSnapshot(const DeckSnapshot& s) {
        snap_ = &s;
        if (wf_ && s.state == (int)DeckState::Playing)
            detail_.ensureVisible(s.positionSec, 0.35f);
        repaint();
    }

    /// Seek callback — AppShell wires it to rt::DeckSeek.
    std::function<void(double sec)> onSeek;

    void setOverviewRatio(double r) { overviewFrac_ = r; }

    // ---- pure helpers (header-visible for tests) ----
    WaveformMapping&       detailMap()   { return detail_; }
    const WaveformMapping& detailMap() const { return detail_; }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        auto b = getLocalBounds();
        const int ovH = juce::jmax(22,
            (int)(b.getHeight() * overviewFrac_));
        auto det = b.withTrimmedBottom(ovH);
        auto ovw = b.withTop(det.getBottom());

        paintDetail(g, det, p);
        paintOverview(g, ovw, p);
    }

    void resized() override {
        const int ovH = juce::jmax(22,
            (int)(getHeight() * overviewFrac_));
        detail_.setView(snap_ ? snap_->durationSec : 0.0,
                        getWidth());
        detail_.widthPx = getWidth();
        overview_.setView(snap_ ? snap_->durationSec : 0.0, getWidth());
        overview_.widthPx = getWidth();
        overview_.fitAll();
        (void)ovH;
    }

    void mouseDown(const juce::MouseEvent& e) override {
        const int ovH = juce::jmax(22,
            (int)(getHeight() * overviewFrac_));
        const bool inOverview = e.y >= getHeight() - ovH;
        if (inOverview) {
            dragOverview_ = true;
            seekOverview(e.x);
        } else {
            dragDetail_ = true;
            if (onSeek && snap_ && snap_->state != 0)
                onSeek(detail_.seekFromPx((double)e.x));
        }
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (dragOverview_) { seekOverview(e.x); return; }
        if (dragDetail_ && onSeek && snap_ && snap_->state != 0)
            onSeek(detail_.seekFromPx((double)e.x));
    }
    void mouseUp(const juce::MouseEvent&) override {
        dragOverview_ = dragDetail_ = false;
    }
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& w) override {
        if (w.deltaY == 0.0f) return;
        const double anchor = detail_.pxToSec((double)e.x);
        detail_.zoomAt(w.deltaY > 0.0f ? 1.2 : 1.0 / 1.2, anchor);
        repaint();
    }

private:
    // ---------------- detail strip ----------------
    void paintDetail(juce::Graphics& g, juce::Rectangle<int> b,
                     const Palette& p) {
        g.setColour(col(p.surfaceSunk));
        g.fillRect(b);
        if (!wf_ || wf_->empty() || !snap_ || snap_->durationSec <= 0.0) {
            g.setColour(col(p.textFaint));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText(snap_ && snap_->state == (int)DeckState::Loading
                           ? "DECODING…"
                           : "NO WAVEFORM — analysis pending",
                       b, juce::Justification::centred);
            return;
        }
        detail_.setView(snap_->durationSec, b.getWidth());

        // waveform bins — one per pixel column, cheap aggregation
        const int n = b.getWidth();
        int64_t f0 = 0, f1 = 0;
        detail_.frameRange(f0, f1, wf_->sampleRate);
        wf_->query(f0, f1, n, bins_, 0);

        const float midY = (float)b.getCentreY();
        const float halfH = (float)b.getHeight() * 0.46f;
        const float playX = (float)detail_.secToPx(snap_->positionSec);
        const uint32_t wc = deckCol_ ? deckCol_
                                     : theme_->theme().accentColor();

        // loops behind the waveform
        for (auto& lr : loops_) {
            const float x0 = (float)detail_.secToPx(lr.inSec);
            const float x1 = (float)detail_.secToPx(lr.outSec);
            g.setColour(col(p.loopRegion));
            g.fillRect(x0, (float)b.getY(), x1 - x0, (float)b.getHeight());
            g.setColour(col(p.loopEdge));
            g.drawVerticalLine((int)x0, (float)b.getY(),
                               (float)b.getBottom());
            g.drawVerticalLine((int)x1, (float)b.getY(),
                               (float)b.getBottom());
        }

        // RMS halo + min/max columns — split at playhead hue
        for (int x = 0; x < n; ++x) {
            const WaveformBin& bn = bins_[(size_t)x];
            const float x0 = (float)(b.getX() + x);
            const bool played = x0 < playX + b.getX();
            const float env  = std::max(std::abs(bn.min),
                                        std::abs(bn.max));
            const float rmsV = std::abs(bn.rms);
            g.setColour(col(wc).withAlpha(played ? 0.95f : 0.45f));
            g.drawVerticalLine(b.getX() + x, midY - env * halfH,
                               midY + env * halfH);
            g.setColour(col(p.waveRms).withAlpha(played ? 0.9f : 0.35f));
            g.drawVerticalLine(b.getX() + x, midY - rmsV * halfH * 0.8f,
                               midY + rmsV * halfH * 0.8f);
        }

        // beatgrid markers — only when dense enough to read
        if (grid_ && !grid_->empty() && detail_.pxPerSec >= 20.0) {
            markers_.clear();
            collectGridMarkers(detail_, wf_->sampleRate,
                [&](int64_t f) { return grid_->frameToBeat(f); },
                [&](double beat) { return grid_->beatToFrame(beat); },
                [&](double beat) { return grid_->isDownbeat(beat); },
                markers_);
            for (auto& mk : markers_) {
                const int x = b.getX() + (int)std::lround(mk.px);
                const uint32_t c = mk.kind == 2 ? p.gridDownbeat
                                 : mk.kind == 1 ? p.gridBar
                                                : p.gridBeat;
                const int hgt = mk.kind == 2 ? b.getHeight()
                              : mk.kind == 1 ? (int)(b.getHeight() * 0.66f)
                                             : (int)(b.getHeight() * 0.4f);
                g.setColour(col(c));
                g.drawVerticalLine(x, (float)b.getBottom() - hgt,
                                   (float)b.getBottom());
            }
        }

        // hot-cue ticks + labels
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        for (auto& cm : cues_) {
            const float x = (float)(b.getX() + detail_.secToPx(cm.sec));
            g.setColour(col(cm.argb));
            g.drawVerticalLine((int)x, (float)b.getY(),
                               (float)b.getBottom());
            juce::Path tri;
            tri.addTriangle(x, (float)b.getY(), x + 7.0f,
                            (float)b.getY(), x + 3.5f,
                            (float)b.getY() + 6.0f);
            g.fillPath(tri);
            g.drawText(cm.label, (int)x + 5, b.getY() + 4, 60, 12,
                       juce::Justification::left);
        }

        // playhead
        g.setColour(col(p.playhead));
        g.drawVerticalLine((int)std::lround(playX + b.getX()),
                           (float)b.getY(), (float)b.getBottom());
        juce::Path ph;
        ph.addTriangle(playX + b.getX() - 5.0f, (float)b.getY(),
                       playX + b.getX() + 5.0f, (float)b.getY(),
                       playX + b.getX(), (float)b.getY() + 7.0f);
        g.fillPath(ph);
        g.setColour(col(p.borderSoft));
        g.drawRect(b, 1);
    }

    // ---------------- overview strip ----------------
    void paintOverview(juce::Graphics& g, juce::Rectangle<int> b,
                       const Palette& p) {
        g.setColour(col(p.surfaceSunk));
        g.fillRect(b);
        g.setColour(col(p.borderSoft));
        g.drawHorizontalLine(b.getY(), (float)b.getX(),
                             (float)b.getRight());
        if (!wf_ || wf_->empty() || !snap_ || snap_->durationSec <= 0.0)
            return;
        overview_.setView(snap_->durationSec, b.getWidth());
        overview_.fitAll();

        const int n = b.getWidth();
        int64_t f0 = 0, f1 = 0;
        overview_.frameRange(f0, f1, wf_->sampleRate);
        wf_->query(f0, f1, n, obins_, 0);

        const float midY = (float)b.getCentreY();
        const float halfH = (float)b.getHeight() * 0.45f;
        const float playX = (float)(b.getX() +
                                    overview_.secToPx(snap_->positionSec));
        const uint32_t wc = deckCol_ ? deckCol_
                                     : theme_->theme().accentColor();
        for (int x = 0; x < n; ++x) {
            const WaveformBin& bn = obins_[(size_t)x];
            const float env = std::max(std::abs(bn.min), std::abs(bn.max));
            const bool played = b.getX() + x < playX;
            g.setColour(col(wc).withAlpha(played ? 0.9f : 0.35f));
            g.drawVerticalLine(b.getX() + x, midY - env * halfH,
                               midY + env * halfH);
        }

        // viewport window of the detail strip
        const double v0 = overview_.secToPx(detail_.scrollSec);
        const double v1 = overview_.secToPx(detail_.endSec());
        g.setColour(col(p.text).withAlpha(0.20f));
        g.fillRect((float)(b.getX() + v0), (float)b.getY(),
                   (float)(v1 - v0), (float)b.getHeight());
        g.setColour(col(p.text).withAlpha(0.6f));
        g.drawRect((float)(b.getX() + v0), (float)b.getY(),
                   (float)(v1 - v0), (float)b.getHeight(), 1.0f);

        g.setColour(col(p.playhead));
        g.drawVerticalLine((int)std::lround(playX), (float)b.getY(),
                           (float)b.getBottom());
        g.drawRect(b, 1);
    }

    void seekOverview(int px) {
        if (!onSeek || !snap_) return;
        onSeek(overview_.seekFromPx((double)px));
    }

    const ThemeEngine* theme_;
    int deck_ = 0;
    const WaveformData* wf_ = nullptr;
    const BeatGrid*     grid_ = nullptr;
    const DeckSnapshot* snap_ = nullptr;
    uint32_t deckCol_ = 0;
    double overviewFrac_ = 0.24;

    WaveformMapping detail_, overview_;
    bool dragOverview_ = false, dragDetail_ = false;

    // reusable buffers — no per-frame allocations after first resize
    std::vector<WaveformBin> bins_, obins_;
    std::vector<GridMarker>  markers_;
    std::vector<CueMarker>   cues_;
    std::vector<LoopRegion>  loops_;
};

} // namespace onda::ui
