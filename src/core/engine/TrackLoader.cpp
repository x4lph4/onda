#include "TrackLoader.h"

#include <chrono>

#include "../decode/IDecoder.h"

namespace onda {

static constexpr size_t kChunk = 16384; // decode granularity (frames)

TrackLoader::TrackLoader(DecoderFactory factory, DeckStats* stats,
                         std::atomic<const TrackAudio*>* pendingSlot,
                         std::atomic<const TrackAudio*>* retiredSlot)
    : factory_(std::move(factory)),
      stats_(stats), pending_(pendingSlot), retired_(retiredSlot),
      thread_([this] { threadMain(); }) {}

TrackLoader::~TrackLoader() {
    requestExit();
}

void TrackLoader::requestExit() {
    exit_.store(true);
    cmd_.store(Cmd::Exit);
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void TrackLoader::requestLoad(const std::string& path, double deviceRate) {
    {
        std::lock_guard<std::mutex> lk(pathMutex_);
        pendingPath_ = path;
    }
    deviceRate_.store(deviceRate);
    serial_.fetch_add(1);
    cmd_.store(Cmd::Load);
    wake_.notify_all();
}

void TrackLoader::cancel() {
    serial_.fetch_add(1); // invalidate in-flight work
    cmd_.store(Cmd::Cancel);
    wake_.notify_all();
}

void TrackLoader::collectRetired() {
    const TrackAudio* expected = retired_->load(std::memory_order_acquire);
    while (expected != nullptr &&
           !retired_->compare_exchange_strong(expected, nullptr,
                                              std::memory_order_acq_rel)) {}
    if (expected != nullptr)
        delete expected; // loader is the sole owner of every TrackAudio
}

void TrackLoader::threadMain() {
    while (!exit_.load()) {
        collectRetired();

        const Cmd cmd = cmd_.exchange(Cmd::None);
        if (cmd == Cmd::Load) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(pathMutex_);
                path = pendingPath_;
            }
            doLoad(path, deviceRate_.load(), serial_.load());
        }

        std::unique_lock<std::mutex> lk(wakeMutex_);
        wake_.wait_for(lk, std::chrono::milliseconds(20));
    }

    // Drain: free everything we still own.
    collectRetired();
    for (const TrackAudio* t : graveyard_) delete t;
    graveyard_.clear();
}

void TrackLoader::doLoad(const std::string& path, double deviceRate,
                         uint32_t serial) {
    setState(DeckState::Loading);
    stats_->streamEnded.store(false);
    stats_->loadProgress.store(0.0f);
    stats_->durationSec.store(0.0);
    const auto t0 = std::chrono::steady_clock::now();

    auto isStale = [&] { return serial_.load() != serial || exit_.load(); };

    auto dec = factory_();
    if (!dec || !dec->open(path, deviceRate)) {
        setState(DeckState::Error);
        stats_->loadProgress.store(0.0f);
        return;
    }
    const auto info = dec->info();

    auto track = std::make_unique<TrackAudio>();
    track->path           = path;
    track->title          = info.title;
    track->artist         = info.artist;
    track->fileSampleRate = info.sampleRate;
    track->deviceRate     = deviceRate;
    track->channels       = 2;
    if (info.totalFrames > 0) {
        const double secs = info.totalFrames / info.sampleRate;
        track->samples.reserve((size_t)(secs * deviceRate * 2.0 * 1.05));
        stats_->durationSec.store(secs);
    }

    std::vector<float> scratch(kChunk * 2);
    int64_t frames = 0;
    while (!isStale()) {
        const size_t got = dec->readChunk(scratch.data(), kChunk);
        if (got == 0) break;
        track->samples.insert(track->samples.end(),
                              scratch.begin(), scratch.begin() + (ptrdiff_t)got * 2);
        frames += (int64_t)got;
        if (info.totalFrames > 0) {
            const double est = (double)frames / deviceRate * info.sampleRate;
            stats_->loadProgress.store(
                (float)std::min(0.99, est / (double)info.totalFrames));
        }
    }

    if (isStale()) {
        // Abandoned load: free and leave deck state untouched.
        setState(DeckState::Empty);
        stats_->loadProgress.store(0.0f);
        return;
    }

    track->numFrames = frames;
    if (track->durationSec() > 0.0)
        stats_->durationSec.store(track->durationSec());

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    stats_->loadLatencyMs.store((float)ms);
    stats_->totalBytes.store((int64_t)track->bytes());
    stats_->loadProgress.store(1.0f);
    stats_->framesRead.store(0);
    stats_->positionSec.store(0.0);
    stats_->underruns.store(0);

    // Publish: whatever was sitting un-adopted in the slot is ours to free.
    const TrackAudio* old = pending_->exchange(track.release(),
                                               std::memory_order_acq_rel);
    if (old != nullptr)
        delete old;
    setState(DeckState::Ready);
}

} // namespace onda
