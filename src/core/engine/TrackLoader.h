#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../audio/TrackAudio.h"
#include "../model/DeckStats.h"

namespace onda {

class IDecoder;
using DecoderFactory = std::function<std::unique_ptr<IDecoder>()>;

/// Background thread that decodes an ENTIRE file into a TrackAudio in RAM
/// and hands it to the deck through the pending/retired slot protocol:
///
///   loader  -- pendingTrack (atomic<const TrackAudio*>) -->  RT deck
///   RT deck -- retiredTrack (atomic<const TrackAudio*>) -->  loader
///
/// The loader OWNS the memory forever. When the RT thread swaps a track
/// out it drops the old pointer into retiredTrack; the loader frees it.
/// If a new load arrives before the previous one was adopted, the loader
/// deletes the un-adopted pending itself. The RT thread never allocates
/// and never frees.
class TrackLoader {
public:
    TrackLoader(DecoderFactory factory, DeckStats* stats,
                std::atomic<const TrackAudio*>* pendingSlot,
                std::atomic<const TrackAudio*>* retiredSlot);
    ~TrackLoader();

    TrackLoader(const TrackLoader&) = delete;
    TrackLoader& operator=(const TrackLoader&) = delete;

    /// UI thread. Cancels any in-flight load and starts a new one.
    void requestLoad(const std::string& path, double deviceRate);
    /// UI thread. Abort the current load (deck keeps whatever it has).
    void cancel();

    void requestExit();

private:
    void threadMain();
    void doLoad(const std::string& path, double deviceRate, uint32_t serial);
    void collectRetired();
    void setState(DeckState s) { stats_->state.store((int)s); }

    DecoderFactory factory_;
    DeckStats* stats_;
    std::atomic<const TrackAudio*>* pending_;
    std::atomic<const TrackAudio*>* retired_;

    enum class Cmd : int { None, Load, Cancel, Exit };
    std::atomic<Cmd>     cmd_{Cmd::None};
    std::atomic<double>  deviceRate_{48000.0};
    std::atomic<uint32_t> serial_{0};     // bumped per requestLoad; staleness cancels
    std::mutex pathMutex_;
    std::string pendingPath_;

    // Owned-but-published tracks, kept until the RT thread retires them.
    std::vector<const TrackAudio*> graveyard_;

    std::thread thread_;
    std::atomic<bool> exit_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
};

} // namespace onda
