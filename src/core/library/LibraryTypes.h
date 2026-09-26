#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onda::library {

/// Track row — the unit every library subsystem deals in. Metadata is
/// split three ways, mirroring the schema:
///   embedded   — read from the file at import (title/artist/…, never
///                rewritten to disk by us)
///   analysis   — populated by MusicAnalysisEngine (bpm/key/…)
///   user       — rating/color/tags/comments/play stats (DB only)
struct Track {
    int64_t id { 0 };
    std::string path;
    std::string filename;
    uint64_t    fingerprint { 0 };   // size+mtime+content sample
    uint64_t    contentHash { 0 };   // content-only (no mtime) — duplicates
    int64_t     fileSize  { 0 };
    int64_t     mtime     { 0 };
    int         status    { 0 };     // TrackStatus

    std::string title, artist, album, albumArtist, genre;
    int year { 0 }, trackNum { 0 }, discNum { 0 };
    double durationSec { 0.0 };
    int bitrate { 0 }, channels { 0 };
    double sampleRate { 0.0 };
    std::string format;              // wav/aiff/flac/mp3/m4a/…
    uint64_t artworkHash { 0 };

    double bpm { 0.0 };
    float  bpmConfidence { 0.0f };
    int    keyIndex { -1 };          // -1 unknown, 0..23 standard
    float  keyConfidence { 0.0f };
    int    analysisState { 0 };      // AnalysisState
    int    analysisVersion { 0 };

    int  rating { 0 };               // 0..5
    int  color { 0 };                // argb
    int  energy { 0 };               // 0..10 user tag
    std::string comments;
    std::vector<std::string> tags;
    int     playCount { 0 };
    int64_t lastPlayed { 0 };
    int64_t dateAdded { 0 };
};

enum class TrackStatus : int {
    Available = 0, Missing = 1, Relocated = 2, Invalid = 3
};

/// What MetadataReader fills — file-side data only.
struct TrackMeta {
    std::string title, artist, album, albumArtist, genre;
    int year { 0 }, trackNum { 0 }, discNum { 0 };
    double durationSec { 0.0 };
    int bitrate { 0 }, channels { 0 };
    double sampleRate { 0.0 };
    std::string format;
    uint64_t artworkHash { 0 };      // non-zero if embedded picture seen
};

// ------------------------------------------------------------------ query

enum class SortKey : int {
    Title = 0, Artist, Album, Bpm, Key, Duration, Rating,
    DateAdded, PlayCount, LastPlayed, Year, Genre, Path
};

enum class CmpOp : int {
    Eq = 0, Neq, Lt, Lte, Gt, Gte, Contains, Between, IsNull, In
};

/// One filter condition. Field names are fixed strings resolved by
/// FilterEngine to real columns (title/artist/album/genre/bpm/key/
/// rating/color/duration/year/date_added/play_count/last_played/
/// analyzed/missing/tags/comments/energy/filename/format).
struct FilterCond {
    std::string field;
    CmpOp       op { CmpOp::Eq };
    std::string text;                // text operands / single value as string
    double      a { 0.0 }, b { 0.0 };// numeric operands / Between range
    std::vector<int> ints;           // In-list (e.g. key indexes)
};

/// Composable query: AND of conditions + sort + page.
struct QuerySpec {
    std::string             search;          // free text → SearchEngine
    std::vector<FilterCond> filters;         // ANDed
    SortKey sort { SortKey::Artist };
    bool    ascending { true };
    int     offset { 0 };
    int     limit  { 200 };
};

struct TrackPage {
    std::vector<Track> items;
    int total { 0 };                 // rows matching (pre-pagination)
};

// ------------------------------------------------------------------ misc

struct ImportResult {
    int added { 0 }, skipped { 0 }, failed { 0 }, updated { 0 };
    std::vector<std::string> errors; // path: reason, bounded
};

enum class PlaylistKind : int { Static = 0, Smart = 1 };

struct PlaylistInfo {
    int64_t id { 0 };
    std::string name;
    int64_t folderId { 0 };
    PlaylistKind kind { PlaylistKind::Static };
    std::string rulesJson;           // smart playlists only
    int64_t createdAt { 0 }, updatedAt { 0 };
    int itemCount { 0 };
};

struct FolderInfo {
    int64_t id { 0 }, parentId { 0 };
    std::string name;
};

struct HistoryEntry {
    int64_t id { 0 }, trackId { 0 }, sessionId { 0 };
    int deck { 0 };
    int64_t startedAt { 0 };
    double playedSec { 0.0 };
    int counted { 0 };
};

/// Play-count rule (documented default): a playback counts when the
/// track played for at least `minSeconds` OR `minFraction` of its
/// duration — whichever comes first. Loading a track never counts.
struct PlayCountRule {
    double minSeconds  { 60.0 };
    double minFraction { 0.5 };
    bool qualifies(double playedSec, double durationSec) const noexcept {
        const double s = minSeconds, f = minFraction * durationSec;
        const double need = s < f ? s : f;
        return playedSec >= need && playedSec > 0.0;
    }
};

/// Change report from a scan pass.
struct ScanDiff {
    std::vector<std::string> added;      // paths new to the DB
    std::vector<int64_t>     missing;    // track ids whose file vanished
    std::vector<int64_t>     modified;   // fingerprint changed
    std::vector<int64_t>     relocated;  // moved file relinked in place
};

/// A duplicate group: tracks sharing a signal strong enough to be
/// "probably the same recording". Never auto-deleted — UI decides.
struct DuplicateGroup {
    int reason;                    // DuplicateReason
    std::vector<int64_t> trackIds;
    std::string detail;
};
enum DuplicateReason {
    DupContentHash = 0, DupSizeDuration, DupMeta
};

} // namespace onda::library
