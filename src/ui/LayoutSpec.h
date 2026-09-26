#pragma once

#include <algorithm>
#include <vector>

namespace onda::ui {

/// Pure layout math — given a window size + preferences, compute the
/// shell regions. JUCE components consume the rectangles; tests assert
/// the priorities without a window.
///
/// Regions (top→bottom, left→right):
///   topBar ─ full-width strip
///   decks  ─ performance row: DeckView A | mixer | DeckView B,
///            waveform strip above decks
///   mixer  ─ channel strips + master column (part of the deck row)
///   fx     ─ fx strip between decks and mixer (optional)
///   browser─ bottom panel (search + table + playlists)
///
/// Responsive rule: when space runs out, collapse panels in this
/// order — never transport/performance controls:
///   fx strip → browser shrink to min → fx hidden → browser hidden →
///   deck waveform shrinks → (floor: never below min sizes).

struct Rect { int x = 0, y = 0, w = 0, h = 0;
    bool valid() const { return w > 0 && h > 0; } };

struct ShellLayout {
    Rect topBar;
    Rect deckRow;         // whole performance row
    Rect waveA, waveB;    // waveform strips over each deck
    Rect deckA, deckB;    // deck control blocks
    Rect mixer;           // center column
    Rect fx;              // fx strip (h==0 when hidden)
    Rect browser;         // bottom panel (h==0 when hidden)
    bool fxVisible = false, browserVisible = false;
};

struct LayoutPrefs {
    bool browserOpen = true;
    bool fxOpen      = true;
    int  browserH    = 260;   // preferred
    int  fxH         = 96;
    int  waveH       = 84;
    int  mixerW      = 340;
};

class LayoutSpec {
public:
    static constexpr int kMinWinW = 960, kMinWinH = 540;
    static constexpr int kMinBrowserH = 140;
    static constexpr int kMinDeckRowH = 300;
    static constexpr int kMinWaveH = 40;

    static ShellLayout compute(int winW, int winH, int topBarH,
                               const LayoutPrefs& p) {
        ShellLayout L;
        L.topBar = { 0, 0, winW, topBarH };
        int y = topBarH, h = winH - topBarH;
        if (h <= 0) return L;

        // --- bottom browser panel ---
        // Reserve room for the fx strip before the browser: on short
        // windows the browser drops to its minimum rather than eating
        // the fx/deck space. (collapse order documented above)
        if (p.browserOpen && h > kMinDeckRowH + kMinBrowserH) {
            L.browserVisible = true;
            const int reserve = p.fxOpen ? p.fxH : 0;
            const int hi = std::max(kMinBrowserH,
                                    h - kMinDeckRowH - reserve);
            const int bh = std::clamp(p.browserH, kMinBrowserH, hi);
            L.browser = { 0, winH - bh, winW, bh };
            h -= bh;
        }

        // --- fx strip between deck row and browser ---
        if (p.fxOpen && h > kMinDeckRowH + p.fxH) {
            L.fxVisible = true;
            L.fx = { 0, y + h - p.fxH, winW, p.fxH };
            h -= p.fxH;
        }

        // --- performance row fills the rest ---
        L.deckRow = { 0, y, winW, h };
        const int waveH = std::clamp(p.waveH, kMinWaveH,
                                     std::max(kMinWaveH, h / 3));
        const int ctrlH = h - waveH;
        const int halfW = (winW - p.mixerW) / 2;
        const int deckW = std::max(0, halfW);

        L.deckA = { 0, y + waveH, deckW, ctrlH };
        L.mixer = { deckW, y + waveH,
                    winW - 2 * deckW, ctrlH };
        L.deckB = { winW - deckW, y + waveH, deckW, ctrlH };
        L.waveA = { 0, y, winW / 2, waveH };
        L.waveB = { winW - winW / 2, y, winW - winW / 2, waveH };
        return L;
    }
};

} // namespace onda::ui
