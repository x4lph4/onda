#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/library/LibraryTypes.h"

namespace onda {

/// Tag/stream reader backed by libavformat (same decoder family as
/// FFmpegDecoder — one dependency, one licence story: LGPL/GPL
/// FFmpeg, covers WAV/AIFF/FLAC/MP3/M4A plus whatever else the build
/// demuxes). Read-only: we never write tags back to files.
///
/// Tag normalization: FFmpeg exposes a unified AVDictionary per
/// container (ID3v2, Vorbis comments, iTunes atoms, RIFF INFO), so
/// title/artist/album/album_artist(=TPE2|"aART")/genre/date/track/disc
/// arrive under common keys. Originals are never modified.
class FFmpegMetadataReader {
public:
    /// Fills TrackMeta. false = unreadable / not audio. Never throws.
    static bool read(const std::string& path, library::TrackMeta& out) noexcept;

    /// Extracts the first embedded picture if present.
    /// `bytes` = encoded image data (jpeg/png detection by magic),
    /// `ext` = "jpg"|"png"|"bin". false = no artwork.
    static bool artwork(const std::string& path, std::vector<uint8_t>& bytes,
                        std::string& ext) noexcept;

private:
    static std::string tag(const void* dict, const char* key);
    static std::string tagAny(const void* dict,
                              std::initializer_list<const char*> keys);
    static int tagInt(const void* dict, const char* key);
};

} // namespace onda
