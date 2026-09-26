#include "TrackImporter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace onda::library {

bool TrackImporter::isAudioFile(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return e == ".wav"  || e == ".wave" || e == ".aif" || e == ".aiff"
        || e == ".aifc" || e == ".flac" || e == ".mp3" || e == ".m4a"
        || e == ".aac"  || e == ".ogg"  || e == ".opus" || e == ".wma";
}

std::vector<std::string> TrackImporter::listAudioFiles(
        const std::string& dir, bool recursive) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    if (recursive) {
        for (auto it = std::filesystem::recursive_directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied,
                 ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec) && isAudioFile(it->path().string()))
                out.push_back(it->path().string());
        }
    } else {
        for (const auto& e :
             std::filesystem::directory_iterator(dir, ec)) {
            if (e.is_regular_file(ec) && isAudioFile(e.path().string()))
                out.push_back(e.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool TrackImporter::importFile(const std::string& path, Track* outTrack) {
    return importOne(path, outTrack) != Outcome::Failed;
}

TrackImporter::Outcome TrackImporter::importOne(const std::string& path,
                                              Track* outTrack) {
    std::error_code ec;
    const std::string abs =
        std::filesystem::absolute(path, ec).lexically_normal().string();
    if (ec || !std::filesystem::is_regular_file(abs, ec))
        return Outcome::Failed;

    Track t;
    t.path     = abs;
    t.filename = std::filesystem::path(abs).filename().string();
    uint64_t fp = 0, ch = 0; int64_t sz = 0, mt = 0;
    if (!FileFingerprint::compute(abs, fp, ch, sz, mt))
        return Outcome::Failed;
    t.fingerprint = fp; t.contentHash = ch;
    t.fileSize = sz;    t.mtime = mt;

    // existing row: skip work entirely when nothing changed
    Track old;
    const bool existed = repo_.getByPath(abs, old);
    if (existed && old.fingerprint == fp &&
        old.status == (int)TrackStatus::Available) {
        if (outTrack) *outTrack = old;
        return Outcome::Skipped;
    }
    if (existed) {
        t.id = old.id;
        t.rating = old.rating; t.color = old.color;
        t.energy = old.energy; t.comments = old.comments;
        t.playCount = old.playCount; t.lastPlayed = old.lastPlayed;
        t.dateAdded = old.dateAdded;
        t.analysisState = 0;   // content changed -> re-analyze
    }
    t.dateAdded = t.dateAdded ? t.dateAdded
                            : (int64_t)std::time(nullptr);
    t.status = (int)TrackStatus::Available;

    TrackMeta meta;
    if (meta_ && meta_(abs, meta)) {
        t.title = meta.title;       t.artist = meta.artist;
        t.album = meta.album;       t.albumArtist = meta.albumArtist;
        t.genre = meta.genre;       t.year = meta.year;
        t.trackNum = meta.trackNum; t.discNum = meta.discNum;
        t.durationSec = meta.durationSec; t.bitrate = meta.bitrate;
        t.channels = meta.channels; t.sampleRate = meta.sampleRate;
        t.format = meta.format;     t.artworkHash = meta.artworkHash;
    }
    if (t.title.empty())
        t.title = std::filesystem::path(abs).stem().string();

    if (repo_.upsert(t) <= 0) return Outcome::Failed;
    if (analyze_) analyze_(abs, 0);
    if (outTrack) *outTrack = t;
    return existed ? Outcome::Updated : Outcome::Added;
}

ImportResult TrackImporter::importFiles(const std::vector<std::string>& paths) {
    ImportResult r;
    for (const auto& p : paths) {
        switch (importOne(p)) {
            case Outcome::Added:   ++r.added;   break;
            case Outcome::Updated: ++r.updated; break;
            case Outcome::Skipped: ++r.skipped; break;
            case Outcome::Failed:
                ++r.failed;
                if (r.errors.size() < 64) r.errors.push_back(p);
                break;
        }
    }
    return r;
}

ImportResult TrackImporter::importFolder(const std::string& dir,
                                         bool recursive) {
    const auto files = listAudioFiles(dir, recursive);
    return importFiles(files);
}

} // namespace onda::library
