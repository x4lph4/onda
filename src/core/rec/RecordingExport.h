#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "core/rec/RecordingTypes.h"

namespace onda::rec {

/// Session export — JSON metadata sidecar + CSV/TXT setlists.
/// Pure string builders; callers own file IO.
namespace export_ {

inline std::string esc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c >= 32) o += c;
    }
    return o;
}

/// Session metadata JSON — written alongside the WAV as
/// "<file>.meta.json" at finalize time (the audio file is untouched).
inline std::string sessionJson(const SessionInfo& s,
                               const std::vector<TrackPlay>& tracks,
                               const std::string& appVersion) {
    std::string j = "{\n";
    auto kv = [&](const char* k, const std::string& v, bool q = true,
                  bool comma = true) {
        j += "  \""; j += k; j += "\": ";
        if (q) { j += "\""; j += esc(v); j += "\""; } else j += v;
        if (comma) j += ",";
        j += "\n";
    };
    kv("app_version", appVersion);
    kv("session_id", std::to_string(s.id), false);
    kv("name", s.name);
    kv("start_ts", std::to_string(s.startTs), false);
    kv("end_ts", std::to_string(s.endTs), false);
    kv("duration_sec", std::to_string(s.durationSec), false);
    kv("source", std::to_string((int)s.source), false);
    kv("sample_rate", std::to_string(s.sampleRate), false);
    kv("bit_depth", std::to_string(bytesPerSample(s.bitDepth) * 8), false);
    kv("channels", std::to_string(s.channels), false);
    kv("file", s.filePath);
    kv("format", s.format);
    kv("file_bytes", std::to_string(s.fileBytes), false);
    kv("peak", std::to_string(s.peakLevel), false);
    kv("clips", std::to_string(s.clipCount), false);
    j += "  \"tracks\": [\n";
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        char line[1024];
        std::snprintf(line, sizeof line,
            "    {\"ts\":%lld,\"deck\":%d,\"title\":\"%s\","
            "\"artist\":\"%s\",\"bpm\":%.2f,\"key\":\"%s\","
            "\"played_sec\":%.1f}%s\n",
            (long long)t.ts, t.deck, esc(t.title).c_str(),
            esc(t.artist).c_str(), t.bpm, esc(t.key).c_str(),
            t.playedSec, i + 1 < tracks.size() ? "," : "");
        j += line;
    }
    j += "  ]\n}\n";
    return j;
}

inline std::string sessionCsv(const SessionInfo& s,
                              const std::vector<TrackPlay>& tracks) {
    std::string c =
        "# Onda session " + std::to_string(s.id) + " " + s.name + "\n";
    c += "ts,deck,title,artist,bpm,key,played_sec\n";
    for (const auto& t : tracks) {
        char line[1024];
        std::snprintf(line, sizeof line, "%lld,%d,\"%s\",\"%s\",%.2f,"
                      "%s,%.1f\n",
                      (long long)t.ts, t.deck, t.title.c_str(),
                      t.artist.c_str(), t.bpm, t.key.c_str(),
                      t.playedSec);
        c += line;
    }
    return c;
}

inline std::string sessionTxt(const SessionInfo& s,
                              const std::vector<TrackPlay>& tracks) {
    std::string t = "Onda session \"" + s.name + "\"  "
                  + std::to_string(s.durationSec / 60.0) + " min\n\n";
    int n = 1;
    for (const auto& tp : tracks) {
        char line[512];
        const int mm = (int)(tp.playedSec / 60), ss = (int)tp.playedSec % 60;
        std::snprintf(line, sizeof line,
                      "%2d. [%c] %s — %s  %.1f BPM %s  (%d:%02d)\n",
                      n++, 'A' + tp.deck, tp.artist.c_str(),
                      tp.title.c_str(), tp.bpm, tp.key.c_str(), mm, ss);
        t += line;
    }
    return t;
}

/// Writes a string to `path` — used by the engine at finalize and by
/// on-demand session export.
inline bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f)
                  == data.size();
    std::fclose(f);
    return ok;
}

} // namespace export_
} // namespace onda::rec
