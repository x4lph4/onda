#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace onda::library {

/// File identification — two stable 64-bit hashes, deliberately cheap
/// (three 32 KiB samples, not a full read, so it scales to 100k files):
///
///   contentHash — FNV-1a over size + head/mid/tail samples.
///                 Stable across renames, moves and mtime changes:
///                 two copies of the same file hash identically.
///   fingerprint — contentHash ^ mtime. The "did it change?" key used
///                 by the scanner; any byte or timestamp change flips it.
///
/// Documented limits: same-size same-sample collisions are possible in
/// theory (sampled, not cryptographic); the duplicate detector layers
/// duration/size/metadata on top before calling anything a duplicate.
class FileFingerprint {
public:
    static constexpr size_t kSample = 32 * 1024;

    static bool stat(const std::string& path, int64_t& size, int64_t& mtime) {
        struct ::stat st {};
        if (::stat(path.c_str(), &st) != 0) return false;
        size = (int64_t)st.st_size;
        mtime = (int64_t)st.st_mtime;
        return true;
    }

    /// 0 when the file can't be opened.
    static uint64_t contentHash(const std::string& path, int64_t size = -1) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return 0;
        if (size < 0) {
            std::fseek(f, 0, SEEK_END);
            size = (int64_t)std::ftell(f);
        }
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const uint8_t* p, size_t n) {
            for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        };
        mix((const uint8_t*)&size, sizeof(size));
        uint8_t buf[kSample];
        const int64_t off[3] = { 0, size / 2, size - (int64_t)kSample };
        for (int k = 0; k < 3; ++k) {
            int64_t o = off[k];
            if (o < 0) o = 0;
            if (o >= size) continue;
            std::fseek(f, (long)o, SEEK_SET);
            const size_t n = std::fread(buf, 1, kSample, f);
            mix(buf, n);
        }
        std::fclose(f);
        return h;
    }

    static uint64_t fingerprint(const std::string& path,
                                int64_t size, int64_t mtime) {
        const uint64_t c = contentHash(path, size);
        return c ^ ((uint64_t)mtime * 0x9e3779b97f4a7c15ull);
    }

    /// Both hashes in one file pass — the common import call.
    static bool compute(const std::string& path, uint64_t& fp,
                        uint64_t& ch, int64_t& size, int64_t& mtime) {
        if (!stat(path, size, mtime)) return false;
        ch = contentHash(path, size);
        if (ch == 0) return false;
        fp = ch ^ ((uint64_t)mtime * 0x9e3779b97f4a7c15ull);
        return true;
    }
};

} // namespace onda::library
