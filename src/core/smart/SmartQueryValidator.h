#pragma once

#include <string>
#include <vector>

#include "../library/FilterEngine.h"
#include "../library/SmartPlaylistEngine.h"

namespace onda::smart {

/// Phase 23 §8 — validates a SmartRules tree before saving.
/// Every error is human-readable and points at the offending rule.
class SmartQueryValidator {
public:
    struct Issue {
        std::string field;
        std::string message;    // e.g. "unknown field 'tempo'"
    };

    /// Numeric fields where BETWEEN/IN make sense; text-ish fields
    /// accept contains/starts/ends/eq/neq/isnull.
    static bool numericField(const std::string& f) {
        static const char* n[] = {
            "bpm", "rating", "color", "energy", "energy_score",
            "energy_analysis", "duration", "year", "track_num",
            "date_added", "play_count", "last_played", "file_size",
            "key_index", "key", "keyconf", "key_confidence",
            "lufs", "rms", "bpm_confidence",
        };
        for (auto* k : n) if (f == k) return true;
        return false;
    }

    static std::vector<Issue> validate(const library::SmartRules& sr) {
        std::vector<Issue> out;
        if (sr.rules.empty() && sr.groups.empty())
            out.push_back({ "", "query is empty — matches everything" });

        auto check = [&](const library::SmartRule& r) {
            if (r.field.empty()) {
                out.push_back({ "", "rule has no field" });
                return;
            }
            if (!library::FilterEngine::isKnownField(r.field)) {
                out.push_back({ r.field,
                                "unknown field '" + r.field + "'" });
                return;
            }
            const library::CmpOp op = library::smartOpFor(r.op);
            const bool num = numericField(r.field);
            switch (op) {
                case library::CmpOp::Between:
                    if (!num)
                        out.push_back({ r.field,
                            "BETWEEN needs a numeric field" });
                    else if (r.a > r.b)
                        out.push_back({ r.field,
                            "BETWEEN range is inverted (a > b)" });
                    break;
                case library::CmpOp::In:
                case library::CmpOp::NotIn:
                    if (r.ints.empty() && r.text.empty())
                        out.push_back({ r.field,
                            "IN list is empty" });
                    break;
                case library::CmpOp::Contains:
                case library::CmpOp::NotContains:
                case library::CmpOp::StartsWith:
                case library::CmpOp::EndsWith:
                    if (num)
                        out.push_back({ r.field,
                            "text operator on a numeric field" });
                    else if (r.text.empty())
                        out.push_back({ r.field,
                            "text operator with an empty value" });
                    break;
                case library::CmpOp::IsNull:
                    break;    // valid on any field
                default:
                    if (!num && r.text.empty() && r.a == 0.0
                        && !num)
                        out.push_back({ r.field,
                            "comparison needs a value" });
                    break;
            }
        };
        for (const auto& r : sr.rules) check(r);
        for (const auto& g : sr.groups) {
            if (g.rules.empty())
                out.push_back({ "", "group is empty" });
            for (const auto& r : g.rules) check(r);
        }
        return out;
    }

    /// Convenience: valid = no issues other than the empty-query
    /// warning (an empty query is legal but noteworthy).
    static bool isValid(const library::SmartRules& sr,
                        bool* emptyOnly = nullptr) {
        const auto v = validate(sr);
        if (emptyOnly)
            *emptyOnly = v.size() == 1 && v[0].field.empty();
        return v.empty();
    }
};

/// Phase 23 §47 — programmatic query builder. Produces the exact
/// SmartRules the engine serializes — the visual model and the
/// stored model are the same object, so preview == saved result.
class QueryBuilder {
public:
    QueryBuilder() = default;
    explicit QueryBuilder(library::SmartRules sr) : sr_(std::move(sr)) {}

    // ---- top level ----
    void setMatchAny(bool any) { sr_.any = any; }   // OR vs AND
    bool matchAny() const { return sr_.any; }

    int  addRule(const library::SmartRule& r) {
        sr_.rules.push_back(r);
        return (int)sr_.rules.size() - 1;
    }
    int  addRule(const std::string& field, const std::string& op,
                 const std::string& text = "",
                 double a = 0.0, double b = 0.0,
                 std::vector<int> ints = {}) {
        library::SmartRule r;
        r.field = field; r.op = op; r.text = text;
        r.a = a; r.b = b; r.ints = std::move(ints);
        return addRule(r);
    }
    bool removeRule(int idx) {
        if (idx < 0 || idx >= (int)sr_.rules.size()) return false;
        sr_.rules.erase(sr_.rules.begin() + idx);
        return true;
    }
    bool negateRule(int idx, bool on = true) {
        if (idx < 0 || idx >= (int)sr_.rules.size()) return false;
        sr_.rules[(size_t)idx].negate = on;
        return true;
    }

    // ---- groups (§3: AND/OR nesting) ----
    int  addGroup(bool any = false) {
        sr_.groups.push_back({});
        sr_.groups.back().any = any;
        return (int)sr_.groups.size() - 1;
    }
    int  addRuleToGroup(int group, const library::SmartRule& r) {
        if (group < 0 || group >= (int)sr_.groups.size()) return -1;
        auto& g = sr_.groups[(size_t)group];
        g.rules.push_back(r);
        return (int)g.rules.size() - 1;
    }
    bool removeGroup(int group) {
        if (group < 0 || group >= (int)sr_.groups.size()) return false;
        sr_.groups.erase(sr_.groups.begin() + group);
        return true;
    }
    void clear() { sr_ = {}; }

    const library::SmartRules& rules() const { return sr_; }
    std::string toJson() const {
        return library::SmartPlaylistEngine::toJson(sr_);
    }
    std::vector<SmartQueryValidator::Issue> validate() const {
        return SmartQueryValidator::validate(sr_);
    }

private:
    library::SmartRules sr_;
};

} // namespace onda::smart
