#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "LibraryTypes.h"
#include "TrackImporter.h"

namespace onda::library {

/// §9-10/§61 — import job queue with per-item state, progress and
/// cooperative cancellation.
///
///   Queued → Scanning → Analyzing → Completed
///                    ↘ Failed        ↘ Cancelled (cooperative —
///                                          checked between items)
///
/// Items are processed ONE PER worker task (the queue posts itself
/// onto the library worker between items), so a huge folder import
/// never monopolises the worker, cancellation lands within one file,
/// and the UI can poll `items()`/`progress` at any cadence.
/// `resume()` re-queues everything not Completed — cancelled/failed
/// items retry, finished work is never repeated.
enum class ImportState : int {
    Queued = 0, Scanning, Analyzing, Completed, Failed, Cancelled
};

struct ImportItem {
    std::string path;
    ImportState state { ImportState::Queued };
    int64_t     trackId { -1 };
    std::string error;
};

class ImportQueue {
public:
    using PostFn     = std::function<void(std::function<void()>)>;
    using ProgressFn = std::function<void(const ImportItem& item,
                                          int done, int total)>;
    using DoneFn     = std::function<void(const ImportResult&)>;

    ImportQueue(TrackImporter& importer, PostFn post)
        : importer_(importer), post_(std::move(post)) {}

    /// Append paths as QUEUED items and start processing. Returns the
    /// new total item count (jobs accumulate — the queue is a view of
    /// the whole batch, not per-call).
    int enqueue(const std::vector<std::string>& paths) {
        {
            std::lock_guard lk(m_);
            for (const auto& p : paths) items_.push_back({ p });
            total_ = (int)items_.size();
            finishedNotified_ = false;
            cancel_ = false;
        }
        kick();
        return total_;
    }

    /// Cooperative stop: items not yet touched become CANCELLED, the
    /// in-flight item finishes its file (imports are per-file
    /// transactional — never half-written rows).
    void cancel() {
        {
            std::lock_guard lk(m_);
            cancel_ = true;
            for (auto& it : items_)
                if (it.state == ImportState::Queued)
                    it.state = ImportState::Cancelled;
        }
        finish();
    }

    /// Re-queue every non-Completed item (cancelled + failed retry)
    /// and continue where the queue stopped.
    void resume() {
        {
            std::lock_guard lk(m_);
            if (running_) return;
            cancel_ = false;
            finishedNotified_ = false;
            for (auto& it : items_)
                if (it.state != ImportState::Completed) {
                    it.state = ImportState::Queued;
                    it.error.clear();
                }
        }
        kick();
    }

    /// Drop Completed/Cancelled/Failed bookkeeping (keeps the list
    /// bounded across many sessions).
    void clearFinished() {
        std::lock_guard lk(m_);
        std::erase_if(items_, [](const ImportItem& i) {
            return i.state != ImportState::Queued &&
                   i.state != ImportState::Scanning &&
                   i.state != ImportState::Analyzing;
        });
        total_ = (int)items_.size();
        done_  = 0;
        skipped_ = 0;
    }

    bool running() const {
        std::lock_guard lk(m_);
        return running_;
    }
    int  totalItems() const { return total_; }
    int  doneItems()  const { return done_; }

    /// Snapshot for UI polling — one copy, callers never hold refs.
    std::vector<ImportItem> items() const {
        std::lock_guard lk(m_);
        return items_;
    }

    void setProgress(ProgressFn cb) {
        std::lock_guard lk(m_);
        progress_ = std::move(cb);
    }
    void setDone(DoneFn cb) {
        std::lock_guard lk(m_);
        doneFn_ = std::move(cb);
    }

private:
    void kick() {
        {
            std::lock_guard lk(m_);
            if (running_ || cancel_) return;
            running_ = true;
        }
        post_([this] { step(); });
    }

    /// One item per worker task — the queue re-posts itself.
    void step() {
        ImportItem* cur = nullptr;
        {
            std::lock_guard lk(m_);
            for (auto& it : items_)
                if (it.state == ImportState::Queued) { cur = &it; break; }
            if (cur == nullptr) { running_ = false; }
        }
        if (cur == nullptr) { finish(); return; }

        const std::string path = cur->path;
        {
            std::lock_guard lk(m_);
            cur->state = ImportState::Scanning;
        }
        Track t;
        const auto out = importer_.importOne(path, &t);
        {
            std::lock_guard lk(m_);
            switch (out) {
                case TrackImporter::Outcome::Added:
                case TrackImporter::Outcome::Updated:
                    // Row stored — analysis enqueued externally by the
                    // importer hook; surface the analyzing edge before
                    // completing the item.
                    cur->state   = ImportState::Analyzing;
                    cur->trackId = t.id;
                    break;
                case TrackImporter::Outcome::Skipped:
                    // fingerprint unchanged — already in the library;
                    // completes but counts as skipped, not added.
                    cur->state   = ImportState::Completed;
                    cur->trackId = t.id;
                    ++skipped_;
                    break;
                case TrackImporter::Outcome::Failed:
                default:
                    cur->state = ImportState::Failed;
                    cur->error = "import failed";
                    break;
            }
            if (cur->state == ImportState::Analyzing)
                cur->state = ImportState::Completed;
            ++done_;
            if (progress_) progress_(*cur, done_, total_);
        }
        {
            std::lock_guard lk(m_);
            if (cancel_.load()) { running_ = false; }
        }
        post_([this] { step(); });
    }

    /// Emit the aggregate once the queue drains or is cancelled.
    void finish() {
        ProgressFn none;
        DoneFn     doneFn;
        ImportResult res;
        {
            std::lock_guard lk(m_);
            if (finishedNotified_) return;
            finishedNotified_ = true;
            for (const auto& it : items_) {
                switch (it.state) {
                    case ImportState::Completed: ++res.added; break;
                    case ImportState::Failed:    ++res.failed; break;
                    case ImportState::Cancelled: ++res.skipped; break;
                    default: break;
                }
            }
            res.added   -= skipped_.load();
            res.skipped += skipped_.load();
            running_ = false;
            doneFn   = doneFn_;
        }
        if (doneFn) doneFn(res);
    }

    TrackImporter& importer_;
    PostFn         post_;
    mutable std::mutex       m_;
    std::vector<ImportItem>  items_;
    std::atomic<int>         total_ { 0 };
    std::atomic<int>         done_  { 0 };
    std::atomic<int>         skipped_ { 0 };
    std::atomic<bool>        cancel_ { false };
    bool                     running_ { false };
    bool                     finishedNotified_ { false };
    ProgressFn               progress_;
    DoneFn                   doneFn_;
};

} // namespace onda::library
