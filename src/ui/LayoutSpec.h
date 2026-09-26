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
    Rect waveA, waveB;    // waveform strips over each deck (2-deck)
    Rect deckA, deckB;    // deck control blocks (2-deck)
    // Phase 19 §45 — 4-deck capable arrays. In 2-deck mode waves[0..1]
    // mirror waveA/waveB; in 4-deck mode all four split the strip.
    Rect waves[4];
    Rect decks[4];        // deck panels: A C | mixer | D B in 4-deck
    int  deckCount = 2;
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
    int  deckMode    = 2;     // 2 or 4 visible decks (Phase 19 §45)
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

        // ---- deck-count-aware arrays (§45) ----
        L.deckCount = p.deckMode >= 4 ? 4 : 2;
        if (L.deckCount == 4) {
            // waves: four equal columns across the window
            const int qw = winW / 4;
            for (int i = 0; i < 4; ++i)
                L.waves[i] = { i * qw, y,
                               i == 3 ? winW - 3 * qw : qw, waveH };
            // deck panels: A C left of mixer, D B right — mixer keeps
            // its preferred width, panels split the remainder evenly
            const int side = (winW - p.mixerW) / 2;
            const int pw = side / 2;
            L.decks[0] = { 0, y + waveH, pw, ctrlH };            // A
            L.decks[2] = { pw, y + waveH,
                           side - pw, ctrlH };                  // C
            L.decks[3] = { winW - side, y + waveH, pw, ctrlH };  // D
            L.decks[1] = { winW - side + pw, y + waveH,
                           side - pw, ctrlH };                  // B
            L.mixer = { side, y + waveH,
                        winW - 2 * side, ctrlH };
        } else {
            L.waves[0] = L.waveA; L.waves[1] = L.waveB;
            L.decks[0] = L.deckA; L.decks[1] = L.deckB;
        }
        return L;
    }
};

} // namespace onda::ui
