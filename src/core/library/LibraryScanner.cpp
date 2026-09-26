#include "LibraryScanner.h"

#include <filesystem>

#include "FileFingerprint.h"

namespace onda::library {

void LibraryScanner::addRoot(const std::string& dir, bool recursive) {
    std::error_code ec;
    const std::string abs =
        std::filesystem::absolute(dir, ec).lexically_normal().string();
    if (ec) return;
    for (auto& [r, rec] : roots_)
        if (r == abs) { rec = recursive; return; }
    roots_.push_back({ abs, recursive });
}

bool LibraryScanner::removeRoot(const std::string& dir) {
    std::error_code ec;
    const std::string abs =
        std::filesystem::absolute(dir, ec).lexically_normal().string();
    for (auto it = roots_.begin(); it != roots_.end(); ++it)
        if (it->first == abs) { roots_.erase(it); return true; }
    return false;
}

std::vector<std::pair<std::string, bool>> LibraryScanner::roots() const {
    return roots_;
}

ScanDiff LibraryScanner::scan() {
    ScanDiff diff;
    // 1) every audio file under every root
    std::unordered_set<std::string> seen;
    std::vector<std::string> files;
    for (const auto& [root, rec] : roots_)
        for (auto& f : TrackImporter::listAudioFiles(root, rec)) {
            if (seen.insert(f).second) files.push_back(f);
        }

    // 2) all known tracks under the roots (path prefix match)
    std::unordered_map<std::string, Track> byPath;
    {
        QuerySpec q; q.limit = 1 << 30;
        for (auto& t : repo_.query(q).items) {
            bool under = false;
            for (const auto& [root, rec] : roots_)
                if (t.path.rfind(root, 0) == 0) { under = true; break; }
            if (under) byPath.emplace(t.path, std::move(t));
        }
    }

    // 3) fingerprint each file → classify
    std::unordered_map<uint64_t, std::string> hashToPath;
    std::unordered_set<std::string> touched;
    for (const auto& f : files) {
        uint64_t fp = 0, ch = 0; int64_t sz = 0, mt = 0;
        if (!FileFingerprint::compute(f, fp, ch, sz, mt)) continue;
        hashToPath[ch] = f;
        touched.insert(f);
        auto it = byPath.find(f);
        if (it == byPath.end()) continue;               // maybe new/moved
        if (it->second.status == (int)TrackStatus::Missing) {
            repo_.setStatus(it->second.id, TrackStatus::Available);
            if (it->second.fingerprint != fp) {
                diff.modified.push_back(it->second.id);
                importer_.importFile(f);
            }
            continue;                                    // file is back
        }
        if (it->second.fingerprint == fp)
            continue;                                    // unchanged
        diff.modified.push_back(it->second.id);
        importer_.importFile(f);                         // re-import + re-analyze
    }

    // 4) vanished rows → relink (same contentHash elsewhere) or MISSING
    for (const auto& [path, t] : byPath) {
        if (touched.count(path)) continue;
        auto h = hashToPath.find(t.contentHash);
        if (h != hashToPath.end()) {
            // same content at a new path → file was moved/renamed
            uint64_t fp2 = 0, ch2 = 0; int64_t sz2 = 0, mt2 = 0;
            FileFingerprint::compute(h->second, fp2, ch2, sz2, mt2);
            repo_.setPath(t.id, h->second,
                          std::filesystem::path(h->second)
                              .filename().string());
            repo_.updateFileIdentity(t.id, fp2, ch2, sz2, mt2);
            diff.relocated.push_back(t.id);
            touched.insert(h->second);
        } else {
            repo_.setStatus(t.id, TrackStatus::Missing);
            diff.missing.push_back(t.id);
        }
    }

    // 5) remaining unseen files are new
    for (const auto& f : files) {
        if (touched.count(f)) continue;
        if (importer_.importFile(f)) diff.added.push_back(f);
    }
    return diff;
}

std::vector<std::string> LibraryScanner::quickDiff() {
    std::vector<std::string> changed;
    std::unordered_set<std::string> seen;
    for (const auto& [root, rec] : roots_) {
        for (auto& f : TrackImporter::listAudioFiles(root, rec)) {
            if (!seen.insert(f).second) continue;
            Track t;
            int64_t sz = 0, mt = 0;
            if (!repo_.getByPath(f, t)) { changed.push_back(f); continue; }
            if (FileFingerprint::stat(f, sz, mt) &&
                (sz != t.fileSize || mt != t.mtime))
                changed.push_back(f);
        }
    }
    return changed;
}

} // namespace onda::library
