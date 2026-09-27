#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "../library/LibraryTypes.h"
#include "../library/TrackRepository.h"

namespace onda::smart {

/// Phase 23 §28-33 — named, reusable QuerySpecs for history- and
/// date-based library views. Each preset is a plain QuerySpec built
/// on the FilterEngine pseudo-fields — same engine, no parallel
/// query system.
class LibraryPresets {
public:
    /// Named preset ids the UI can enumerate.
    enum class Id : int {
        RecentlyAdded = 0, RecentlyPlayed, MostPlayed, Unplayed,
        NotPlayedRecently, PlayedThisSession, PlayedInSession,
        MissingFiles, Unanalyzed,
    };
    static const char* name(Id id) {
        switch (id) {
            case Id::RecentlyAdded:      return "Recently Added";
            case Id::RecentlyPlayed:     return "Recently Played";
            case Id::MostPlayed:         return "Most Played";
            case Id::Unplayed:           return "Unplayed";
            case Id::NotPlayedRecently:  return "Not Played Recently";
            case Id::PlayedThisSession:  return "Played This Session";
            case Id::PlayedInSession:    return "Played In Session";
            case Id::MissingFiles:       return "Missing Files";
            case Id::Unanalyzed:         return "Analysis Status";
            default:                     return "?";
        }
    }

    /// §33 — Recently Added, newest first.
    static library::QuerySpec recentlyAdded(int limit = 200) {
        library::QuerySpec q;
        q.sort = library::SortKey::DateAdded;
        q.ascending = false;
        q.limit = limit;
        return q;
    }

    /// Recently Played — by last_played, only actually-played rows.
    static library::QuerySpec recentlyPlayed(int limit = 200) {
        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = "last_played"; f.op = library::CmpOp::Gt; f.a = 0.0;
        q.filters.push_back(f);
        q.sort = library::SortKey::LastPlayed;
        q.ascending = false;
        q.limit = limit;
        return q;
    }

    /// §31 — Most Played (min play count, ordered desc).
    static library::QuerySpec mostPlayed(int minPlays = 1,
                                         int limit = 200) {
        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = "play_count"; f.op = library::CmpOp::Gte;
        f.a = (double)minPlays;
        q.filters.push_back(f);
        q.sort = library::SortKey::PlayCount;
        q.ascending = false;
        q.limit = limit;
        return q;
    }

    /// §32 — never played, keyed on play_count (not last_played).
    static library::QuerySpec unplayed(int limit = 200) {
        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = "unplayed"; f.op = library::CmpOp::Eq; f.a = 1.0;
        q.filters.push_back(f);
        q.sort = library::SortKey::Artist;
        q.limit = limit;
        return q;
    }

    /// Played at some point, but not within `days`.
    static library::QuerySpec notPlayedRecently(int days = 30,
                                                int limit = 200) {
        library::QuerySpec q = base();
        const int64_t cutoff = now() - (int64_t)days * 86400;
        library::FilterCond a, b;
        a.field = "last_played"; a.op = library::CmpOp::Gt; a.a = 0.0;
        b.field = "played_before"; b.op = library::CmpOp::Eq;
        b.a = (double)cutoff;
        q.filters.push_back(a);
        q.filters.push_back(b);
        q.sort = library::SortKey::LastPlayed;
        q.limit = limit;
        return q;
    }

    /// §38 — tracks whose file is flagged missing (relocate workflow
    /// lives in MissingFileManager; the view is a plain query).
    static library::QuerySpec missingFiles(int limit = 500) {
        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = "missing"; f.op = library::CmpOp::Eq; f.a = 1.0;
        q.filters.push_back(f);
        q.sort = library::SortKey::Artist;
        q.limit = limit;
        return q;
    }

    /// Tracks not yet analyzed — the analysis-status view.
    static library::QuerySpec unanalyzed(int limit = 500) {
        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = "analyzed"; f.op = library::CmpOp::Eq; f.a = 0.0;
        q.filters.push_back(f);
        q.sort = library::SortKey::DateAdded;
        q.ascending = false;
        q.limit = limit;
        return q;
    }

    /// §29 — tracks played inside one session id.
    static library::QuerySpec playedInSession(int64_t sessionId,
                                              int limit = 500) {
        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = "session"; f.op = library::CmpOp::Eq;
        f.a = (double)sessionId;
        q.filters.push_back(f);
        q.sort = library::SortKey::LastPlayed;
        q.ascending = false;
        q.limit = limit;
        return q;
    }

    // ---- §30 date presets -----------------------------------------
    enum class DateRange : int {
        Today = 0, Yesterday, Last7Days, Last30Days, ThisYear, Custom
    };

    /// `field` = "added_after"|"played_after" style anchor. Custom
    /// ranges pass [from,to] epochs explicitly.
    static library::QuerySpec dateFiltered(
        const std::string& field, DateRange r,
        int64_t customFrom = 0, int64_t customTo = 0,
        int limit = 200) {
        int64_t from = 0, to = 0;
        dayBounds(r, from, to);
        if (r == DateRange::Custom) { from = customFrom; to = customTo; }

        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = field;                       // added_* / played_*
        f.op = library::CmpOp::Eq;             // pseudo-field ignores op
        f.a = (double)from;
        q.filters.push_back(f);
        if (to > 0) {
            library::FilterCond g;
            g.field = std::string(field).replace(
                field.find("after"), 5, "before");
            g.op = library::CmpOp::Eq;
            g.a = (double)to;
            q.filters.push_back(g);
        }
        q.limit = limit;
        return q;
    }

    /// §31 — play-count buckets.
    static library::QuerySpec playCountRange(int lo, int hi,
                                             int limit = 200) {
        library::QuerySpec q = base();
        library::FilterCond f;
        f.field = "play_count";
        f.op = library::CmpOp::Between;
        f.a = (double)lo; f.b = (double)hi;
        q.filters.push_back(f);
        q.sort = library::SortKey::PlayCount;
        q.ascending = false;
        q.limit = limit;
        return q;
    }
    /// Named bucket: 0=0 plays, 1=1-5, 2=5-10, 3=10+.
    static library::QuerySpec playCountBucket(int bucket,
                                              int limit = 200) {
        switch (bucket) {
            case 0:  return playCountRange(0, 0, limit);
            case 1:  return playCountRange(1, 5, limit);
            case 2:  return playCountRange(5, 10, limit);
            default: return playCountRange(10, 1 << 30, limit);
        }
    }

    static int64_t now() { return (int64_t)std::time(nullptr); }

private:
    static library::QuerySpec base() {
        library::QuerySpec q;
        q.sort = library::SortKey::Artist;
        q.limit = 200;
        return q;
    }
    /// Epoch bounds for the named range [from, toExclusive].
    static void dayBounds(DateRange r, int64_t& from, int64_t& to) {
        const int64_t t = now();
        const int64_t day = t - (t % 86400);           // UTC midnight
        switch (r) {
            case DateRange::Today:
                from = day; to = t; break;
            case DateRange::Yesterday:
                from = day - 86400; to = day; break;
            case DateRange::Last7Days:
                from = t - 7 * 86400; to = t; break;
            case DateRange::Last30Days:
                from = t - 30 * 86400; to = t; break;
            case DateRange::ThisYear: {
                std::tm tm = *std::gmtime(&t);
                tm.tm_mon = 0; tm.tm_mday = 1;
                tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
#ifdef _WIN32
                from = (int64_t)_mkgmtime(&tm);
#else
                from = (int64_t)timegm(&tm);
#endif
                to = t; break;
            }
            default: from = 0; to = 0; break;
        }
    }
};

/// §29 — session-centric queries for future Recording/SetHistory/
/// Export integration. Thin, synchronous, read-only.
class SessionLibraryQuery {
public:
    explicit SessionLibraryQuery(library::TrackRepository& repo)
        : repo_(repo) {}

    library::TrackPage sessionTracks(int64_t sessionId, int offset = 0,
                                     int limit = 500) {
        auto q = LibraryPresets::playedInSession(sessionId, limit);
        q.offset = offset;
        return repo_.query(q);
    }

    /// Sessions as a lightweight list for pickers (id/name/time).
    struct SessionRow {
        int64_t id { 0 };
        std::string name;
        int64_t startedAt { 0 }, endedAt { 0 };
        int trackCount { 0 };
    };
    std::vector<SessionRow> sessions(int limit = 200) {
        std::lock_guard lk(repo_.db().mutex());
        auto s = repo_.db().prepare(
            "SELECT s.id,s.name,s.started_at,s.ended_at,"
            " (SELECT COUNT(DISTINCT h.track_id) FROM history h"
            "   WHERE h.session_id=s.id)"
            " FROM sessions s ORDER BY s.started_at DESC LIMIT ?");
        s.bind(1, (int64_t)limit);
        std::vector<SessionRow> out;
        while (s.step())
            out.push_back({ s.colInt(0), s.colText(1),
                            s.colInt(2), s.colInt(3), (int)s.colInt(4) });
        return out;
    }

private:
    library::TrackRepository& repo_;
};

} // namespace onda::smart
