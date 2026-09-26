#pragma once

#include <algorithm>
#include <cstdint>

namespace onda::ui {

/// Sec↔pixel transform for waveform views — pure math, no JUCE, no
/// audio. WaveformView uses it to convert (bounds, zoom, scroll,
/// playhead) into the frame range to query from WaveformData, and to
/// translate clicks into seeks.
///
/// Two display modes share it:
///   ScrollView  — zoomed detail: content scrolls, playhead fixed or free
///   Overview    — whole track fit to width (pxPerSec = w/duration)
struct WaveformMapping {
    double durationSec = 0.0;
    double pxPerSec    = 100.0;   // zoom: detail ~50..2000, overview computed
    double scrollSec   = 0.0;     // sec at left edge (ScrollView)
    int    widthPx     = 0;

    // ---- configuration ----
    static constexpr double kMinPxPerSec = 4.0;
    static constexpr double kMaxPxPerSec = 2000.0;

    void setView(double durSec, int wPx) {
        durationSec = std::max(0.0, durSec);
        widthPx     = std::max(0, wPx);
        clampScroll();
    }

    /// Overview mode: fit whole track into the view.
    void fitAll() {
        pxPerSec = durationSec > 0.0 && widthPx > 0
                 ? widthPx / durationSec : kMinPxPerSec;
        scrollSec = 0.0;
    }

    void zoomAt(double factor, double anchorSec) {
        const double next =
            std::clamp(pxPerSec * factor, kMinPxPerSec, kMaxPxPerSec);
        // keep anchorSec at the same pixel position
        scrollSec = anchorSec - (anchorSec - scrollSec) * (pxPerSec / next);
        pxPerSec = next;
        clampScroll();
    }

    double visibleSec() const { return widthPx / pxPerSec; }
    double endSec()     const { return scrollSec + visibleSec(); }

    /// ScrollView: keep the playhead inside a margin of the view.
    void ensureVisible(double posSec, double marginFrac = 0.15) {
        const double vis = visibleSec();
        const double m   = vis * marginFrac;
        if (posSec < scrollSec + m)        scrollSec = posSec - m;
        else if (posSec > endSec() - m)    scrollSec = posSec - vis + m;
        clampScroll();
    }

    void scrollBy(double ds) { scrollSec += ds; clampScroll(); }
    void scrollTo(double s)  { scrollSec = s;  clampScroll(); }
    void clampScroll() {
        scrollSec = std::clamp(
            scrollSec, 0.0, std::max(0.0, durationSec - visibleSec()));
        if (visibleSec() >= durationSec) scrollSec = 0.0;
    }

    // ---- transforms ----
    double secToPx(double sec) const { return (sec - scrollSec) * pxPerSec; }
    double pxToSec(double px)  const { return scrollSec + px / pxPerSec; }

    /// Frame range to feed WaveformData::query (mono frames at rate).
    void frameRange(int64_t& begin, int64_t& end, double rate) const {
        begin = (int64_t)(scrollSec * rate);
        end   = (int64_t)(endSec() * rate);
    }

    /// Column-wise seek target: clamp px into the view, clamp sec into
    /// the track — a drag past the edges still seeks sanely.
    double seekFromPx(double px) const {
        return std::clamp(pxToSec(std::clamp(px, 0.0, (double)widthPx)),
                          0.0, durationSec);
    }
};

/// Beat/bar marker computation for a visible window — given grid
/// access functions (beatToFrame, isDownbeat, barOf), emit the list of
/// marker x-positions with kind. JUCE-free so tests verify marker math
/// without a component.
struct GridMarker { double px; int kind; int index; }; // kind 0 beat,1 bar,2 downbeat

/// Emit one marker per beat inside the visible window.
///   frameToBeat(frame) -> fractional beat index at a source frame
///   beatToFrame(beat)  -> source frame of an integer beat index
///   isDownbeat(beat)   -> first beat of a bar (bar boundary)
/// kind: 0 plain beat, 1 bar-start, 2 downbeat (every 4th bar-start).
/// markers[] may be reused across frames — no allocation churn when
/// callers keep it as a member.
template <typename F2B, typename B2F, typename Down>
inline void collectGridMarkers(
    const WaveformMapping& m, double rate,
    F2B frameToBeat, B2F beatToFrame, Down isDownbeat,
    std::vector<GridMarker>& out, int maxMarkers = 4096) {
    out.clear();
    if (rate <= 0.0 || m.pxPerSec <= 0.0) return;
    const double startF = m.scrollSec * rate;
    const double endF   = m.endSec()   * rate;
    // floor the fractional beat to the integer beat ≤ window start
    int64_t beat = (int64_t)frameToBeat((int64_t)startF);
    if (beat < 0) beat = 0;
    while (beat > 0 && beatToFrame(beat) > startF) --beat;
    int bars = 0;
    for (; (int)out.size() < maxMarkers; ++beat) {
        const double f = beatToFrame((double)beat);
        if (f < 0) continue;               // grid doesn't reach back
        if (f > endF) break;
        const double px = m.secToPx(f / rate);
        const bool db = isDownbeat((double)beat);
        const int kind = db ? (++bars % 4 == 1 ? 2 : 1) : 0;
        out.push_back({ px, kind, (int)beat });
    }
}

} // namespace onda::ui
