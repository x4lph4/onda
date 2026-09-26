#pragma once

#include <functional>
#include <string>
#include <vector>

#include "FileFingerprint.h"
#include "LibraryTypes.h"
#include "TrackRepository.h"

namespace onda::library {

/// Import pipeline:
///   stat → fingerprint/contentHash → duplicate-aware upsert
///   → metadata read (injected provider) → track row → analysis enqueue
/// All steps run inside a transaction per file; a failed file never
/// leaves partial rows. Runs on the library worker — never blocks UI
/// or audio; sync entry points exist for tests/CLI.
///
/// metadataProvider(path, TrackMeta&) is injected so this header stays
/// codec-free — the app plugs FFmpegMetadataReader.
class TrackImporter {
public:
    using MetadataProvider =
        std::function<bool(const std::string&, TrackMeta&)>;
    using AnalyzeHook = std::function<void(const std::string& path, int prio)>;

    explicit TrackImporter(TrackRepository& repo) : repo_(repo) {}

    void setMetadataProvider(MetadataProvider p) { meta_ = std::move(p); }
    void setAnalyzeHook(AnalyzeHook h)           { analyze_ = std::move(h); }

    static bool isAudioFile(const std::string& path);
    static std::vector<std::string> listAudioFiles(
        const std::string& dir, bool recursive);

    /// One file. Track row created (or refreshed when fingerprint
    /// changed); returns true when the row exists afterwards.
    bool importFile(const std::string& path, Track* outTrack = nullptr);
    enum class Outcome { Failed, Added, Updated, Skipped };
    Outcome importOne(const std::string& path, Track* outTrack = nullptr);
    ImportResult importFiles(const std::vector<std::string>& paths);
    ImportResult importFolder(const std::string& dir, bool recursive);

private:
    TrackRepository& repo_;
    MetadataProvider meta_;
    AnalyzeHook      analyze_;
};

} // namespace onda::library
