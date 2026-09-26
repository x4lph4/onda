#include "ThemeEngine.h"

#include <sstream>

namespace onda::ui {

std::string ThemeEngine::serialize() const {
    std::ostringstream o;
    o << "theme=" << (int)theme_.id
      << "\ndensity=" << (int)theme_.density
      << "\naccent=" << std::hex << theme_.accent
      << "\nwaveA=" << std::hex << theme_.waveA
      << "\nwaveB=" << std::hex << theme_.waveB;
    return o.str();
}

bool ThemeEngine::deserialize(const std::string& text) {
    Theme t = theme_; // parse into a copy — bad input keeps old theme
    std::istringstream in(text);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr(0, eq);
        const auto val = line.substr(eq + 1);
        try {
            if (key == "theme") {
                const int id = std::stoi(val);
                if (id < 0 || id > (int)ThemeId::DarkHighContrast) return false;
                set((ThemeId)id); t = theme_;
            } else if (key == "density") {
                const int d = std::stoi(val);
                if (d < 0 || d > (int)Density::Expanded) return false;
                t.density = (Density)d;
            } else if (key == "accent") t.accent = (uint32_t)std::stoul(val, nullptr, 16);
            else if (key == "waveA")  t.waveA  = (uint32_t)std::stoul(val, nullptr, 16);
            else if (key == "waveB")  t.waveB  = (uint32_t)std::stoul(val, nullptr, 16);
            else continue;
            any = true;
        } catch (...) { return false; }
    }
    if (any) theme_ = t;
    return any;
}

} // namespace onda::ui
