#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "LibraryScanner.h"

namespace onda::library {

/// Folder watcher: a background thread that periodically runs the
/// scanner's cheap stat-diff (`quickDiff`) and escalates changed
/// candidates to a callback (typically: re-import on the worker).
///
/// Polling, not OS events — uniform on Windows/macOS, and the stat
/// pass stays cheap at library scale (no hashing until a file's
/// size/mtime actually moved). Interval is configurable; the thread
/// never touches the DB itself — it only reports candidates.
class FileWatcher {
public:
    using ChangedFn = std::function<void(const std::vector<std::string>&)>;

    FileWatcher(LibraryScanner& scanner, ChangedFn cb)
        : scanner_(scanner), cb_(std::move(cb)) {}

    ~FileWatcher() { stop(); }
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    void start(int intervalMs = 3000) {
        stop();
        exit_ = false;
        thread_ = std::thread([this, intervalMs] {
            while (!exit_.load()) {
                const auto changed = scanner_.quickDiff();
                if (!changed.empty() && cb_) cb_(changed);
                for (int i = 0; i < intervalMs / 50 && !exit_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

    void stop() {
        exit_ = true;
        if (thread_.joinable()) thread_.join();
    }

    bool running() const { return thread_.joinable(); }

private:
    LibraryScanner& scanner_;
    ChangedFn cb_;
    std::thread thread_;
    std::atomic<bool> exit_ { false };
};

} // namespace onda::library
