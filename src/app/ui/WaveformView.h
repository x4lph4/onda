#pragma once

#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "core/analysis/AnalysisTypes.h"
#include "core/analysis/BeatGrid.h"
#include "core/analysis/WaveformData.h"
#include "ui/UiState.h"
#include "ui/WaveformViewEngine.h"

namespace onda::ui {

/// Phase 19 — deck waveform component. Thin JUCE shell over
/// WaveformViewEngine (src/ui — pure model, unit-tested):
///
///   ┌── detail strip ─────────────────────────────────────────────┐
///   │ cached raster: waveform columns + loop fills + bar band +   │
///   │ beat/bar/downbeat ticks (rebuilt only when its cache key    │
///   │ changes — never re-queried per frame)                       │
///   │ live overlays: cue flags, playhead, jump flashes, badges    │
///   ├── overview strip ───────────────────────────────────────────┤
///   │ whole track + detail-viewport window + playhead; drag       │
///   │ inside the window pans, outside seeks                       │
///   └─────────────────────────────────────────────────────────────┘
///
/// Interactions become WaveCmd values — the host (AppShell) routes
/// them to rt commands / PerformanceEngine / the analysis-edit port.
/// The component performs no musical logic itself (§41-42).
class WaveformView : public juce::Component,
                     public juce::DragAndDropTarget {
public:
    WaveformView(const ThemeEngine& te, int deck)
        : theme_(&te), deck_(deck) {
        engine_.detail.pxPerSec = 160.0;  // sensible DJ zoom default
    }

    // ---- data (non-owning pointers; views re-point per track) ----
    void setWaveform(const WaveformData* wf) {
        if (engine_.wf == wf) return;
        engine_.wf = wf; ++dataRev_;
        if (wf && wf->sampleRate > 0.0)
            engine_.detail.setView(wf->numFrames / wf->sampleRate,
                                   engine_.detail.widthPx);
        repaint();
    }
    void setGrid(const BeatGrid* g) {
        if (engine_.grid == g) return;
        engine_.grid = g; ++dataRev_; repaint();
    }
    void setMarkers(std::vector<CueMarker> cues,
                    std::vector<LoopRegion> loops) {
        engine_.cues = std::move(cues);
        engine_.loops = std::move(loops);
        ++engine_.markersRev;            // §47-48 — reindex lazily
        ++dataRev_; repaint();
    }
    void setDeckColor(uint32_t argb) { deckCol_ = argb; repaint(); }

    /// Future phrase markers (§30) — seam only.
    void setPhrases(std::vector<PhraseMarker> p) {
        engine_.phrases = std::move(p); ++dataRev_; repaint();
    }

    /// Feed the latest deck state — drives auto-scroll + playhead
    /// smoothing (the real position is the snapshot's; smoothing is
    /// presentation-only, §10-11).
    void setSnapshot(const DeckSnapshot& s) {
        engine_.snap = &s;
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (lastSnapMs_ > 0.0) {
            const int before = engine_.jumpEvents();
            engine_.tick((now - lastSnapMs_) / 1000.0);
            if (engine_.jumpEvents() > before &&
                engine_.detail.durationSec > 0.0) {
                jumpFlashes_.push_back(
                    { engine_.playhead.lastJumpPosSec, now });
                if (jumpFlashes_.size() > 8)
                    jumpFlashes_.erase(jumpFlashes_.begin());
            }
        }
        lastSnapMs_ = now;
        repaint();
    }

    // ---- commands out — AppShell routes them (§42) ----
    std::function<void(double sec)> onSeek;          // fast seek path
    std::function<void(const WaveCmd&)> onWaveCmd;   // everything else
    /// browser track drag dropped on the waveform → load this path.
    std::function<void(const std::string&)> onTrackDrop;

    // ---- track-drop target ("ondatrack:<path>" var drags) ----
    bool isInterestedInDragSource(
            const juce::DragAndDropTarget::SourceDetails& d) override {
        return d.description.toString()
                   .startsWith(kTrackDragPrefix);
    }
    void itemDragEnter(const juce::DragAndDropTarget::SourceDetails&)
        override { dropHover_ = true; repaint(); }
    void itemDragExit(const juce::DragAndDropTarget::SourceDetails&)
        override { dropHover_ = false; repaint(); }
    void itemDropped(const juce::DragAndDropTarget::SourceDetails& d)
        override {
        dropHover_ = false; repaint();
        const auto p = d.description.toString()
            .substring((int)std::strlen(kTrackDragPrefix))
            .toStdString();
        if (!p.empty() && onTrackDrop) onTrackDrop(p);
    }

    // ---- view-local configuration (command surface) ----
    void setGridEdit(bool on) {
        engine_.interact.gridEdit = on; ++dataRev_;
        curType_ = on ? juce::MouseCursor::CrosshairCursor
                      : juce::MouseCursor::NormalCursor;
        setMouseCursor(juce::MouseCursor(curType_));
        repaint();
    }
    bool gridEdit() const { return engine_.interact.gridEdit; }
    void cycleAutoScroll() {
        engine_.autoScrollMode = (AutoScrollMode)(
            ((int)engine_.autoScrollMode + 1) % 3);
    }
    void zoomIn()  { engine_.zoom.in (engine_.detail,
                     ZoomAnchor::Playhead, 0.0, playheadSec()); }
    void zoomOut() { engine_.zoom.out(engine_.detail,
                     ZoomAnchor::Playhead, 0.0, playheadSec()); }
    void zoomFit() { engine_.detail.fitAll(); }
    void setStyle(int s) { engine_.style = s; ++dataRev_; }
    /// §45 — performance degrade: sheds secondary detail (bar
    /// numbers, plain beats, RMS trace, phrases) to hold FPS.
    void setPerfMode(bool on) {
        engine_.vis.perfMode = on; ++dataRev_; repaint();
    }
    bool perfMode() const { return engine_.vis.perfMode; }
    /// §53 — abort an in-flight scrub (Esc / focus loss).
    void cancelSeekDrag() {
        emit(engine_.interact.cancelSeek());
        repaint();
    }
    bool seekActive() const { return engine_.interact.seekSession_; }

    WaveformViewEngine&       engine()       { return engine_; }
    const WaveformViewEngine& engine() const { return engine_; }
    WaveformMapping&          detailMap()    { return engine_.detail; }
    const WaveformDiagnostics& diag() const  { return engine_.diag; }

    // ======================= paint =======================
    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        auto b = getLocalBounds();
        const int ovH = juce::jmax(22,
            (int)(b.getHeight() * overviewFrac_));
        auto det = b.withTrimmedBottom(ovH);
        auto ovw = b.withTop(det.getBottom());

        engine_.detail.setView(trackSec(), det.getWidth());
        engine_.overview.setView(trackSec(), ovw.getWidth());
        engine_.overview.fitAll();
        engine_.diag.scaleFactor = (double)scaleFactor();

        engine_.buildDetail(frame_);
        paintDetail(g, det, p);
        engine_.buildOverview(ovFrame_);
        paintOverview(g, ovw, p);

        // browser track drag hovering — accent edge + LOAD hint
        if (dropHover_) {
            g.setColour(col(theme_->theme().accentColor())
                            .withAlpha(0.15f));
            g.fillRect(b);
            g.setColour(col(theme_->theme().accentColor()));
            g.drawRect(b.reduced(1), 2);
            g.setFont(juce::Font(juce::FontOptions(
                (float)theme_->metrics().fontMd, juce::Font::bold)));
            g.drawText("LOAD", b, juce::Justification::centred);
        }
    }

    void resized() override {
        engine_.detail.setView(trackSec(), getWidth());
        engine_.overview.setView(trackSec(), getWidth());
        engine_.overview.fitAll();
        waveKey_ = ovKey_ = 0;   // size changed → rebuild rasters
    }

    // ======================= mouse =======================
    void mouseDown(const juce::MouseEvent& e) override {
        const int ovH = overviewH();
        if (e.y >= getHeight() - ovH) {          // overview strip
            dragOver_ = true;
            const auto h = WaveformInteraction::hitTestOverview(
                (double)e.x, engine_.detail, engine_.overview);
            if (h.zone == HitZone::OverviewWindow) {
                engine_.interact.dragZone = HitZone::OverviewWindow;
                engine_.interact.dragLastPx = h.sec;
            } else {
                engine_.interact.dragZone = HitZone::None;
                emit({ WaveCmdKind::Seek, -1,
                       engine_.overview.seekFromPx((double)e.x), 0.0 });
            }
            return;
        }
        dragOver_ = false;
        const auto h = WaveformInteraction::hitTest(
            (double)e.x, (double)e.y, frame_, engine_.detail);
        emit(engine_.interact.press(h, frame_,
                e.mods.isRightButtonDown(), nowMs()));
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (dragOver_) {
            const auto c = engine_.interact.drag((double)e.x,
                                                 engine_.overview);
            if (c.kind == WaveCmdKind::ScrollDetail) {
                engine_.detail.scrollBy(c.a);
                repaint();
            } else if (c.kind == WaveCmdKind::ScrubSeek ||
                       c.kind == WaveCmdKind::Seek) {
                emit({ WaveCmdKind::Seek, -1,
                       engine_.overview.seekFromPx((double)e.x), 0.0 });
            }
            return;
        }
        const auto c = engine_.interact.drag((double)e.x,
                                             engine_.detail);
        // Wave-drags use clamped seeks so dragging past the edge is sane
        if (c.kind == WaveCmdKind::ScrubSeek ||
            c.kind == WaveCmdKind::Seek) {
            emit({ c.kind, c.slot,
                   engine_.detail.seekFromPx((double)e.x), c.b });
        } else {
            emit(c);
        }
        if (c.kind == WaveCmdKind::GridNudgeMs) repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (dragOver_) { dragOver_ = false; return; }
        emit(engine_.interact.release(frame_,
                e.mods.isRightButtonDown(), engine_.detail));
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& w) override {
        if (w.deltaY == 0.0f && w.deltaX == 0.0f) return;
        if (e.mods.isShiftDown()) {              // horizontal scroll
            engine_.detail.scrollBy(-(double)w.deltaY * 1.5
                                    - (double)w.deltaX * 1.5);
        } else if (w.deltaY != 0.0f) {           // cursor-anchored zoom
            engine_.zoom.apply(engine_.detail,
                w.deltaY > 0.0f ? engine_.zoom.step
                                : 1.0 / engine_.zoom.step,
                ZoomAnchor::Cursor, (double)e.x, playheadSec());
        } else {
            engine_.detail.scrollBy(-(double)w.deltaX * 1.5);
        }
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override {
        if (engine_.interact.gridEdit) return;   // crosshair already set
        const auto h = WaveformInteraction::hitTest(
            (double)e.x, (double)e.y, frame_, engine_.detail);
        const auto cur =
            h.zone == HitZone::LoopIn || h.zone == HitZone::LoopOut
                ? juce::MouseCursor::LeftRightResizeCursor
            : h.zone == HitZone::CueFlag
                ? juce::MouseCursor::PointingHandCursor
            : h.zone == HitZone::LoopBody
                ? juce::MouseCursor::DraggingHandCursor
                : juce::MouseCursor::NormalCursor;
        if (cur != curType_) {
            curType_ = cur;
            setMouseCursor(juce::MouseCursor(cur));
        }
    }

    void setOverviewRatio(double r) { overviewFrac_ = r; }

private:
    // ---------------- detail strip ----------------
    void paintDetail(juce::Graphics& g, juce::Rectangle<int> b,
                     const Palette& p) {
        g.setColour(col(p.surfaceSunk));
        g.fillRect(b);
        const float midY = (float)b.getCentreY();

        if (!frame_.hasWave) {
            g.setColour(col(p.textFaint));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            const auto st = engine_.snap
                ? (DeckState)engine_.snap->state : DeckState::Empty;
            g.drawText(st == DeckState::Loading    ? "DECODING…"
                     : st == DeckState::Error      ? "LOAD ERROR"
                     : engine_.snap && !engine_.snap->path.empty()
                                                  ? "ANALYZING…"
                                                    : "EMPTY",
                       b, juce::Justification::centred);
            paintBadges(g, b, p);
            return;
        }

        // ---- cached raster: waveform + loops + band + grid ----
        const float sf = scaleFactor();
        const uint64_t k = rasterKey(engine_.detail, b.getWidth());
        if (k != waveKey_ || waveImg_.isNull()) {
            rebuildDetailRaster(b, midY, sf);
            waveKey_ = k;
            ++engine_.diag.cacheMisses;
        } else {
            ++engine_.diag.cacheHits;
        }
        g.drawImageTransformed(waveImg_,
            juce::AffineTransform::translation((float)b.getX(), 0.0f)
                .scaled(1.0f / sf));

        // ---- live overlays (cheap; change every frame) ----
        paintCueFlags(g, b, p);
        paintPlayhead(g, b, p, frame_.playheadPx);
        // §53 — scrub ghost: preview position while dragging
        if (frame_.previewPx >= 0.0) {
            const float gx = (float)b.getX() + (float)frame_.previewPx;
            g.setColour(col(p.playhead).withAlpha(0.45f));
            g.drawVerticalLine((int)std::lround(gx), (float)b.getY(),
                               (float)b.getBottom());
        }
        paintJumpFlashes(g, b, p);
        paintPhrases(g, b, p);
        paintBadges(g, b, p);

        g.setColour(col(p.borderSoft));
        g.drawRect(b, 1);
        if (engine_.interact.gridEdit) {
            g.setColour(col(p.accent).withAlpha(0.10f));
            g.fillRect(b);
        }
    }

    void rebuildDetailRaster(juce::Rectangle<int> b, float midY,
                             float sf) {
        const int w = juce::jmax(1, (int)(b.getWidth() * sf));
        const int h = juce::jmax(1, (int)(b.getHeight() * sf));
        waveImg_ = juce::Image(juce::Image::RGB, w, h, true);
        juce::Graphics g(waveImg_);
        g.addTransform(juce::AffineTransform::scale(sf));
        const int bx = 0;                       // image-local x origin
        const float halfH = (float)b.getHeight() * 0.46f;
        const double playX = engine_.detail.secToPx(playheadSec());

        // current-bar band behind everything (§29)
        if (frame_.hasBarBand) {
            g.setColour(col(theme_->theme().palette.accent)
                            .withAlpha(0.07f));
            g.fillRect((float)bx + frame_.barBand.x0, 0.0f,
                       frame_.barBand.x1 - frame_.barBand.x0,
                       (float)b.getHeight());
        }
        // loop regions behind the waveform (§20)
        for (auto& lr : frame_.loops) {
            const Palette& p = theme_->theme().palette;
            g.setColour(col(p.loopRegion).withAlpha(
                lr.active ? 1.0f : 0.55f));
            g.fillRect((float)bx + lr.x0, 0.0f, lr.x1 - lr.x0,
                       (float)b.getHeight());
            g.setColour(col(p.loopEdge).withAlpha(
                lr.active ? 1.0f : 0.6f));
            g.drawVerticalLine((int)std::lround(bx + lr.x0), 0.0f,
                               (float)b.getHeight());
            g.drawVerticalLine((int)std::lround(bx + lr.x1), 0.0f,
                               (float)b.getHeight());
        }
        // waveform columns — deck color, played half dimmed, optional
        // crest tint (spectral style: honest crest factor, §9)
        const Palette& p = theme_->theme().palette;
        const uint32_t wc = deckCol_ ? deckCol_
                                     : theme_->theme().accentColor();
        const juce::Colour base = col(wc);
        const juce::Colour hot  = col(p.playhead);
        for (size_t x = 0; x < frame_.cols.size(); ++x) {
            const WaveColumn& c = frame_.cols[x];
            const float x0 = (float)(bx + x);
            const bool played = x0 < playX;
            juce::Colour cEnv = base;
            if (engine_.style == 1)
                cEnv = base.interpolatedWith(hot, c.crest * 0.55f);
            g.setColour(cEnv.withAlpha(played ? 0.95f : 0.45f));
            g.drawVerticalLine((int)std::lround(x0),
                               midY - c.env * halfH,
                               midY + c.env * halfH);
            if (engine_.vis.showRms && !engine_.vis.perfMode) {
                g.setColour(col(p.waveRms).withAlpha(
                    played ? 0.9f : 0.35f));
                g.drawVerticalLine((int)std::lround(x0),
                                   midY - c.rms * halfH * 0.8f,
                                   midY + c.rms * halfH * 0.8f);
            }
        }
        // beat/bar/downbeat ticks + bar numbers (§12-14)
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        for (const auto& m : frame_.beats) {
            const uint32_t c = m.kind == 2 ? p.gridDownbeat
                             : m.kind == 1 ? p.gridBar : p.gridBeat;
            const float hgt = m.kind == 2 ? (float)b.getHeight()
                            : m.kind == 1 ? b.getHeight() * 0.66f
                                          : b.getHeight() * 0.4f;
            float alpha = m.gridEdit ? 0.95f
                        : m.current  ? 0.85f : 0.55f;
            g.setColour(col(c).withAlpha(alpha));
            const float lw = m.kind == 2 || m.gridEdit ? 1.6f : 1.0f;
            g.drawLine((float)bx + m.px, (float)b.getHeight() - hgt,
                       (float)bx + m.px, (float)b.getHeight(), lw);
            if (m.barNo >= 0)
                g.drawText(juce::String(m.barNo),
                           (int)(bx + m.px) + 2, b.getHeight() - 12,
                           34, 10, juce::Justification::left);
        }
    }

    void paintCueFlags(juce::Graphics& g, juce::Rectangle<int> b,
                       const Palette& p) {
        g.setFont(juce::Font(juce::FontOptions(10.0f,
                                             juce::Font::bold)));
        for (const auto& cf : frame_.cues) {
            const float x = (float)b.getX() + cf.px;
            // §25 — the primary cue uses the playhead/readout colour
            // (never a pad colour) so it can't be mistaken for a hot
            // cue; §28 memory cues stay hollow.
            const bool primary = cf.type == 3;
            const bool mem     = cf.type == 1;
            juce::Colour cc = primary ? col(p.text) : col(cf.argb);
            g.setColour(cc.withAlpha(cf.armedDelete ? 0.9f : 0.8f));
            g.drawVerticalLine((int)std::lround(x), (float)b.getY(),
                               (float)b.getBottom());
            juce::Path tri;
            tri.addTriangle(x, (float)b.getY(), x + 7.0f,
                            (float)b.getY(), x + 3.5f,
                            (float)b.getY() + 6.0f);
            if (primary) {
                // diamond glyph — distinct shape, not just colour
                juce::Path dia;
                dia.addQuadrilateral(x, (float)b.getY(),
                                     x + 4.0f, (float)b.getY() + 4.0f,
                                     x, (float)b.getY() + 8.0f,
                                     x - 4.0f, (float)b.getY() + 4.0f);
                g.fillPath(dia);
            } else if (mem) {
                g.strokePath(tri, juce::PathStrokeType(1.4f));
            } else {
                g.fillPath(tri);
            }
            // §33 — collision tier offsets the label vertically; the
            // marker line itself never moves.
            const int ly = b.getY() + 3 + cf.tier * 11;
            g.drawText(cf.armedDelete ? "×" : cf.label,
                       (int)x + 5, ly, 60, 12,
                       juce::Justification::left);
            if (cf.armedDelete) {          // armed → strike cue line
                g.setColour(col(p.danger));
                g.drawLine(x - 3.0f, (float)b.getY() + 1.0f,
                           x + 3.0f, (float)b.getY() + 7.0f, 1.6f);
            }
        }
    }

    void paintPlayhead(juce::Graphics& g, juce::Rectangle<int> b,
                       const Palette& p, double px) {
        const float x = (float)b.getX() + (float)px;
        g.setColour(col(p.playhead));
        g.drawVerticalLine((int)std::lround(x), (float)b.getY(),
                           (float)b.getBottom());
        juce::Path ph;
        ph.addTriangle(x - 5.0f, (float)b.getY(), x + 5.0f,
                       (float)b.getY(), x, (float)b.getY() + 7.0f);
        g.fillPath(ph);
    }

    /// Beat-jump flashes — decaying markers at jump landing points;
    /// the waveform itself is never redrawn for them (§22).
    void paintJumpFlashes(juce::Graphics& g, juce::Rectangle<int> b,
                          const Palette& p) {
        const double now = nowMs();
        for (size_t i = jumpFlashes_.size(); i > 0; --i) {
            const auto& jf = jumpFlashes_[i - 1];
            const double age = now - jf.second;
            if (age > 700.0) {
                jumpFlashes_.erase(jumpFlashes_.begin() + (int)i - 1);
                continue;
            }
            const float a = (float)(1.0 - age / 700.0);
            const float x = (float)b.getX() +
                            (float)engine_.detail.secToPx(jf.first);
            if (x < b.getX() - 8 || x > b.getRight() + 8) continue;
            g.setColour(col(p.accent).withAlpha(0.7f * a));
            g.drawVerticalLine((int)std::lround(x), (float)b.getY(),
                               (float)b.getBottom());
            juce::Path ar;
            ar.addTriangle(x - 4.0f, (float)b.getBottom() - 8.0f,
                           x + 4.0f, (float)b.getBottom() - 8.0f,
                           x, (float)b.getBottom() - 1.0f);
            g.fillPath(ar);
        }
    }

    /// Phrase markers — seam for future detection (§30); nothing is
    /// emitted today but the draw path is ready.
    void paintPhrases(juce::Graphics& g, juce::Rectangle<int> b,
                      const Palette&) {
        // (data path exists in WaveformFrame::phrases; visuals land
        //  with phrase analysis in a later phase)
        (void)g; (void)b;
    }

    /// State badges — right-aligned chips from the frame (§38-40).
    /// Text + severity only here; colors are the theme's.
    void paintBadges(juce::Graphics& g, juce::Rectangle<int> b,
                     const Palette& p) {
        if (frame_.badges.empty()) return;
        g.setFont(juce::Font(juce::FontOptions(9.5f,
                                             juce::Font::bold)));
        int rx = b.getRight() - 4;
        for (size_t i = frame_.badges.size(); i > 0; --i) {
            const auto& bd = frame_.badges[i - 1];
            const juce::String t(bd.text);
            const int w = (int)t.length() * 6 + 10;
            rx -= w;
            const uint32_t cc = bd.severity == 3 ? p.danger
                              : bd.severity == 2 ? p.warn
                              : bd.severity == 1 ? p.accent
                                                 : p.textDim;
            g.setColour(col(p.surface).withAlpha(0.75f));
            g.fillRoundedRectangle((float)rx, (float)b.getY() + 4.0f,
                                   (float)w - 2.0f, 14.0f, 3.0f);
            g.setColour(col(cc));
            g.drawText(t, rx, b.getY() + 4, w - 2, 14,
                       juce::Justification::centred);
            rx -= 4;
        }
    }

    // ---------------- overview strip ----------------
    void paintOverview(juce::Graphics& g, juce::Rectangle<int> b,
                       const Palette& p) {
        g.setColour(col(p.surfaceSunk));
        g.fillRect(b);
        g.setColour(col(p.borderSoft));
        g.drawHorizontalLine(b.getY(), (float)b.getX(),
                             (float)b.getRight());
        if (!ovFrame_.hasWave) { g.drawRect(b, 1); return; }

        const float sf = scaleFactor();
        const uint64_t k = rasterKey(engine_.overview, b.getWidth());
        if (k != ovKey_ || ovImg_.isNull()) {
            rebuildOverviewRaster(b, sf);
            ovKey_ = k;
            ++engine_.diag.cacheMisses;
        } else {
            ++engine_.diag.cacheHits;
        }
        g.drawImageTransformed(ovImg_,
            juce::AffineTransform::translation((float)b.getX(),
                                               (float)b.getY())
                .scaled(1.0f / sf));

        // viewport window of the detail strip
        const double v0 = engine_.overview.secToPx(
            engine_.detail.scrollSec);
        const double v1 = engine_.overview.secToPx(
            engine_.detail.endSec());
        g.setColour(col(p.text).withAlpha(0.20f));
        g.fillRect((float)(b.getX() + v0), (float)b.getY(),
                   (float)(v1 - v0), (float)b.getHeight());
        g.setColour(col(p.text).withAlpha(0.6f));
        g.drawRect((float)(b.getX() + v0), (float)b.getY(),
                   (float)(v1 - v0), (float)b.getHeight(), 1.0f);

        // playhead — real position (not smoothed) on the overview
        const float x = (float)b.getX() +
                        (float)engine_.overview.secToPx(
                            engine_.snap ? engine_.snap->positionSec
                                         : 0.0);
        g.setColour(col(p.playhead));
        g.drawVerticalLine((int)std::lround(x), (float)b.getY(),
                           (float)b.getBottom());
        g.drawRect(b, 1);
    }

    void rebuildOverviewRaster(juce::Rectangle<int> b, float sf) {
        const int w = juce::jmax(1, (int)(b.getWidth() * sf));
        const int h = juce::jmax(1, (int)(b.getHeight() * sf));
        ovImg_ = juce::Image(juce::Image::RGB, w, h, true);
        juce::Graphics g(ovImg_);
        g.addTransform(juce::AffineTransform::scale(sf));
        const float midY = (float)b.getHeight() * 0.5f;
        const float halfH = (float)b.getHeight() * 0.45f;
        const Palette& p = theme_->theme().palette;
        const double playX = engine_.overview.secToPx(playheadSec());

        for (auto& lr : ovFrame_.loops) {
            g.setColour(col(p.loopRegion).withAlpha(
                lr.active ? 0.9f : 0.5f));
            g.fillRect(lr.x0, 0.0f, lr.x1 - lr.x0,
                       (float)b.getHeight());
        }
        const uint32_t wc = deckCol_ ? deckCol_
                                     : theme_->theme().accentColor();
        for (size_t x = 0; x < ovFrame_.cols.size(); ++x) {
            const float env = ovFrame_.cols[x].env;
            const bool played = (double)x < playX;
            g.setColour(col(wc).withAlpha(played ? 0.9f : 0.35f));
            g.drawVerticalLine((int)std::lround((float)x),
                               midY - env * halfH, midY + env * halfH);
        }
        for (const auto& cf : ovFrame_.cues) {
            g.setColour(col(cf.argb).withAlpha(0.9f));
            g.drawVerticalLine((int)std::lround(cf.px), 0.0f, 4.0f);
        }
    }

    // ---------------- helpers ----------------
    void emit(const WaveCmd& c) {
        if (c.kind == WaveCmdKind::None) return;
        // §53 — SeekCommit lands the scrub's final position through
        // the same transport path; Begin/Cancel only reach onWaveCmd
        // so the host can bracket the transaction.
        if ((c.kind == WaveCmdKind::Seek ||
             c.kind == WaveCmdKind::ScrubSeek ||
             c.kind == WaveCmdKind::SeekCommit) && onSeek) {
            onSeek(c.a);                          // fast path → rt queue
        }
        if (onWaveCmd) onWaveCmd(c);
    }

    uint64_t rasterKey(const WaveformViewport& v, int wPx) const {
        const int64_t fr = engine_.wf ? engine_.wf->numFrames : 0;
        // variant folds style + grid-edit + data revision so marker/
        // cue/style changes invalidate the raster deterministically.
        const int variant = engine_.style
            | (engine_.interact.gridEdit ? 0x10 : 0)
            | ((int)(dataRev_ & 0xFF) << 8);
        return WaveformCache::key(v, wPx, engine_.wf, fr, variant,
                                  (double)scaleFactor());
    }

    double trackSec() const {
        if (engine_.wf && engine_.wf->sampleRate > 0.0 &&
            engine_.wf->numFrames > 0)
            return engine_.wf->numFrames / engine_.wf->sampleRate;
        return engine_.snap ? engine_.snap->durationSec : 0.0;
    }
    double playheadSec() const {
        return engine_.snap ? engine_.snap->positionSec : 0.0;
    }
    int overviewH() const {
        return juce::jmax(22, (int)(getHeight() * overviewFrac_));
    }
    float scaleFactor() const {
        return juce::Component::getApproximateScaleFactorForComponent(
            const_cast<WaveformView*>(this));
    }
    static double nowMs() {
        return juce::Time::getMillisecondCounterHiRes();
    }

    const ThemeEngine* theme_;
    int deck_ = 0;
    uint32_t deckCol_ = 0;
    double overviewFrac_ = 0.24;

    WaveformViewEngine engine_;
    WaveformFrame      frame_, ovFrame_;   // reused render plans

    // raster caches — rebuilt only when their key changes (§43)
    juce::Image waveImg_, ovImg_;
    uint64_t    waveKey_ = 0, ovKey_ = 0;
    uint32_t    dataRev_ = 0;

    bool   dragOver_ = false;
    bool   dropHover_ = false;   // external track drag hovering
    double lastSnapMs_ = 0.0;
    juce::MouseCursor::StandardCursorType curType_ =
        juce::MouseCursor::NormalCursor;
    std::vector<std::pair<double, double>> jumpFlashes_; // sec, ms
};

} // namespace onda::ui
