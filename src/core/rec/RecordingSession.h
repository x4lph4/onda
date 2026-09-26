#pragma once

#include <chrono>
#include <cstdio>
#include <string>

#include <filesystem>

#include "core/rec/RecordingTypes.h"

namespace onda::rec {

/// Small append-only event journal, sidecar to the WAV
/// ("<file>.journal"). Each event is written + flushed immediately so
/// it survives a crash: a journal missing "finalize_completed" marks
/// the session RECOVERABLE. Writer/engine threads only — never RT.
struct Journal {
    bool open(const std::string& wavPath) {
        path_ = wavPath + ".journal";
        f_ = std::fopen(path_.c_str(), "wb");
        return f_ != nullptr;
    }
    void log(const char* kind, const std::string& detail = {}) {
        if (!f_) return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::fprintf(f_, "%lld %s %s\n", (long long)ms, kind,
                     detail.c_str());
        std::fflush(f_);                    // crash-safe
    }
    void close() { if (f_) { std::fclose(f_); f_ = nullptr; } }
    const std::string& path() const { return path_; }
private:
    FILE* f_ = nullptr;
    std::string path_;
};

/// Filesystem helpers (app/writer threads).
inline int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
/// Safe, collision-free filename: "<base>_YYYY-MM-DD_HH-MM-SS[_pN].wav"
/// — appends -1/-2/... when the name already exists. Never overwrites.
inline std::string makeWavPath(const std::string& dir,
                               const std::string& base, int part = 0) {
    const std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string stem = base.empty() ? "Session" : base;
    for (char& c : stem)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    namespace fs = std::filesystem;
    for (int attempt = 0;; ++attempt) {
        std::string name = stem + "_" + stamp;
        if (attempt) name += "-" + std::to_string(attempt);
        if (part)    name += "_p" + std::to_string(part);
        name += ".wav";
        const fs::path p = fs::path(dir) / name;
        std::error_code ec;
        if (!fs::exists(p, ec)) return p.string();
    }
}
/// Estimated bytes for duration at the given format.
inline int64_t estimateBytes(double rate, BitDepth d, int ch,
                             double seconds) {
    return (int64_t)(rate * ch * bytesPerSample(d) * seconds);
}
/// Free bytes on the volume containing `dir` — 0 on error/unknown.
/// std::filesystem::space keeps this windows.h-free (min/max macros
/// would poison every header that includes this file).
inline int64_t diskFreeBytes(const std::string& dir) {
    std::error_code ec;
    const auto si = std::filesystem::space(dir, ec);
    return ec ? 0 : (int64_t)si.available;
}

} // namespace onda::rec
