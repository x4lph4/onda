#pragma once

#include <cctype>
#include <string>

#include "../analysis/KeyAnalyzer.h"

namespace onda::library {

/// §43 — key display notation. The stored value is always the key
/// index 0..23 (majors C..B = 0..11, minors Cm..Bm = 12..23, -1
/// unknown); notation is a presentation concern only.
enum class KeyNotation { Standard, Camelot, OpenKey };

class KeyNotationConverter {
public:
    /// Display name for a key index in the given notation.
    /// -1/out-of-range → "?".
    static const char* name(int key, KeyNotation n) {
        if (key < 0 || key >= 24) return "?";
        switch (n) {
            case KeyNotation::Camelot:
                return KeyAnalyzer::camelotName(key);
            case KeyNotation::OpenKey:
                return openName(key);
            case KeyNotation::Standard:
            default:
                return KeyAnalyzer::standardName(key);
        }
    }

    /// Parse a key name in any supported notation → 0..23, or -1.
    /// Accepts: "Am"/"C"/"F#m" (standard, also flats "Dbm"),
    /// "8A"/"12B" (Camelot), "1d"/"5m" (OpenKey). Case-insensitive.
    static int parse(const std::string& raw) {
        std::string s = trim(raw);
        if (s.empty()) return -1;

        // leading digits → Camelot (nA/nB) or OpenKey (nd/nm)
        size_t i = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        if (i > 0 && i < s.size()) {
            const int n = std::atoi(s.substr(0, i).c_str());
            const char suf = (char)std::tolower((unsigned char)s[i]);
            if (n >= 1 && n <= 12) {
                if (suf == 'a') return camelotToIndex(n, true);
                if (suf == 'b') return camelotToIndex(n, false);
                if (suf == 'm') return openToIndex(n, true);
                if (suf == 'd') return openToIndex(n, false);
            }
            return -1;
        }

        // standard note name + optional #/b + optional m
        i = 0;
        int pc = -1;
        const char c0 = (char)std::toupper((unsigned char)s[0]);
        switch (c0) {
            case 'C': pc = 0; break; case 'D': pc = 2; break;
            case 'E': pc = 4; break; case 'F': pc = 5; break;
            case 'G': pc = 7; break; case 'A': pc = 9; break;
            case 'B': pc = 11; break; default: return -1;
        }
        i = 1;
        if (i < s.size() && (s[i] == '#' || s[i] == 'b')) {
            pc = (pc + (s[i] == '#' ? 1 : 11)) % 12;
            ++i;
        }
        bool minor = false;
        if (i < s.size() && std::tolower((unsigned char)s[i]) == 'm') {
            minor = true; ++i;
        }
        if (i != s.size()) return -1; // trailing garbage
        return minor ? pc + 12 : pc;
    }

    /// Wheel neighbours used for harmonic-mixing filters:
    /// same key, ±1 wheel step, relative major/minor.
    /// (Moved here as the canonical home; FilterEngine keeps its own
    /// copy internally for SQL generation.)
    static void compatibleIndices(int key, int out[4]) {
        // reuse the same table math as the camelot map
        const bool minor = key >= 12;
        const int wheel = minor ? kMinorWheel[key - 12] : kMajorWheel[key];
        int idx = 0;
        out[idx++] = key;
        out[idx++] = wheelStep(wheel, +1, minor);
        out[idx++] = wheelStep(wheel, -1, minor);
        out[idx++] = relativeFlip(key);
    }

private:
    // Camelot wheel number per pitch class (index into kNames order).
    static constexpr int kMajorWheel[12] = { 8, 3, 10, 5, 12, 7,
                                             2, 9, 4, 11, 6, 1 };
    static constexpr int kMinorWheel[12] = { 5, 12, 7, 2, 9, 4,
                                             11, 6, 1, 8, 3, 10 };

    static const char* openName(int key) {
        static thread_local char buf[8];
        const bool minor = key >= 12;
        const int w = minor ? kMinorWheel[key - 12] : kMajorWheel[key];
        // OpenKey numbering = Camelot shifted by 4 (8B→1d).
        const int n = ((w - 8 + 12) % 12) + 1;
        std::snprintf(buf, sizeof(buf), "%d%c", n, minor ? 'm' : 'd');
        return buf;
    }

    static int camelotToIndex(int n, bool minor) {
        const auto* wheel = minor ? kMinorWheel : kMajorWheel;
        for (int i = 0; i < 12; ++i)
            if (wheel[i] == n) return i + (minor ? 12 : 0);
        return -1;
    }

    static int openToIndex(int n, bool minor) {
        // inverse of openName: w ≡ n + 7 (mod 12)
        const int w = ((n + 6) % 12) + 1;
        return camelotToIndex(w, minor);
    }

    static int wheelStep(int wheel, int delta, bool minor) {
        const int w = ((wheel - 1 + delta + 12) % 12) + 1;
        return camelotToIndex(w, minor);
    }

    static int relativeFlip(int key) {
        // relative major/minor keeps the wheel position
        return key < 12 ? key + 12 : key - 12;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        return s.substr(a, s.find_last_not_of(" \t") - a + 1);
    }
};

} // namespace onda::library
