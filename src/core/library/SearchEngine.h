#pragma once

#include <string>
#include <vector>

namespace onda::library {

/// Free-text search → FTS5 MATCH expression.
///
/// Tokens are split on whitespace and each becomes a prefix term:
///   "daft pun"  →  "daft" OR "pun"*  semantics via FTS5 default AND
/// We emit:  "daft"* AND "pun"*  — quoted, so FTS syntax chars in user
/// input can't break the query. Empty input → empty string (no filter).
/// The FTS table indexes title/artist/album/genre/filename/tags/comments.
class SearchEngine {
public:
    static std::string matchExpr(const std::string& text) {
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
            if (i >= text.size()) break;
            std::string tok;
            while (i < text.size() && !std::isspace((unsigned char)text[i]))
                tok += text[i++];
            // sanitize: keep word chars + wildcards impossible to inject
            std::string clean;
            for (char ch : tok)
                if (std::isalnum((unsigned char)ch) || ch == '_' || ch == '-'
                    || ch == '\'' || ch == '.')
                    clean += ch;
            if (clean.empty()) continue;
            if (!out.empty()) out += " AND ";
            out += '"' + clean + '"';
            out += '*';
        }
        return out;
    }
};

} // namespace onda::library
