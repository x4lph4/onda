#pragma once

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "../harmonic/CamelotWheel.h"
#include "KeyNotation.h"
#include "LibraryTypes.h"

namespace onda::library {

/// §13 — structured search tokens on top of free-text FTS.
///
/// Accepted syntax (deliberately small — §13):
///   artist:daft punk       bpm:120-128     genre:house
///   key:Am | 8A | 1d       rating:4+       year:2020-2025
///   tag:warmup             album:"discovery"   title:get lucky
///   bpm:>120  bpm:<130     missing:1       analyzed:1
/// Unrecognised text remains in `search` and goes to FTS as before.
class SearchQuery {
public:
    /// Parse a raw search box string into a QuerySpec:
    /// recognised field tokens → FilterCond entries appended to
    /// `q.filters`; everything else → `q.search`.
    static void parse(const std::string& raw, QuerySpec& q) {
        std::string rest;
        for (const auto& tok : tokenize(raw)) {
            const auto colon = tok.find(':');
            if (colon == std::string::npos || colon == 0) {
                rest += tok + ' ';
                continue;
            }
            const std::string field = lower(tok.substr(0, colon));
            const std::string val = tok.substr(colon + 1);
            if (val.empty() || !applyToken(field, val, q))
                rest += tok + ' ';
        }
        if (!rest.empty() && rest.back() == ' ') rest.pop_back();
        q.search = rest;
    }

    /// Convenience: raw text → complete QuerySpec.
    static QuerySpec toSpec(const std::string& raw) {
        QuerySpec q;
        parse(raw, q);
        return q;
    }

private:
    /// Split on whitespace, honouring "double quotes" for phrases.
    static std::vector<std::string> tokenize(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        bool inQ = false;
        for (char ch : s) {
            if (ch == '"') { inQ = !inQ; continue; }
            if (std::isspace((unsigned char)ch) && !inQ) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur += ch;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static double num(const std::string& v) {
        return std::strtod(v.c_str(), nullptr);
    }

    /// value patterns: "a-b" range, "a+" minimum, ">a" / "<a", plain.
    static bool numericCond(const std::string& v, FilterCond& c) {
        if (v.front() == '>') { c.op = CmpOp::Gt;  c.a = num(v.substr(1)); return true; }
        if (v.front() == '<') { c.op = CmpOp::Lt;  c.a = num(v.substr(1)); return true; }
        if (v.back() == '+' && v.size() > 1) {
            c.op = CmpOp::Gte; c.a = num(v.substr(0, v.size() - 1));
            return true;
        }
        const auto dash = v.find('-');
        if (dash != std::string::npos && dash > 0 && dash < v.size() - 1) {
            c.op = CmpOp::Between;
            c.a = num(v.substr(0, dash));
            c.b = num(v.substr(dash + 1));
            return true;
        }
        c.op = CmpOp::Eq;
        c.a = num(v);
        return true;
    }

    static bool applyToken(const std::string& f, const std::string& v,
                           QuerySpec& q) {
        FilterCond c;
        if (f == "bpm" || f == "rating" || f == "year" || f == "duration"
            || f == "plays" || f == "playcount") {
            c.field = f == "plays" || f == "playcount" ? "play_count" : f;
            numericCond(v, c);
            q.filters.push_back(c);
            return true;
        }
        if (f == "artist" || f == "album" || f == "genre" || f == "title"
            || f == "label" || f == "filename" || f == "comment"
            || f == "comments" || f == "tag" || f == "tags"
            || f == "path" || f == "color") {
            c.field = f == "tag" ? "tags"
                    : f == "comment" ? "comments" : f;
            c.op = CmpOp::Contains;
            c.text = v;
            q.filters.push_back(c);
            return true;
        }
        if (f == "key") {
            const int k = KeyNotationConverter::parse(v);
            if (k < 0) return false;
            c.field = "key_index";
            c.op = CmpOp::Eq;
            c.a = (double)k;
            q.filters.push_back(c);
            return true;
        }
        // Phase 22 — harmonic-mixing tokens
        if (f == "camelot") {
            const int k = harmonic::CamelotConverter::toIndex(v);
            if (k < 0) return false;
            c.field = "key_index";
            c.op = CmpOp::Eq;
            c.a = (double)k;
            q.filters.push_back(c);
            return true;
        }
        if (f == "mode") {
            const std::string m = lower(v);
            if (m != "major" && m != "minor" && m != "unknown") return false;
            c.field = "key_mode";
            c.op = CmpOp::Eq;
            c.a = m == "minor" ? 1.0 : m == "unknown" ? -1.0 : 0.0;
            q.filters.push_back(c);
            return true;
        }
        if (f == "keyconf" || f == "keyconfidence") {
            c.field = "keyconf";
            numericCond(v, c);
            q.filters.push_back(c);
            return true;
        }
        if (f == "compatible") {
            const int k = KeyNotationConverter::parse(v);
            if (k < 0) return false;
            c.field = "key_compatible";
            c.op = CmpOp::Eq;
            c.a = (double)k;
            q.filters.push_back(c);
            return true;
        }
        if (f == "camelot_compatible") {
            const int k = harmonic::CamelotConverter::toIndex(v);
            if (k < 0) return false;
            c.field = "key_camelot_compatible";
            c.op = CmpOp::Eq;
            c.a = (double)k;
            q.filters.push_back(c);
            return true;
        }
        if (f == "missing" || f == "analyzed" || f == "analysed") {
            c.field = f == "analysed" ? "analyzed" : f;
            c.op = CmpOp::Eq;
            c.a = num(v);
            q.filters.push_back(c);
            return true;
        }
        // Phase 23 — smart-library tokens
        if (f == "energy") {
            // energy:high|low|medium|veryhigh|verylow or 0.6-0.9
            static const std::pair<const char*, double> lvl[] = {
                { "verylow", 0.0 }, { "low", 0.2 }, { "medium", 0.4 },
                { "high", 0.6 }, { "veryhigh", 0.8 },
            };
            const std::string lv = lower(v);
            bool matched = false;
            for (size_t i = 0; i < 5; ++i)
                if (lv == lvl[i].first) {
                    c.field = "energy_score";
                    c.op = CmpOp::Between;
                    c.a = lvl[i].second;
                    c.b = i + 1 < 5 ? lvl[i + 1].second : 1.01;
                    matched = true;
                    break;
                }
            if (!matched) {
                c.field = "energy_score";
                if (!numericCond(v, c)) return false;
            }
            q.filters.push_back(c);
            return true;
        }
        if (f == "added" || f == "played") {
            // added:today|yesterday|7d|30d|year or epoch range
            const std::string lv = lower(v);
            const int64_t now = (int64_t)std::time(nullptr);
            const int64_t day = now - (now % 86400);
            int64_t from = -1, to = -1;
            if (lv == "today")          { from = day; to = now; }
            else if (lv == "yesterday") { from = day - 86400; to = day; }
            else if (lv == "7d" || lv == "week")   { from = now - 7 * 86400; to = now; }
            else if (lv == "30d" || lv == "month") { from = now - 30 * 86400; to = now; }
            if (from < 0) return false;
            c.field = f == "added" ? "added_after" : "played_after";
            c.op = CmpOp::Eq; c.a = (double)from;
            q.filters.push_back(c);
            FilterCond g;
            g.field = f == "added" ? "added_before" : "played_before";
            g.op = CmpOp::Eq; g.a = (double)to;
            q.filters.push_back(g);
            return true;
        }
        if (f == "unplayed") {
            c.field = "unplayed"; c.op = CmpOp::Eq;
            c.a = v == "0" || lower(v) == "false" ? 0.0 : 1.0;
            q.filters.push_back(c);
            return true;
        }
        if (f == "session") {
            c.field = "session"; c.op = CmpOp::Eq; c.a = num(v);
            q.filters.push_back(c);
            return true;
        }
        if (f == "autotag") {
            c.field = "autotag"; c.op = CmpOp::Contains;
            c.text = v; c.a = -1.0;
            q.filters.push_back(c);
            return true;
        }
        return false;
    }
};

} // namespace onda::library
