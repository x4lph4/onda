#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/rec/RecordingEncoder.h"
#include "core/rec/RecordingTypes.h"

namespace onda::rec {

/// Finds interrupted recordings and repairs them. A session is
/// RECOVERABLE when a "*.wav.journal" sidecar exists without a
/// "finalize_completed" line (crash / power loss mid-write).
/// Recovery patches the WAV header in place — audio data is never
/// rewritten — and appends a "recovered" journal line.
class RecordingRecovery {
public:
    struct Item {
        std::string journalPath;
        std::string wavPath;
        int64_t     firstTs = 0;         // journal start timestamp
        std::vector<std::string> events; // journal kinds, for Inspect
        int64_t     dataBytes = 0;
        int64_t     frames    = 0;
    };

    /// Scans a recording directory for incomplete journals.
    static std::vector<Item> scan(const std::string& dir) {
        namespace fs = std::filesystem;
        std::vector<Item> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string p = e.path().string();
            if (p.size() < 8 || p.substr(p.size() - 8) != ".journal")
                continue;
            Item it;
            it.journalPath = p;
            it.wavPath = p.substr(0, p.size() - 8);
            bool finalized = false;
            FILE* f = std::fopen(p.c_str(), "rb");
            if (!f) continue;
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                char kind[64] = {0};
                long long ts = 0;
                if (std::sscanf(line, "%lld %63s", &ts, kind) >= 2) {
                    if (!it.firstTs) it.firstTs = ts;
                    it.events.push_back(kind);
                    if (std::strcmp(kind, "finalize_completed") == 0
                        || std::strcmp(kind, "recovered") == 0
                        || std::strcmp(kind, "discarded") == 0)
                        finalized = true;   // resolved — not pending
                }
            }
            std::fclose(f);
            if (finalized) continue;          // completed — not recoverable
            // measure the audio payload left on disk
            std::error_code ec2;
            const auto sz = fs::file_size(it.wavPath, ec2);
            if (ec2 || sz <= 44) continue;    // nothing worth recovering
            it.dataBytes = (int64_t)sz - 44;
            out.push_back(std::move(it));
        }
        return out;
    }

    /// Patches the WAV header to declare the actual payload on disk.
    /// `bytesPerFrame` from the session (ch*bps); returns repaired path.
    static bool recover(Item& it, int bytesPerFrame) {
        if (bytesPerFrame <= 0) bytesPerFrame = 6;
        if (!WavEncoder::patchHeader(it.wavPath, it.dataBytes))
            return false;
        it.frames = it.dataBytes / bytesPerFrame;
        FILE* j = std::fopen(it.journalPath.c_str(), "ab");
        if (j) {
            std::fprintf(j, "0 recovered bytes=%lld\n",
                         (long long)it.dataBytes);
            std::fclose(j);
        }
        return true;
    }

    /// Discard: removes the partial WAV + journal (user-initiated only).
    static bool discard(const Item& it) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove(it.wavPath, ec);
        fs::remove(it.wavPath + ".meta.json", ec);
        // mark the journal discarded rather than deleting it — keeps
        // an audit trail and stops future scans from re-flagging
        FILE* j = std::fopen(it.journalPath.c_str(), "ab");
        if (j) { std::fputs("0 discarded\n", j); std::fclose(j); }
        return !ec;
    }
};

} // namespace onda::rec
