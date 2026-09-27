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
    std::string label, composer;     // v5 — TPUB / TCOM
    int year { 0 }, trackNum { 0 }, discNum { 0 };
    double durationSec { 0.0 };
    int bitrate { 0 }, channels { 0 };
    double sampleRate { 0.0 };
    std::string format;              // wav/aiff/flac/mp3/m4a/…
    uint64_t artworkHash { 0 };
    int  playState { 0 };            // PlayState — decode-ability, §6
    uint32_t editedFields { 0 };     // EditedField mask, §8

    double bpm { 0.0 };
    float  bpmConfidence { 0.0f };
    int    keyIndex { -1 };          // -1 unknown, 0..23 standard
    float  keyConfidence { 0.0f };
    int    keyAmbiguous { 0 };       // analysis.key_ambiguous — §22
    int    analysisState { 0 };      // AnalysisState
    int    analysisVersion { 0 };
    int    needsReview  { 0 };       // analysis.needs_review — §36
    int    hasBeatgrid  { 0 };       // beatgrids row exists — §36
    int    hasWaveform  { 0 };       // waveforms row exists — §37
    float  energyScore  { -1.0f };   // analysis.energy 0..1 (-1 unknown)
                                     // §23 — descriptive, not truth

    int  rating { 0 };               // 0..5
    int  color { 0 };                // argb
    int  energy { 0 };               // 0..10 user tag
    std::string comments;
    std::vector<std::string> tags;
    int     playCount { 0 };
    int64_t lastPlayed { 0 };
    int64_t dateAdded { 0 };
    int64_t updatedAt { 0 };         // library-row modified (not file mtime)
};

enum class TrackStatus : int {
    Available = 0, Missing = 1, Relocated = 2, Invalid = 3
};

/// §6 — decode-ability, orthogonal to file presence (TrackStatus).
/// Playable          — decoder can open it
/// UnsupportedFormat — extension/container not supported by the decoder
/// MetadataOnly      — tags read, but audio stream won't decode
enum class PlayState : int {
    Playable = 0, UnsupportedFormat = 1, MetadataOnly = 2
};

/// §8 — bitmask of file-metadata fields the user edited; rescans
/// never overwrite marked fields.
namespace EditedField {
    inline constexpr uint32_t Title       = 1u << 0;
    inline constexpr uint32_t Artist      = 1u << 1;
    inline constexpr uint32_t Album       = 1u << 2;
    inline constexpr uint32_t AlbumArtist = 1u << 3;
    inline constexpr uint32_t Genre       = 1u << 4;
    inline constexpr uint32_t Year        = 1u << 5;
    inline constexpr uint32_t TrackNum    = 1u << 6;
    inline constexpr uint32_t DiscNum     = 1u << 7;
    inline constexpr uint32_t Label       = 1u << 8;
    inline constexpr uint32_t Composer    = 1u << 9;
    inline constexpr uint32_t Comments    = 1u << 10;
}

/// §35-37/§6 — presentation helpers (UI shows these verbatim).
inline const char* playStateText(int ps) {
    switch ((PlayState)ps) {
        case PlayState::UnsupportedFormat: return "UNSUPPORTED FORMAT";
        case PlayState::MetadataOnly:      return "METADATA ONLY";
        default:                           return "";
    }
}
inline const char* analysisStateText(int state) {
    // MusicAnalysisEngine AnalysisState: 0 none,1 queued,2 running,
    // 3 done,4 failed; version bump => "outdated" handled by callers.
    switch (state) {
        case 1: return "ANALYZING"; case 2: return "ANALYZING";
        case 3: return "ANALYZED";  case 4: return "FAILED";
        default: return "NOT_ANALYZED";
    }
}
inline const char* beatgridStatusText(const Track& t) {
    if (!t.hasBeatgrid) return "";
    return t.needsReview ? "BEATGRID NEEDS REVIEW" : "BEATGRID READY";
}
inline const char* waveformStatusText(const Track& t) {
    return t.hasWaveform ? "WAVEFORM READY" : "";
}

/// What MetadataReader fills — file-side data only.
struct TrackMeta {
    std::string title, artist, album, albumArtist, genre;
    std::string label, composer, comment;
    int year { 0 }, trackNum { 0 }, discNum { 0 };
    double durationSec { 0.0 };
    int bitrate { 0 }, channels { 0 };
    double sampleRate { 0.0 };
    std::string format;
    uint64_t artworkHash { 0 };      // non-zero if embedded picture seen
    int  playState { 0 };            // PlayState — §6
};

// ------------------------------------------------------------------ query

enum class SortKey : int {
    Title = 0, Artist, Album, Bpm, Key, Duration, Rating,
    DateAdded, PlayCount, LastPlayed, Year, Genre, Path,
    Energy                          // §23 — analysis.energy
};

enum class CmpOp : int {
    Eq = 0, Neq, Lt, Lte, Gt, Gte, Contains, Between, IsNull, In,
    StartsWith, NotIn,              // §21 — smart-playlist operators
    NotContains, EndsWith           // §23 — full operator set
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
    bool        negate { false };    // NOT (clause) — smart playlists §22
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

/// §30 — a playlist's stored view order. Manual = the explicit
/// playlist_items.position order (canonical storage); every other
/// mode is a VIEW over the same membership — sorting never rewrites
/// stored positions.
enum class PlaylistSort : int {
    Manual = 0, Title, Artist, Album, Bpm, Key, Rating, DateAdded
};
/// PlaylistSort → repository SortKey (Manual has no column — callers
/// keep the position path for it).
inline SortKey sortKeyFor(PlaylistSort m) {
    switch (m) {
        case PlaylistSort::Title:     return SortKey::Title;
        case PlaylistSort::Artist:    return SortKey::Artist;
        case PlaylistSort::Album:     return SortKey::Album;
        case PlaylistSort::Bpm:       return SortKey::Bpm;
        case PlaylistSort::Key:       return SortKey::Key;
        case PlaylistSort::Rating:    return SortKey::Rating;
        case PlaylistSort::DateAdded: return SortKey::DateAdded;
        default:                      return SortKey::Path;
    }
}

struct PlaylistInfo {
    int64_t id { 0 };
    std::string name;
    int64_t folderId { 0 };
    PlaylistKind kind { PlaylistKind::Static };
    std::string rulesJson;           // smart playlists only
    std::string description;         // v10
    int     color { 0 };             // argb, 0 = none (v10)
    int     sortMode { 0 };          // PlaylistSort (v10)
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

/// §47 — one session's full view: the session row plus its entries.
struct SessionHistory {
    int64_t id { 0 };
    std::string name;
    int64_t startedAt { 0 };
    int64_t endedAt { 0 };
    std::vector<HistoryEntry> entries;
};

/// §52-53 — the hand-off object between library and deck. The library
/// knows the track; the deck receives ONLY this reference and resolves
/// audio through the loader — library never controls playback and no
/// audio data crosses this boundary.
struct TrackReference {
    int64_t     trackId { -1 };     // DB identity (cues/loops/analysis)
    std::string path;               // filesystem source for the loader
    std::string title, artist;      // display fallback before metadata
    double      durationSec { 0.0 };
    double      bpm { 0.0 };
    int         keyIndex { -1 };
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
