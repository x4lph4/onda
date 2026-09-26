#pragma once

#include <string>
#include <vector>

#include "LibraryTypes.h"

namespace onda::library {

/// Turns FilterCond lists into a parameterized WHERE fragment.
/// Field names are whitelisted — user text never reaches SQL.
///
/// Field -> column map (a.* = analysis join, t.* = tracks):
///   title/artist/album/album_artist/genre/filename/comments/format/path
///   bpm/key/rating/color/energy/duration/year/track_num/date_added/
///   play_count/last_played/file_size/status/tags/analyzed/missing
class FilterEngine {
public:
    /// Musical-key compatibility for harmonic mixing (Camelot-style
    /// neighbourhood over the 24-key standard index space):
    ///   same key, +-1 semitone same mode, relative major/minor,
    ///   dominant/subdominant (+-7 semitones same mode).
    static std::vector<int> compatibleKeys(int keyIndex) {
        if (keyIndex < 0 || keyIndex > 23) return {};
        const bool minor = keyIndex >= 12;
        const int  pc    = keyIndex % 12;
        auto rel = [&](int p, bool m) { return m ? p + 12 : p; };
        std::vector<int> out;
        out.push_back(keyIndex);
        out.push_back(rel((pc + 1) % 12, minor));
        out.push_back(rel((pc + 11) % 12, minor));
        out.push_back(rel((pc + 7) % 12, minor));
        out.push_back(rel((pc + 5) % 12, minor));
        out.push_back(rel(pc, !minor));            // relative maj/min
        return out;
    }

    /// WHERE clause + bound values (text via bind, numbers inlined —
    /// they're parsed doubles, never raw user text).
    static std::string where(const std::vector<FilterCond>& conds,
                             std::vector<std::string>& binds) {
        return whereJoin(conds, binds, false);
    }

    /// §22 — same as where() but OR-joins the conditions (smart
    /// playlist "match any"). negate is honoured per-condition.
    static std::string whereJoin(const std::vector<FilterCond>& conds,
                                 std::vector<std::string>& binds,
                                 bool anyOf) {
        std::string w;
        for (const auto& c : conds) {
            std::string cl = clause(c, binds);
            if (cl.empty()) continue;
            if (!w.empty()) w += anyOf ? " OR " : " AND ";
            w += "(" + cl + ")";
        }
        return w;
    }

    /// One condition → SQL fragment (public for SmartPlaylistEngine's
    /// nested groups). Unknown fields → "" (skipped).
    static std::string clause(const FilterCond& c,
                              std::vector<std::string>& binds) {
        std::string cl = one(c, binds);
        if (cl.empty()) return cl;
        return c.negate ? "NOT (" + cl + ")" : cl;
    }

    static bool hasFilters(const QuerySpec& q) {
        return !q.filters.empty() || !q.search.empty();
    }

private:
    static const char* column(const std::string& f) {
        static const std::pair<const char*, const char*> map[] = {
            { "title", "t.title" }, { "artist", "t.artist" },
            { "album", "t.album" }, { "album_artist", "t.album_artist" },
            { "genre", "t.genre" }, { "filename", "t.filename" },
            { "comments", "t.comments" }, { "format", "t.format" },
            { "label", "t.label" }, { "composer", "t.composer" },
            { "path", "t.path" },   { "bpm", "a.bpm" },
            { "analysis_state", "t.analysis_state" },
            { "play_state", "t.play_state" },
            { "key", "a.key_index" }, { "key_index", "a.key_index" },
            { "rating", "t.rating" }, { "color", "t.color" },
            { "energy", "t.energy" }, { "duration", "t.duration_sec" },
            { "year", "t.year" }, { "track_num", "t.track_num" },
            { "date_added", "t.date_added" }, { "play_count", "t.play_count" },
            { "last_played", "t.last_played" }, { "file_size", "t.file_size" },
            { "status", "t.status" },
        };
        for (auto& [k, v] : map) if (f == k) return v;
        return nullptr;
    }

    static std::string numOp(CmpOp op) {
        switch (op) {
            case CmpOp::Eq:  return " = ";   case CmpOp::Neq: return " != ";
            case CmpOp::Lt:  return " < ";   case CmpOp::Lte: return " <= ";
            case CmpOp::Gt:  return " > ";   case CmpOp::Gte: return " >= ";
            default: return " = ";
        }
    }

    static std::string one(const FilterCond& c, std::vector<std::string>& binds) {
        // pseudo-fields first
        if (c.field == "analyzed")
            return c.a != 0.0 ? "a.track_id IS NOT NULL" : "a.track_id IS NULL";
        if (c.field == "missing")
            return "t.status " + std::string(c.a != 0.0 ? "= 1" : "!= 1");
        // §36/§37 — presence pseudo-fields over related tables
        if (c.field == "waveform")
            return std::string(c.a != 0.0 ? "" : "NOT ")
                + "EXISTS(SELECT 1 FROM waveforms wf WHERE wf.track_id=t.id)";
        if (c.field == "beatgrid")
            return std::string(c.a != 0.0 ? "" : "NOT ")
                + "EXISTS(SELECT 1 FROM beatgrids bg WHERE bg.track_id=t.id)";
        if (c.field == "needs_review")
            return "COALESCE(a.needs_review,0) "
                + std::string(c.a != 0.0 ? "!= 0" : "= 0");
        if (c.field == "tags") {
            binds.push_back("%" + c.text + "%");
            return "t.id IN (SELECT track_id FROM track_tags tt "
                   "JOIN tags g ON g.id = tt.tag_id WHERE g.name LIKE ?)";
        }
        if (c.field == "key_compatible") {
            const auto keys = compatibleKeys((int)c.a);
            if (keys.empty()) return "1 = 1";
            std::string s = "a.key_index IN (";
            for (size_t i = 0; i < keys.size(); ++i)
                s += (i ? "," : "") + std::to_string(keys[i]);
            return s + ")";
        }

        const char* col = column(c.field);
        if (!col) return {};
        std::string out = col;
        switch (c.op) {
            case CmpOp::Contains:
                binds.push_back("%" + c.text + "%");
                return out + " LIKE ?";
            case CmpOp::StartsWith:
                binds.push_back(c.text + "%");
                return out + " LIKE ?";
            case CmpOp::NotIn: {
                if (c.ints.empty()) return "1 = 1";
                out += " NOT IN (";
                for (size_t i = 0; i < c.ints.size(); ++i)
                    out += (i ? "," : "") + std::to_string(c.ints[i]);
                return out + ")";
            }
            case CmpOp::IsNull:
                return out + " IS NULL";
            case CmpOp::Between:
                return out + " BETWEEN " + std::to_string(c.a)
                       + " AND " + std::to_string(c.b);
            case CmpOp::In: {
                if (c.ints.empty()) return "1 = 1";
                out += " IN (";
                for (size_t i = 0; i < c.ints.size(); ++i)
                    out += (i ? "," : "") + std::to_string(c.ints[i]);
                return out + ")";
            }
            case CmpOp::Eq:
                if (!c.text.empty()) { binds.push_back(c.text); return out + " = ?"; }
                return out + " = " + std::to_string(c.a);
            default:
                return out + numOp(c.op) + std::to_string(c.a);
        }
    }
};

} // namespace onda::library
