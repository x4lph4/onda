#pragma once

#include <cctype>
#include <string>

#include "TextFold.h"

namespace onda::library {

/// §20 — opt-in metadata normalization helpers.
///
/// Pure functions: they PROPOSE a normalized value, never write it.
/// The caller (batch "normalize metadata" action, or an editor
/// preview) decides whether to apply — original file tags are never
/// touched without an explicit user gesture.
///
/// Policy (documented):
///   whitespace  — trim + collapse inner runs to one space
///   artist/album/title — whitespace only; casing is left alone
///                 ("beatles" ≠ "Beatles" is a user choice, not ours)
///   genre       — whitespace + canonical case via a small
///                 well-known table (falls back to input case)
///   label       — whitespace only
///   feat/ft     — OPTIONAL: strip "(feat. X)"/"feat."-suffixes from
///                 titles into the artist string when requested
class TagNormalizer {
public:
    /// Trim + collapse whitespace. Basis for every rule below.
    static std::string whitespace(std::string s) {
        std::string out;
        out.reserve(s.size());
        bool pendingSpace = false;
        for (char ch : s) {
            if (std::isspace((unsigned char)ch)) { pendingSpace = !out.empty(); continue; }
            if (pendingSpace) out += ' ';
            pendingSpace = false;
            out += ch;
        }
        return out;
    }

    /// Artist/title/album/label: whitespace normalization only.
    static std::string text(const std::string& s) { return whitespace(s); }

    /// Genre: whitespace + canonical case for well-known genres.
    /// Unknown genres keep their (whitespace-fixed) spelling —
    /// normalization must not mangle names we don't know.
    static std::string genre(const std::string& s) {
        static const char* kGenres[] = {
            "House", "Techno", "Tech House", "Deep House", "Acid House",
            "Progressive House", "Electro House", "Bass House",
            "Drum & Bass", "Dubstep", "Trap", "Trance", "Psytrance",
            "Hardstyle", "Hardcore", "Breakbeat", "Garage", "UK Garage",
            "Hip Hop", "R&B", "Soul", "Funk", "Disco", "Nu-Disco",
            "Indie Dance", "Minimal", "Ambient", "Downtempo", "Reggae",
            "Dancehall", "Pop", "Rock", "Jazz", "Classical", "Latin",
            "Afro House", "Melodic Techno", "Afro Tech", "Amapiano"
        };
        const std::string w = whitespace(s);
        const std::string f = TextFold::fold(w);
        for (const char* g : kGenres)
            if (f == TextFold::fold(g)) return g;
        return w;
    }

    /// Strip a "(feat. X)"/"(ft. X)"/"feat. X"-suffix from a title.
    /// Returns {cleanTitle, featuring} — featuring may be empty.
    /// Opt-in only: callers append `featuring` to artist themselves.
    static std::pair<std::string, std::string>
    splitFeaturing(const std::string& title) {
        const std::string f = TextFold::fold(title);
        size_t pos = std::string::npos;
        for (const char* pat : { "(feat. ", "(feat.", " feat. ",
                                 "(ft. ", "(ft.", " ft. ",
                                 "featuring ", " featuring " }) {
            pos = f.find(pat);
            if (pos != std::string::npos) break;
        }
        if (pos == std::string::npos) return { whitespace(title), {} };
        std::string clean = title.substr(0, pos);
        std::string feat  = title.substr(pos);
        // tidy the featuring string: strip surrounding parens/patterns
        for (char& c : feat)
            if (c == '(' || c == ')') c = ' ';
        feat = whitespace(feat);
        for (const std::string p : { "feat.", "feat", "ft.", "ft",
                                     "featuring" }) {
            if (feat.size() > p.size() &&
                TextFold::fold(feat.substr(0, p.size())) == p &&
                std::isspace((unsigned char)feat[p.size()])) {
                feat = whitespace(feat.substr(p.size()));
                break;
            }
        }
        return { whitespace(clean), feat };
    }
};

} // namespace onda::library
