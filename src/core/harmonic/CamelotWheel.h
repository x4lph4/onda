#pragma once

#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "../analysis/KeyAnalyzer.h"
#include "HarmonicTypes.h"

namespace onda::harmonic {

/// ------------------------------------------------------------------
/// §11 — canonical Camelot conversion. Majors → B side, minors → A
/// side. Canonical data = key index; Camelot is derived.
/// ------------------------------------------------------------------
struct CamelotKey {
    int  number { 0 };      // 1..12, 0 = invalid
    char side   { '\0' };   // 'A' (minor) | 'B' (major)
    bool valid() const { return number >= 1 && number <= 12 &&
                                (side == 'A' || side == 'B'); }
    std::string str() const {
        return valid() ? std::to_string(number) + side : "?";
    }
    /// Linear position 0..23 (0..11 = A1..A12, 12..23 = B1..B12).
    int pos() const { return valid() ? number - 1 + (side == 'B' ? 12 : 0) : -1; }
};

class CamelotConverter {
public:
    /// Canonical index → Camelot. -1/out-of-range → invalid.
    static CamelotKey fromIndex(int keyIndex) {
        if (keyIndex < 0 || keyIndex >= 24) return {};
        const char* n = KeyAnalyzer::camelotName(keyIndex); // e.g. "8A"
        return parse(n);
    }
    /// Camelot → canonical index. "8A"/"8a"/" 8A " → index; invalid → -1.
    static int toIndex(const std::string& cam) {
        const CamelotKey c = parse(cam);
        if (!c.valid()) return -1;
        return indexOf(c.number, c.side);
    }
    /// Direct conversion (canonical math, no table lookup).
    ///   major: num ≡ pc·7 + 8 (mod 12)  →  pc ≡ (num−8)·7 (mod 12)
    ///   minor: num ≡ pc·7 + 5 (mod 12)  →  pc ≡ (num−5)·7 (mod 12)
    /// (7⁻¹ mod 12 = 7; C=8B, Am=8A per the standard wheel.)
    static int indexOf(int number, char side) {
        if (number < 1 || number > 12) return -1;
        if (side == 'A')
            return 12 + (((number - 5) * 7) % 12 + 12) % 12;
        if (side == 'B')
            return (((number - 8) * 7) % 12 + 12) % 12;
        return -1;
    }
    /// Pitch class + mode → Camelot (inverse of indexOf).
    static CamelotKey fromPitchClassMode(int pc, KeyMode m) {
        if (pc < 0 || pc >= 12) return {};
        auto num = [](int v) { const int n = v % 12; return n == 0 ? 12 : n; };
        if (m == KeyMode::Minor)
            return { num(pc * 7 + 5), 'A' };
        if (m == KeyMode::Major)
            return { num(pc * 7 + 8), 'B' };
        return {};
    }

    static CamelotKey parse(const std::string& s) {
        std::string t;
        for (char c : s) if (c != ' ') t += (char)std::toupper((unsigned char)c);
        if (t.size() < 2) return {};
        const char side = t.back();
        if (side != 'A' && side != 'B') return {};
        t.pop_back();
        const int num = std::atoi(t.c_str());
        if (num < 1 || num > 12) return {};
        return { num, side };
    }
};

/// ------------------------------------------------------------------
/// §12/§14 — the wheel model (pure data; the widget renders it).
/// All 24 positions, current key, and classified neighbours:
///   same · relative (A↔B same number) · adjacent (number±1 same
///   side) · energy (relative of adjacent — power-boost moves).
/// ------------------------------------------------------------------
class CamelotWheel {
public:
    enum class Rel : int { None = 0, Same, Relative, Adjacent, RelativeAdjacent };

    /// 24 wheel slots (A side 0..11, B side 12..23).
    static CamelotKey position(int pos) {
        if (pos < 0 || pos >= 24) return {};
        return { pos % 12 + 1, pos < 12 ? 'A' : 'B' };
    }

    /// Relationship of `candidate` (Camelot pos 0..23) to `current`.
    static Rel relationship(int currentPos, int candidatePos) {
        if (currentPos < 0 || candidatePos < 0) return Rel::None;
        if (currentPos == candidatePos) return Rel::Same;
        const int cn = currentPos % 12, an = candidatePos % 12;
        const bool cA = currentPos < 12, aA = candidatePos < 12;
        if (cn == an && cA != aA) return Rel::Relative;
        const int dn = (an - cn + 12) % 12;
        const bool adjacent = dn == 1 || dn == 11;
        if (adjacent && cA == aA) return Rel::Adjacent;
        if (adjacent && cA != aA) return Rel::RelativeAdjacent;
        return Rel::None;
    }

    /// Camelot positions related to `keyIndex` (-1 → empty).
    /// `highlight` gets per-position Rel for the renderer.
    static void related(int keyIndex,
                        std::array<Rel, 24>& highlight) {
        highlight.fill(Rel::None);
        const int cur = fromIndex(keyIndex).pos();
        if (cur < 0) return;
        for (int p = 0; p < 24; ++p)
            highlight[(size_t)p] = relationship(cur, p);
    }

    /// Canonical key indexes compatible with `keyIndex` under the
    /// classic Camelot rule set (same + relative + ±1 same side).
    static std::vector<int> compatibleIndices(int keyIndex) {
        std::vector<int> out;
        const int cur = fromIndex(keyIndex).pos();
        if (cur < 0) return out;
        for (int p = 0; p < 24; ++p) {
            const Rel r = relationship(cur, p);
            if (r == Rel::Same || r == Rel::Relative || r == Rel::Adjacent)
                out.push_back(CamelotConverter::indexOf(
                    position(p).number, position(p).side));
        }
        return out;
    }

private:
    static CamelotKey fromIndex(int idx) { return CamelotConverter::fromIndex(idx); }
};

} // namespace onda::harmonic
