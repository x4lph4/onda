#include "MusicLibraryEngine.h"

#include <ctime>
#include <filesystem>

#include "../analysis/AnalysisService.h"

namespace onda::library {

MusicLibraryEngine::MusicLibraryEngine()
    : repo_(db_), importer_(repo_), scanner_(repo_, importer_),
      playlists_(repo_), history_(repo_), dup_(repo_), missing_(repo_),
      watcher_(scanner_, [this](const std::vector<std::string>& changed) {
          // file changed on disk → re-import on the worker
          post([this, changed] {
              for (const auto& p : changed) importer_.importFile(p);
          });
      }) {}

MusicLibraryEngine::~MusicLibraryEngine() {
    watcher_.stop();
    stopWorker();
    close();
}

bool MusicLibraryEngine::open(const std::string& dbPath) {
    dbPath_ = dbPath;
    if (!db_.open(dbPath)) return false;
    return true;
}

void MusicLibraryEngine::close() {
    if (db_.isOpen()) db_.close();
}

void MusicLibraryEngine::setMetadataProvider(
        TrackImporter::MetadataProvider p) {
    importer_.setMetadataProvider(std::move(p));
}

void MusicLibraryEngine::setAnalysisService(AnalysisService* svc) {
    analysis_ = svc;
    importer_.setAnalyzeHook(svc
        ? [svc](const std::string& path, int prio) {
              svc->analyzeTrack(path, prio);
          }
        : TrackImporter::AnalyzeHook{});
}

void MusicLibraryEngine::setArtworkProvider(
        std::function<std::string(const std::string&,
                                  const std::string&)> p) {
    artworkProvider_ = std::move(p);
}

// ---------------------------------------------------------------- worker
void MusicLibraryEngine::post(std::function<void()> fn) {
    {
        std::lock_guard lk(wq_);
        tasks_.push_back(std::move(fn));
    }
    wcv_.notify_one();
}

void MusicLibraryEngine::startWorker() {
    if (worker_.joinable()) return;
    exit_ = false;
    worker_ = std::thread([this] { workerMain(); });
}

void MusicLibraryEngine::stopWorker() {
    {
        std::lock_guard lk(wq_);
        exit_ = true;
    }
    wcv_.notify_all();
    if (worker_.joinable()) worker_.join();
    // drain leftovers inline so state is deterministic for callers
    runPending();
}

void MusicLibraryEngine::workerMain() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lk(wq_);
            wcv_.wait(lk, [this] {
                return exit_.load() || !tasks_.empty();
            });
            if (tasks_.empty()) return;
            fn = std::move(tasks_.front());
            tasks_.pop_front();
        }
        fn();
    }
}

int MusicLibraryEngine::runPending() {
    int n = 0;
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard lk(wq_);
            if (tasks_.empty()) break;
            fn = std::move(tasks_.front());
            tasks_.pop_front();
        }
        fn();
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------- async
void MusicLibraryEngine::importPathsAsync(
        const std::vector<std::string>& paths,
        std::function<void(const ImportResult&)> done) {
    post([this, paths, done = std::move(done)] {
        const ImportResult r = importer_.importFiles(paths);
        if (done) done(r);
    });
}

void MusicLibraryEngine::scanAsync(std::function<void(const ScanDiff&)> done) {
    post([this, done = std::move(done)] {
        const ScanDiff d = scanner_.scan();
        if (done) done(d);
    });
}

void MusicLibraryEngine::recordPlayAsync(int64_t trackId, int deck,
                                         int64_t startedAt,
                                         double playedSec,
                                         int64_t sessionId) {
    post([this, trackId, deck, startedAt, playedSec, sessionId] {
        history_.recordPlay(trackId, deck, startedAt, playedSec, sessionId);
    });
}

// ---------------------------------------------------------------- artwork
std::string MusicLibraryEngine::dbDir() const {
    if (dbPath_.empty()) return ".";
    return std::filesystem::path(dbPath_).parent_path().string();
}

std::string MusicLibraryEngine::artworkPath(int64_t trackId) {
    Track t;
    if (!repo_.getById(trackId, t) || t.artworkHash == 0 ||
        !artworkProvider_)
        return "";
    const std::string dir =
        (std::filesystem::path(dbDir()) / "artwork").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // cache name = track id + artwork marker (hash is a presence flag)
    const std::string base =
        dir + "/" + std::to_string(t.id) + "_" +
        std::to_string((long long)t.artworkHash);
    for (const char* ext : { "jpg", "png", "bin" }) {
        const std::string p = base + "." + ext;
        if (std::filesystem::exists(p, ec)) return p;
    }
    const std::string ext = artworkProvider_(t.path, base);
    if (ext.empty()) return "";
    return base + "." + ext;
}

// ---------------------------------------------------------------- settings
void MusicLibraryEngine::saveSetting(const std::string& k,
                                     const std::string& v) {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare(
        "INSERT INTO kv(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    s.bind(1, k); s.bind(2, v);
    s.step();
}

std::string MusicLibraryEngine::setting(const std::string& k,
                                        const std::string& def) {
    std::lock_guard lk(db_.mutex());
    auto s = db_.prepare("SELECT value FROM kv WHERE key=?");
    s.bind(1, k);
    return s.step() ? s.colText(0) : def;
}

} // namespace onda::library
