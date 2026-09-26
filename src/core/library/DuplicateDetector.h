#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "LibraryTypes.h"
#include "TrackRepository.h"

namespace onda::library {

/// Duplicate candidates, layered (never auto-deleted — UI decides):
///   DupContentHash   — identical sampled-content hash (strongest)
///   DupSizeDuration  — same size ±2 % AND duration ±0.5 s
///   DupMeta          — normalized title+artist equality
class DuplicateDetector {
public:
    explicit DuplicateDetector(TrackRepository& repo) : repo_(repo) {}

    /// All groups. Bounded — scans indexed columns only.
    std::vector<DuplicateGroup> findAll(int maxGroups = 1000) {
        std::vector<DuplicateGroup> out;
        findByHash(out, maxGroups);
        findBySizeDuration(out, maxGroups);
        return out;
    }

    /// Duplicates of one track only (for import warnings).
    std::vector<int64_t> of(const Track& t) {
        auto ids = repo_.idsByContentHash(t.contentHash);
        std::erase(ids, t.id);
        if (!ids.empty()) return ids;
        // fallback: same size ±2 % + duration ±0.5 s + same title
        std::string w =
            "t.id != " + std::to_string(t.id) +
            " AND t.file_size BETWEEN " + std::to_string((int64_t)(t.fileSize * 0.98)) +
            " AND " + std::to_string((int64_t)(t.fileSize * 1.02)) +
            " AND t.duration_sec BETWEEN " + std::to_string(t.durationSec - 0.5) +
            " AND " + std::to_string(t.durationSec + 0.5) +
            " AND t.title = ?";
        std::vector<int64_t> out;
        std::lock_guard lk(repo_.db().mutex());
        auto s = repo_.db().prepare(
            ("SELECT t.id FROM tracks t WHERE " + w).c_str());
        s.bind(1, t.title);
        while (s.step()) out.push_back(s.colInt(0));
        return out;
    }

private:
    void findByHash(std::vector<DuplicateGroup>& out, int max) {
        std::lock_guard lk(repo_.db().mutex());
        auto s = repo_.db().prepare(
            "SELECT content_hash, GROUP_CONCAT(id) FROM tracks "
            "WHERE content_hash > 0 GROUP BY content_hash "
            "HAVING COUNT(*) > 1 LIMIT ?");
        s.bind(1, (int64_t)max);
        while (s.step()) {
            DuplicateGroup g;
            g.reason = DupContentHash;
            g.detail = "identical content";
            const std::string csv = s.colText(1);
            g.trackIds.clear();
            size_t p = 0;
            while (p <= csv.size()) {
                const size_t c = csv.find(',', p);
                g.trackIds.push_back(std::stoll(csv.substr(
                    p, c == std::string::npos ? c : c - p)));
                if (c == std::string::npos) break;
                p = c + 1;
            }
            out.push_back(std::move(g));
        }
    }

    void findBySizeDuration(std::vector<DuplicateGroup>& out, int max) {
        std::lock_guard lk(repo_.db().mutex());
        auto s = repo_.db().prepare(
            "SELECT t1.id, t2.id FROM tracks t1 JOIN tracks t2 ON "
            "t2.id > t1.id AND ABS(t2.file_size - t1.file_size) "
            "<= MAX(1, t1.file_size / 50) AND ABS(t2.duration_sec - "
            "t1.duration_sec) <= 0.5 WHERE t1.content_hash != "
            "t2.content_hash AND t1.duration_sec > 0 LIMIT ?");
        s.bind(1, (int64_t)max);
        // merge pairwise hits into groups by first id
        std::unordered_map<int64_t, int> groupOf;
        while (s.step()) {
            const int64_t a = s.colInt(0), b = s.colInt(1);
            auto it = groupOf.find(a);
            if (it == groupOf.end()) {
                DuplicateGroup g;
                g.reason = DupSizeDuration;
                g.detail = "same size & duration";
                g.trackIds = { a, b };
                groupOf[a] = (int)out.size();
                groupOf[b] = (int)out.size();
                out.push_back(std::move(g));
            } else {
                out[(size_t)it->second].trackIds.push_back(b);
                groupOf[b] = it->second;
            }
        }
    }

    TrackRepository& repo_;
};

} // namespace onda::library
