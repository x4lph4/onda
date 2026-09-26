#pragma once

#include <string>
#include <vector>

#include "FilterEngine.h"
#include "LibraryTypes.h"
#include "MiniJson.h"
#include "SearchEngine.h"
#include "TrackRepository.h"

namespace onda::library {

/// §20-22 — smart playlists store a QUERY, never a materialised
/// result. Rules live as JSON on playlists.rules and evaluate live
/// through the whitelisted FilterEngine → TrackRepository::queryWhere.
///
/// JSON shape (v1):
///   { "match": "all"|"any",
///     "rules": [ {field,op,text?,a?,b?,ints?,not?}, ... ],
///     "groups":[ { "match":..., "rules":[...] } ] }   // nested AND/OR
///   op: eq|neq|lt|lte|gt|gte|contains|starts_with|between|in|not_in|
///       isnull ; "not": true negates the leaf. Fields are the
///       FilterEngine whitelist (bpm/key_index/genre/rating/...).
/// op string ↔ CmpOp (shared by SmartRule::toCond and the codec).
inline CmpOp smartOpFor(const std::string& op) {
    if (op == "neq")         return CmpOp::Neq;
    if (op == "lt")          return CmpOp::Lt;
    if (op == "lte")         return CmpOp::Lte;
    if (op == "gt")          return CmpOp::Gt;
    if (op == "gte")         return CmpOp::Gte;
    if (op == "contains")    return CmpOp::Contains;
    if (op == "starts_with") return CmpOp::StartsWith;
    if (op == "between")     return CmpOp::Between;
    if (op == "in")          return CmpOp::In;
    if (op == "not_in")      return CmpOp::NotIn;
    if (op == "isnull")      return CmpOp::IsNull;
    return CmpOp::Eq;
}

struct SmartRule {
    std::string field;
    std::string op { "eq" };
    std::string text;
    double a { 0.0 }, b { 0.0 };
    std::vector<int> ints;
    bool negate { false };

    FilterCond toCond() const {
        FilterCond c;
        c.field = field;
        c.op    = smartOpFor(op);
        c.text  = text;
        c.a = a; c.b = b; c.ints = ints;
        c.negate = negate;
        return c;
    }
};

struct SmartGroup {
    bool any { false };               // false=AND, true=OR
    std::vector<SmartRule> rules;
};

struct SmartRules {
    bool any { false };               // top-level match logic
    std::vector<SmartRule> rules;
    std::vector<SmartGroup> groups;   // nested groups, joined by `any`
};

class SmartPlaylistEngine {
public:
    // ---- op strings ↔ CmpOp
    static CmpOp opFor(const std::string& op) { return smartOpFor(op); }
    static const char* opName(CmpOp op) {
        switch (op) {
            case CmpOp::Neq:        return "neq";
            case CmpOp::Lt:         return "lt";
            case CmpOp::Lte:        return "lte";
            case CmpOp::Gt:         return "gt";
            case CmpOp::Gte:        return "gte";
            case CmpOp::Contains:   return "contains";
            case CmpOp::StartsWith: return "starts_with";
            case CmpOp::Between:    return "between";
            case CmpOp::In:         return "in";
            case CmpOp::NotIn:      return "not_in";
            case CmpOp::IsNull:     return "isnull";
            default:                return "eq";
        }
    }

    // ---- codec
    static std::string toJson(const SmartRules& sr) {
        MiniJson root = MiniJson::object();
        root.set("match", sr.any ? "any" : "all");
        auto rulesToJson = [](const std::vector<SmartRule>& rs) {
            MiniJson arr = MiniJson::array();
            for (const auto& r : rs) {
                MiniJson o = MiniJson::object();
                o.set("field", r.field);
                o.set("op", r.op.empty() ? "eq" : r.op);
                if (!r.text.empty()) o.set("text", r.text);
                if (r.a != 0.0 || r.b != 0.0) { o.set("a", r.a); o.set("b", r.b); }
                if (!r.ints.empty()) {
                    MiniJson ints = MiniJson::array();
                    for (int v : r.ints) ints.push(v);
                    o.set("ints", std::move(ints));
                }
                if (r.negate) o.set("not", true);
                arr.push(std::move(o));
            }
            return arr;
        };
        root.set("rules", rulesToJson(sr.rules));
        if (!sr.groups.empty()) {
            MiniJson gs = MiniJson::array();
            for (const auto& g : sr.groups) {
                MiniJson o = MiniJson::object();
                o.set("match", g.any ? "any" : "all");
                o.set("rules", rulesToJson(g.rules));
                gs.push(std::move(o));
            }
            root.set("groups", std::move(gs));
        }
        return root.dump();
    }

    /// Tolerant parse — unknown members ignored; malformed input →
    /// empty rules (match-all) and `ok` false when provided.
    static SmartRules fromJson(const std::string& json, bool* ok = nullptr) {
        SmartRules sr;
        bool parsed = false;
        const MiniJson root = MiniJson::parse(json, parsed);
        if (ok) *ok = parsed;
        if (!parsed) return sr;                    // empty → match all
        sr.any = root.at("match").str() == "any";
        auto readRules = [](const MiniJson& arr,
                            std::vector<SmartRule>& out) {
            for (const auto& e : arr.arr()) {
                if (!e.isObj()) continue;
                SmartRule r;
                r.field  = e.at("field").str();
                r.op     = e.at("op").str();
                r.text   = e.at("text").str();
                r.a      = e.at("a").num();
                r.b      = e.at("b").num();
                r.negate = e.at("not").boolean();
                for (const auto& iv : e.at("ints").arr())
                    r.ints.push_back((int)iv.num());
                if (!r.field.empty()) out.push_back(std::move(r));
            }
        };
        readRules(root.at("rules"), sr.rules);
        for (const auto& g : root.at("groups").arr()) {
            SmartGroup sg;
            sg.any = g.at("match").str() == "any";
            readRules(g.at("rules"), sg.rules);
            if (!sg.rules.empty()) sr.groups.push_back(std::move(sg));
        }
        return sr;
    }

    // ---- evaluation
    /// Rules → WHERE fragment (whitelist SQL, bound params).
    static std::string where(const SmartRules& sr,
                             std::vector<std::string>& binds) {
        std::vector<FilterCond> conds;
        for (const auto& r : sr.rules) conds.push_back(r.toCond());
        std::string w = FilterEngine::whereJoin(conds, binds, sr.any);
        for (const auto& g : sr.groups) {
            std::vector<FilterCond> gc;
            for (const auto& r : g.rules) gc.push_back(r.toCond());
            const std::string gw =
                FilterEngine::whereJoin(gc, binds, g.any);
            if (gw.empty()) continue;
            if (!w.empty()) w += sr.any ? " OR " : " AND ";
            w += "(" + gw + ")";
        }
        return w;
    }

    /// Live evaluation — QuerySpec carries sort/page/search; a
    /// free-text `q.search` ANDs an FTS clause on top of the rules.
    static TrackPage evaluate(TrackRepository& repo,
                              const std::string& rulesJson,
                              const QuerySpec& q) {
        std::vector<std::string> binds;
        std::string w = where(fromJson(rulesJson), binds);
        const std::string m = SearchEngine::matchExpr(q.search);
        if (!m.empty()) {
            if (!w.empty()) w += " AND ";
            w += "t.id IN (SELECT rowid FROM tracks_fts"
                 " WHERE tracks_fts MATCH ?)";
            binds.push_back(m);
        }
        return repo.queryWhere(w, binds, q);
    }
};

} // namespace onda::library
