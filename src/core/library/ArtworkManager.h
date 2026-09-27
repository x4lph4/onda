#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace onda::library {

/// §41-43 — artwork surface: embedded → disk cache → thumbnails →
/// bounded in-memory LRU.
///
///   track → extract (provider) → <dbDir>/artwork/<id>_<hash>.<ext>
///        → thumb (provider)    → <id>_<hash>_t<px>.<ext>
///        → bytes()             → LRU hit, or read file once
///
/// Original files are decoded/re-encoded by INJECTED providers —
/// this header stays codec-free (the app plugs JUCE image I/O).
/// `bytes()` is the UI hot path: it never returns the multi-MB
/// original, only the thumbnail file's (small) bytes, and repeated
/// hits cost nothing but a hash lookup. All work runs off the UI
/// thread (providers + file reads happen on the caller — wire it
/// through the library worker or a paint-time prefetch).
class ArtworkManager {
public:
    /// (trackId) -> cached original-side artwork path, "" when none.
    /// Typically MusicLibraryEngine::artworkPath.
    using PathFn  = std::function<std::string(int64_t trackId)>;
    /// (srcPath, destBase, maxPx) -> writes a thumbnail at
    /// `destBase + "." + ext`; returns the ext or "" on failure.
    using ThumbFn = std::function<std::string(const std::string& srcPath,
                                              const std::string& destBase,
                                              int maxPx)>;

    explicit ArtworkManager(PathFn pathFn) : pathFn_(std::move(pathFn)) {}

    void setThumbMaker(ThumbFn f) { thumbFn_ = std::move(f); }
    void setCacheBudget(size_t bytes) { budget_ = bytes; }
    bool canThumbnail() const { return (bool)thumbFn_; }

    /// Original-side cached path (full-res; exists on disk).
    std::string path(int64_t trackId) {
        return pathFn_ ? pathFn_(trackId) : std::string{};
    }

    /// Thumbnail file path (≤ maxPx side), "" when unavailable.
    /// Lazy like the parent cache — created on first request.
    std::string thumbPath(int64_t trackId, int maxPx) {
        const std::string src = path(trackId);
        if (src.empty() || !thumbFn_) return "";
        const std::string base = src.substr(0, src.find_last_of('.'))
                               + "_t" + std::to_string(maxPx);
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* ext : { "jpg", "png", "webp" }) {
            const std::string p = base + "." + ext;
            if (fs::exists(p, ec)) return p;
        }
        const std::string ext = thumbFn_(src, base, maxPx);
        return ext.empty() ? "" : base + "." + ext;
    }

    /// Thumbnail bytes through the LRU — the paint loop's entry
    /// point. Returns nullptr when artwork/thumbnail is unavailable.
    std::shared_ptr<const std::vector<uint8_t>>
    bytes(int64_t trackId, int maxPx) {
        const std::string key =
            std::to_string(trackId) + ":" + std::to_string(maxPx);
        {
            std::lock_guard lk(m_);
            auto it = map_.find(key);
            if (it != map_.end()) {
                // touch — move to MRU front
                lru_.splice(lru_.begin(), lru_, it->second.it);
                return it->second.data;
            }
        }
        const std::string p = thumbPath(trackId, maxPx);
        if (p.empty()) return nullptr;
        auto data = readFile(p);
        if (!data || data->empty()) return nullptr;
        std::lock_guard lk(m_);
        // bound: evict LRU tail until under budget
        const size_t cost = data->size();
        while (bytes_ + cost > budget_ && !lru_.empty()) {
            auto& tail = lru_.back();
            bytes_ -= tail.second->size();
            map_.erase(tail.first);
            lru_.pop_back();
        }
        lru_.push_front({ key, data });
        auto it = map_.insert(
            { key, Entry{ data, lru_.begin() } }).first;
        bytes_ += cost;
        return it->second.data;
    }

    /// Drop cached bytes for one track (artwork changed on disk) —
    /// all px variants.
    void invalidate(int64_t trackId) {
        std::lock_guard lk(m_);
        const std::string prefix = std::to_string(trackId) + ":";
        for (auto it = lru_.begin(); it != lru_.end();)
            if (it->first.rfind(prefix, 0) == 0) {
                bytes_ -= it->second->size();
                map_.erase(it->first);
                it = lru_.erase(it);
            } else ++it;
    }

    void clear() {
        std::lock_guard lk(m_);
        lru_.clear(); map_.clear(); bytes_ = 0;
    }
    size_t cacheBytes() const {
        std::lock_guard lk(m_);
        return bytes_;
    }
    size_t cacheCount() const {
        std::lock_guard lk(m_);
        return map_.size();
    }

private:
    struct Entry {
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::list<std::pair<std::string,
            std::shared_ptr<const std::vector<uint8_t>>>>::iterator it;
    };

    static std::shared_ptr<std::vector<uint8_t>>
    readFile(const std::string& path) {
        auto out = std::make_shared<std::vector<uint8_t>>();
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return nullptr;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        out->resize((size_t)std::max<long>(n, 0));
        const size_t r = std::fread(out->data(), 1, out->size(), f);
        std::fclose(f);
        out->resize(r);
        return out;
    }

    PathFn  pathFn_;
    ThumbFn thumbFn_;
    size_t  budget_ { 32 * 1024 * 1024 };   // 32 MB of thumbnails
    mutable std::mutex m_;
    std::list<std::pair<std::string,
        std::shared_ptr<const std::vector<uint8_t>>>> lru_;   // MRU front
    std::unordered_map<std::string, Entry> map_;
    size_t bytes_ { 0 };
};

} // namespace onda::library
