#pragma once

#include <cstdint>
#include <string>

#include "DesignTokens.h"

namespace onda::ui {

/// Control-state taxonomy shared by every button/control in the app —
/// states are rendered by color AND shape/label, never color alone.
enum class ControlState : int { Off = 0, On, Active, Disabled, Error };

/// Theme variants. Dark is the default and the only fully-tuned one
/// today; Dark HC (high contrast) raises text/border contrast for
/// accessibility. The struct is open — serialized themes can override
/// any token without touching code.
enum class ThemeId : int { Dark = 0, DarkHighContrast = 1 };

struct Theme {
    ThemeId  id      = ThemeId::Dark;
    Palette  palette {};
    Density  density = Density::Comfortable;
    uint32_t accent  = 0;   // 0 → palette.accent
    uint32_t waveA   = 0;   // waveform colors per deck theme (0=default)
    uint32_t waveB   = 0;

    uint32_t accentColor() const { return accent ? accent : palette.accent; }
    uint32_t waveColor(int deck) const {
        if (deck == 0 && waveA) return waveA;
        if (deck == 1 && waveB) return waveB;
        return palette.deckColor(deck);
    }
};

/// Owns the active theme; serializes to a compact `k=v` text block so
/// the app can persist it in kv / settings without a JSON dependency.
class ThemeEngine {
public:
    ThemeEngine() { set(ThemeId::Dark); }

    void set(ThemeId id) {
        theme_ = Theme{};
        theme_.id = id;
        if (id == ThemeId::DarkHighContrast) {
            theme_.palette.text      = argb(255, 250, 252, 255);
            theme_.palette.textDim   = argb(255, 196, 204, 216);
            theme_.palette.textFaint = argb(255, 150, 158, 170);
            theme_.palette.border    = argb(255,  92, 100, 114);
            theme_.palette.borderSoft= argb(255,  70,  77,  89);
            theme_.palette.selection = argb(140,  46, 196, 182);
        }
    }

    const Theme& theme() const { return theme_; }
    Metrics      metrics() const { return metricsFor(theme_.density); }

    void setDensity(Density d) { theme_.density = d; }
    void setAccent(uint32_t a) { theme_.accent = a; }
    void setWaveColor(int idx, uint32_t c) {
        if (idx == 0) theme_.waveA = c; else theme_.waveB = c;
    }

    // ---- persistence ----
    std::string serialize() const;
    bool        deserialize(const std::string& text);

private:
    Theme theme_;
};

} // namespace onda::ui
