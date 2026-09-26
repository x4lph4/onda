#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/rec/RecordingExport.h"
#include "core/rec/RecordingRepository.h"
#include "core/rec/RecordingTypes.h"

namespace onda::rec {

/// Session management: list / rename / delete / export. Deleting a
/// session removes its DB rows only — audio files on disk are never
/// deleted automatically.
class RecordingManager {
public:
    explicit RecordingManager(RecordingRepository* repo) : repo_(repo) {}

    std::vector<SessionInfo> sessions(int limit = 100) {
        return repo_ ? repo_->sessions(limit) : std::vector<SessionInfo>{};
    }
    std::vector<TrackPlay> tracks(int64_t sessionId) {
        return repo_ ? repo_->tracks(sessionId) : std::vector<TrackPlay>{};
    }
    void rename(int64_t id, const std::string& name) {
        if (repo_) repo_->rename(id, name);
    }
    void remove(int64_t id) {
        if (repo_) repo_->remove(id);
    }

    /// Exports session metadata next to the WAV file:
    /// "json" → <file>.meta.json, "csv" → <file>.csv, "txt" → .txt.
    /// Returns the written path, empty on failure.
    std::string exportSession(const SessionInfo& s,
                              const std::string& format,
                              const std::string& appVersion) {
        const auto tr = tracks(s.id);
        std::string data, ext;
        if (format == "csv")      { data = export_::sessionCsv(s, tr); ext = ".csv"; }
        else if (format == "txt") { data = export_::sessionTxt(s, tr); ext = ".txt"; }
        else { data = export_::sessionJson(s, tr, appVersion);
               ext = ".meta.json"; }
        const std::string path = s.filePath + ext;
        return export_::writeFile(path, data) ? path : std::string{};
    }

private:
    RecordingRepository* repo_ = nullptr;
};

} // namespace onda::rec
