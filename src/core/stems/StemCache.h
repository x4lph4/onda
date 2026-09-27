#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../rec/RecordingEncoder.h"   // WavEncoder (float32 writer)
#include "StemSeparationModel.h"       // StemCancel
#include "StemTypes.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// §27-31 — file cache for separated audio.
///
/// Layout:  <cacheDir>/<key>/<stem>.wav   +   <cacheDir>/<key>/meta
/// `key` = FNV-1a(contentHash | modelId | modelVersion | quality |
///                sampleRate | channels) — change ANY parameter and the
/// key changes, so incompatible results can never be read back (§28).
///
/// Writes are atomic: into `<key>.tmp/` then rename (§42 partial
/// artifacts never appear as a valid set). Reads validate the meta
/// sidecar AND each WAV header (rate/channels/frames) before use.
///
/// `read()` loads whole stems for deck use; `readRange()` proves the
/// streaming seam (§31) — frames can be pulled in chunks without
/// loading the whole set.
/// ------------------------------------------------------------------
class StemCache {
public:
    explicit StemCache(std::string dir) : dir_(std::move(dir)) {}

    const std::string& dir() const { return dir_; }

    // ---- key derivation ------------------------------------------
    static uint64_t fnv1a(uint64_t h, const void* p, size_t n) {
        const auto* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
        return h;
    }
    static uint64_t fnvStr(uint64_t h, const std::string& s) {
        return fnv1a(h, s.data(), s.size());
    }
    /// Cache identity (§27): track content + full processing config.
    static std::string keyFor(const StemSetMeta& m) {
        uint64_t h = 14695981039346656037ULL;
        h = fnv1a(h, &m.contentHash, sizeof(m.contentHash));
        h = fnvStr(h, m.modelId);
        h = fnvStr(h, m.modelVersion);
        h = fnv1a(h, &m.quality,   sizeof(m.quality));
        h = fnv1a(h, &m.rate,      sizeof(m.rate));
        h = fnv1a(h, &m.channels,  sizeof(m.channels));
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      (unsigned long long)h);
        return buf;
    }

    std::string setDir(const std::string& key) const {
        return (std::filesystem::path(dir_) / key).string();
    }
    static std::string stemFile(StemType t) {
        std::string n = stemTypeName(t);
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        return n + ".wav";
    }

    bool exists(const std::string& key) const {
        std::error_code ec;
        return std::filesystem::exists(
            std::filesystem::path(setDir(key)) / "meta", ec);
    }

    // ---- write ----------------------------------------------------
    /// Write a complete set atomically. Returns the cache key on
    /// success, "" on failure (disk full etc. — §42 temp dir cleaned).
    std::string write(const StemSet& set, const StemSetMeta& meta,
                      StemCancel* cancel = nullptr) {
        const std::string key = keyFor(meta);
        const auto tmp  = std::filesystem::path(dir_) / (key + ".tmp");
        const auto dst  = std::filesystem::path(setDir(key));
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::create_directories(tmp, ec);
        if (ec) return "";

        for (int i = 0; i < kStemMax; ++i) {
            if (!(set.mask & (1u << i))) continue;
            if (cancel && cancel->load()) {
                std::filesystem::remove_all(tmp, ec);
                return "";
            }
            const auto p = tmp / stemFile((StemType)i);
            rec::WavEncoder w;
            if (!w.open(p.string(), set.rate, set.channels,
                        rec::BitDepth::F32)) {
                std::filesystem::remove_all(tmp, ec);
                return "";
            }
            if (!w.writeFrames(set.samples[(size_t)i].data(),
                               (size_t)set.numFrames)) {
                w.finalize();
                std::filesystem::remove_all(tmp, ec);
                return "";
            }
            w.finalize();
        }
        // meta sidecar — plain "k=v" lines (MiniJson-compatible shape
        // not needed; the key IS the identity, meta is provenance).
        {
            const auto mp = tmp / "meta";
            FILE* f = std::fopen(mp.string().c_str(), "wb");
            if (!f) { std::filesystem::remove_all(tmp, ec); return ""; }
            std::fprintf(f,
                "model=%s\nversion=%s\nquality=%d\nrate=%lld\n"
                "channels=%d\nframes=%lld\nmask=%u\nhash=%llu\n",
                meta.modelId.c_str(), meta.modelVersion.c_str(),
                meta.quality, (long long)meta.rate, meta.channels,
                (long long)meta.frames, (unsigned)meta.mask,
                (unsigned long long)meta.contentHash);
            std::fclose(f);
        }
        std::filesystem::rename(tmp, dst, ec);
        if (ec) {                       // dst exists → replace
            std::filesystem::remove_all(dst, ec);
            ec.clear();
            std::filesystem::rename(tmp, dst, ec);
        }
        if (ec) { std::filesystem::remove_all(tmp, ec); return ""; }
        return key;
    }

    // ---- read -----------------------------------------------------
    /// Validate + load a whole set. `expect` = the meta the caller
    /// asked for; mismatched params (rate/ch/model/ver/quality) are a
    /// hard failure (§28 — never use incompatible results).
    bool read(const std::string& key, const StemSetMeta& expect,
              StemSet& out) {
        const auto dir = std::filesystem::path(setDir(key));
        StemSetMeta disk;
        if (!readMeta(dir, disk)) return false;
        if (disk.modelId != expect.modelId ||
            disk.modelVersion != expect.modelVersion ||
            disk.quality != expect.quality ||
            disk.rate != expect.rate ||
            disk.channels != expect.channels)
            return false;

        out.rate = disk.rate; out.channels = disk.channels;
        out.numFrames = disk.frames; out.mask = 0;
        for (int i = 0; i < kStemMax; ++i) {
            if (!(disk.mask & (1u << i))) continue;
            const auto p = dir / stemFile((StemType)i);
            double r; int ch, bits; int64_t fr;
            if (!rec::WavEncoder::validate(p.string(), &r, &ch, &fr, &bits))
                continue;                        // missing/corrupt → skip
            if ((int)r != (int)disk.rate || ch != disk.channels ||
                fr != disk.frames || bits != 32)
                continue;                        // incompatible file
            std::vector<float> buf;
            if (!readFloatWav(p.string(), buf, fr, ch)) continue;
            out.samples[(size_t)i] = std::move(buf);
            out.mask |= (1u << i);
        }
        return out.mask != 0;                    // §26 partial ok
    }

    /// Streaming seam (§31): load a frame window of one stem without
    /// materialising the whole set. `out` gets `frames*ch` samples.
    bool readRange(const std::string& key, StemType t,
                   int64_t startFrame, int64_t frames,
                   std::vector<float>& out) {
        const auto p = std::filesystem::path(setDir(key)) / stemFile(t);
        double r; int ch, bits; int64_t total;
        if (!rec::WavEncoder::validate(p.string(), &r, &ch, &total, &bits)
            || bits != 32)
            return false;
        if (startFrame < 0 || frames <= 0 || startFrame + frames > total)
            return false;
        FILE* f = std::fopen(p.string().c_str(), "rb");
        if (!f) return false;
        const int64_t off = 44 + startFrame * ch * 4;
        if (std::fseek(f, off, SEEK_SET) != 0) { std::fclose(f); return false; }
        out.resize((size_t)(frames * ch));
        const size_t got = std::fread(out.data(), 4,
                                      (size_t)(frames * ch), f);
        std::fclose(f);
        return got == (size_t)(frames * ch);
    }

    // ---- storage management (§30) ---------------------------------
    struct StorageStats {
        int64_t bytes     { 0 };
        int     sets      { 0 };
        int64_t freeBytes { 0 };
        struct Item { std::string key; int64_t bytes; };
        std::vector<Item> largest;      // top 8 by size
    };
    StorageStats stats() const {
        StorageStats st;
        std::error_code ec;
        if (std::filesystem::exists(dir_, ec)) {
            for (auto& e : std::filesystem::directory_iterator(dir_, ec)) {
                if (!e.is_directory() ||
                    e.path().extension() == ".tmp") continue;
                int64_t sz = 0;
                for (auto& f : std::filesystem::recursive_directory_iterator(
                         e.path(), ec))
                    if (f.is_regular_file())
                        sz += (int64_t)f.file_size(ec);
                if (sz > 0) {
                    st.sets++;
                    st.bytes += sz;
                    st.largest.push_back({ e.path().filename().string(), sz });
                }
            }
            std::sort(st.largest.begin(), st.largest.end(),
                      [](const StorageStats::Item& a,
                         const StorageStats::Item& b) {
                          return a.bytes > b.bytes; });
            if (st.largest.size() > 8) st.largest.resize(8);
        }
        const auto space = std::filesystem::space(dir_, ec);
        if (!ec) st.freeBytes = (int64_t)space.available;
        return st;
    }

    /// §30 — delete ONE set's files. Library metadata and original
    /// tracks are never touched (the store row is the caller's call).
    bool clearSet(const std::string& key) {
        std::error_code ec;
        const bool had = std::filesystem::exists(setDir(key), ec)
            || std::filesystem::exists(setDir(key) + ".tmp", ec);
        if (!had) return false;                 // nothing to clear
        std::filesystem::remove_all(setDir(key), ec);
        std::filesystem::remove_all(setDir(key) + ".tmp", ec);
        return !std::filesystem::exists(setDir(key), ec);
    }
    /// §30 — wipe the whole stem cache (metadata rows too — the
    /// caller clears the store). Original tracks/library untouched.
    void clearAll() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
    }

private:
    bool readMeta(const std::filesystem::path& dir, StemSetMeta& m) {
        FILE* f = std::fopen((dir / "meta").string().c_str(), "rb");
        if (!f) return false;
        char model[64] = {0}, ver[64] = {0};
        long long rate = 0, frames = 0; unsigned mask = 0;
        int quality = 0, ch = 0; unsigned long long hash = 0;
        const int got = std::fscanf(
            f, "model=%63s\nversion=%63s\nquality=%d\nrate=%lld\n"
               "channels=%d\nframes=%lld\nmask=%u\nhash=%llu\n",
            model, ver, &quality, &rate, &ch, &frames, &mask, &hash);
        std::fclose(f);
        if (got < 8) return false;
        m.modelId = model; m.modelVersion = ver; m.quality = quality;
        m.rate = (double)rate; m.channels = ch; m.frames = frames;
        m.mask = mask; m.contentHash = hash;
        return true;
    }

    /// Strict float32 RIFF reader — cache files are our own writes,
    /// so a fixed 44-byte header + f32 payload suffices.
    static bool readFloatWav(const std::string& path,
                             std::vector<float>& out,
                             int64_t frames, int ch) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        unsigned char h[44];
        if (std::fread(h, 1, 44, f) != 44) { std::fclose(f); return false; }
        out.resize((size_t)(frames * ch));
        const size_t got = std::fread(out.data(), 4,
                                      (size_t)(frames * ch), f);
        std::fclose(f);
        return got == (size_t)(frames * ch);
    }

    std::string dir_;
};

} // namespace onda::stems
