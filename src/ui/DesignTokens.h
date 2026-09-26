#pragma once

#include <cstdint>

namespace onda::ui {

/// Design-system tokens: pure values, zero JUCE. JUCE components map
/// these to juce::Colour at paint time — tests assert on them raw.
///
/// Palette intent: near-black base with a cyan-teal accent family.
/// Every state that matters (clip, error, sync, cue) ALSO differs in
/// shape/position — never color-only (accessibility rule).

/// 0xAARRGGBB packing used everywhere in the UI layer.
constexpr uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(a) << 24) | (uint32_t(r) << 16)
         | (uint32_t(g) << 8) | uint32_t(b);
}

struct Palette {
    uint32_t background    = argb(255,  13,  15,  18);  // app base
    uint32_t surface       = argb(255,  21,  24,  29);  // panels
    uint32_t surfaceRaised = argb(255,  29,  33,  40);  // cards / knobs
    uint32_t surfaceSunk   = argb(255,   9,  10,  13);  // wells / table bg
    uint32_t border        = argb(255,  44,  49,  58);
    uint32_t borderSoft    = argb(255,  32,  36,  43);

    uint32_t text          = argb(255, 232, 235, 240);
    uint32_t textDim       = argb(255, 148, 155, 166);
    uint32_t textFaint     = argb(255,  96, 102, 112);
    uint32_t textOnAccent  = argb(255,   8,  10,  12);

    uint32_t accent        = argb(255,  46, 196, 182);  // teal-cyan
    uint32_t accentDim     = argb(255,  30, 128, 120);
    uint32_t accentSoft    = argb( 56,  46, 196, 182);

    uint32_t warn          = argb(255, 230, 164,  52);  // amber
    uint32_t danger        = argb(255, 224,  82,  82);  // clip / error
    uint32_t ok            = argb(255,  82, 196, 118);

    uint32_t deckA         = argb(255,  64, 160, 255);  // per-deck hue
    uint32_t deckB         = argb(255, 255, 120,  80);
    uint32_t deckC         = argb(255, 178, 132, 255);
    uint32_t deckD         = argb(255, 255, 196,  64);

    uint32_t waveFill      = argb(255,  52, 140, 170);
    uint32_t wavePlayed    = argb(255,  46, 196, 182);
    uint32_t waveRms       = argb(160, 120, 220, 235);
    uint32_t gridBeat      = argb(120, 120, 130, 145);
    uint32_t gridBar       = argb(160, 170, 180, 195);
    uint32_t gridDownbeat  = argb(200, 235, 240, 250);
    uint32_t playhead      = argb(255, 245, 248, 252);
    uint32_t loopRegion    = argb( 64,  46, 196, 182);
    uint32_t loopEdge      = argb(255,  46, 196, 182);

    uint32_t meterGreen    = argb(255,  74, 214, 126);
    uint32_t meterAmber    = argb(255, 232, 176,  54);
    uint32_t meterRed      = argb(255, 236,  80,  70);
    uint32_t meterBack     = argb(255,  18,  20,  25);

    uint32_t selection     = argb( 90,  46, 196, 182);
    uint32_t rowAlt        = argb(255,  17,  19,  24);
    uint32_t missingRow    = argb(255,  66,  34,  34);

    /// Hue for a deck index (0..3), wraps for spares.
    uint32_t deckColor(int deck) const {
        const uint32_t c[4] = { deckA, deckB, deckC, deckD };
        return c[deck & 3];
    }
};

/// Spacing scale — every component derives gaps from these so the
/// whole UI reflows consistently when density changes.
struct Metrics {
    int gap1 = 2,  gap2 = 4,  gap3 = 8,  gap4 = 12, gap5 = 16, gap6 = 24;
    int radiusSm = 3, radiusMd = 6;
    int knobSm = 34, knobMd = 44, knobLg = 56;
    int faderW = 34, faderH = 150;
    int rowH   = 24;       // table row / list line
    int fontSm = 11, fontMd = 13, fontLg = 16, fontXl = 20;
    int topBarH = 34;
    int transportH = 46;
    int minDeckW = 360;
};

/// UI density: Compact prioritizes controls (smaller fonts/rows),
/// Comfortable is default, Expanded enlarges touch targets.
enum class Density : int { Compact = 0, Comfortable = 1, Expanded = 2 };

inline Metrics metricsFor(Density d) {
    Metrics m;
    switch (d) {
        case Density::Compact:
            m.rowH = 20; m.knobMd = 38; m.fontMd = 12; m.topBarH = 30;
            m.faderH = 132; break;
        case Density::Expanded:
            m.rowH = 30; m.knobMd = 50; m.fontMd = 15; m.topBarH = 40;
            m.faderH = 176; m.transportH = 56; break;
        default: break;
    }
    return m;
}

/// Tri-level meter bands (fractions of full scale, linear).
struct MeterBands { float greenTo = 0.70f, amberTo = 0.90f; };

} // namespace onda::ui
