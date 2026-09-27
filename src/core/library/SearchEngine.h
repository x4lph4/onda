#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "TextFold.h"

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
            // fold (lowercase + strip diacritics) to match the folded
            // index; sanitize keeps word chars so FTS syntax chars in
            // user input can't break the query
            const std::string folded = TextFold::fold(tok);
            std::string clean;
            for (char ch : folded)
                if (std::isalnum((unsigned char)ch)
                    || (uint8_t)ch >= 0x80  // non-ASCII (UTF-8 continuation)
                    || ch == '_' || ch == '-'
                    || ch == '\'' || ch == '.')
                    clean += ch;
            if (clean.empty()) continue;
            if (!out.empty()) out += " AND ";
            out += '"' + clean + '"';
            out += '*';
        }
        return out;
    }

    // ============================ fuzzy (§22) ============================

    /// Broad candidate net for the fuzzy fallback: OR-joined prefix
    /// terms (every token can hit, only one needs to). Used ONLY when
    /// the exact AND query returns nothing — never a ranking change.
    static std::string fuzzyExpr(const std::string& text) {
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
            if (i >= text.size()) break;
            std::string tok;
            while (i < text.size() && !std::isspace((unsigned char)text[i]))
                tok += text[i++];
            const std::string folded = TextFold::fold(tok);
            std::string clean;
            for (char ch : folded)
                if (std::isalnum((unsigned char)ch) || (uint8_t)ch >= 0x80)
                    clean += ch;
            if (clean.empty()) continue;
            if (!out.empty()) out += " OR ";
            out += '"' + clean + '"';
            out += '*';
        }
        // Typo-tolerance needs candidates a prefix of the WHOLE query
        // can't produce: "daftpunk"→"daft punk" — no stored token
        // starts with "daftpunk", but the first-4-squashed prefix
        // ("daft") does. Emit it as an extra OR candidate so the
        // distance re-rank sees the right rows. Only when the whole
        // squash is longer than the token prefixes already emitted.
        const std::string sq = squash(text);
        if (sq.size() > 4) {
            const std::string pre = sq.substr(0, 4);
            if (out.find('"' + pre + '"') == std::string::npos) {
                if (!out.empty()) out += " OR ";
                out += '"' + pre + '"';
                out += '*';
            }
        }
        return out;
    }

    /// Fold + strip non-alphanumerics: "Daft Punk"→"daftpunk",
    /// "The Prodigy"→"theprodigy". Typo tolerance works over this
    /// squashed space so missing/extra whitespace is free.
    static std::string squash(const std::string& s) {
        std::string out;
        for (char ch : TextFold::fold(s))
            if (std::isalnum((unsigned char)ch) || (uint8_t)ch >= 0x80)
                out += ch;
        return out;
    }

    /// Banded Levenshtein — early-out when the distance already
    /// exceeds `maxDist` (score is an integer distance or maxDist+1).
    static int editDistance(const std::string& a, const std::string& b,
                            int maxDist) {
        const int n = (int)a.size(), m = (int)b.size();
        if (std::abs(n - m) > maxDist) return maxDist + 1;
        std::vector<int> row((size_t)m + 1);
        for (int j = 0; j <= m; ++j) row[(size_t)j] = j;
        for (int i = 1; i <= n; ++i) {
            int prev = row[0];
            row[0] = i;
            int rowMin = row[0];
            for (int j = 1; j <= m; ++j) {
                const int t = row[(size_t)j];
                row[(size_t)j] = std::min({
                    row[(size_t)j] + 1,
                    row[(size_t)j - 1] + 1,
                    prev + (a[(size_t)i - 1] == b[(size_t)j - 1] ? 0 : 1) });
                prev = t;
                if (row[(size_t)j] < rowMin) rowMin = row[(size_t)j];
            }
            if (rowMin > maxDist) return maxDist + 1;   // band bail
        }
        return row[(size_t)m];
    }

    /// Best distance of `query` against `hay` — the query may match a
    /// window of the haystack (substring windowed compare), so
    /// "daftpunk" scores 0 against "daftpunkone" too, just ranked lower
    /// than an exact hit. Returns maxDist+1 on failure.
    static int fuzzyDistance(const std::string& query,
                             const std::string& hay, int maxDist) {
        const std::string q = squash(query), h = squash(hay);
        if (q.empty() || h.empty()) return maxDist + 1;
        if (h.find(q) != std::string::npos) return 0;   // substring hit
        // windowed compare: q against each h-window of ±maxDist length
        int best = maxDist + 1;
        const int qlen = (int)q.size();
        for (int start = 0; start <= std::max(0, (int)h.size() - qlen);
             ++start) {
            for (int len = std::max(1, qlen - maxDist);
                 len <= std::min((int)h.size() - start, qlen + maxDist);
                 ++len) {
                const int d = editDistance(
                    q, h.substr((size_t)start, (size_t)len), maxDist);
                if (d < best) best = d;
                if (best == 0) return 0;
            }
        }
        return best;
    }

    /// Max tolerated distance for a query — scales with length, floor
    /// 1 so 4-char typos still match ("dapt"→"daft").
    static int fuzzyThreshold(const std::string& query) {
        return std::max(1, (int)squash(query).size() / 4);
    }
};

} // namespace onda::library
