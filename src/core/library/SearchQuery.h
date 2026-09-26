#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

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
        if (f == "missing" || f == "analyzed" || f == "analysed") {
            c.field = f == "analysed" ? "analyzed" : f;
            c.op = CmpOp::Eq;
            c.a = num(v);
            q.filters.push_back(c);
            return true;
        }
        return false;
    }
};

} // namespace onda::library
