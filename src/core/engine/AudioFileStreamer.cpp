#include "AudioFileStreamer.h"

#include "../decode/IDecoder.h"

namespace onda {

static constexpr size_t kDecodeChunkFrames = 4096;

AudioFileStreamer::AudioFileStreamer(DecoderFactory factory, size_t fifoFrames)
    : factory_(std::move(factory)),
      fifo_(fifoFrames),
      scratch_(kDecodeChunkFrames * 2, 0.0f),
      thread_([this] { threadMain(); }) {}

AudioFileStreamer::~AudioFileStreamer() {
    exit_.store(true);
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void AudioFileStreamer::requestLoad(const std::string& path, double targetSampleRate) {
    {
        std::lock_guard<std::mutex> lk(pathMutex_);
        pendingPath_ = path;
    }
    targetRate_.store(targetSampleRate);
    pendingCmd_.store(Cmd::Load);
    wake_.notify_all();
}

void AudioFileStreamer::requestUnload() {
    pendingCmd_.store(Cmd::Unload);
    wake_.notify_all();
}

void AudioFileStreamer::threadMain() {
    while (!exit_.load()) {
        // ---- handle pending command ----
        const Cmd cmd = pendingCmd_.exchange(Cmd::None);
        if (cmd == Cmd::Load) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(pathMutex_);
                path = pendingPath_;
            }
            decoder_.reset();
            stats_.state.store((int)DeckState::Loading);
            stats_.streamEnded.store(false);
            stats_.durationSec.store(0.0);

            auto dec = factory_();
            const double rate = targetRate_.load();
            if (dec && dec->open(path, rate)) {
                decoder_ = std::move(dec);
                eof_ = false;
                fifo_.reset();
                stats_.framesRead.store(0);
                stats_.positionSec.store(0.0);
                stats_.underruns.store(0);
                const auto info = decoder_->info();
                stats_.fileSampleRate.store(info.sampleRate);
                if (info.totalFrames > 0)
                    stats_.durationSec.store(info.totalFrames / info.sampleRate);
                stats_.state.store((int)DeckState::Ready);
            } else {
                stats_.state.store((int)DeckState::Error);
            }
        } else if (cmd == Cmd::Unload) {
            decoder_.reset();
            eof_ = false;
            fifo_.reset();
            stats_.state.store((int)DeckState::Empty);
        }

        // ---- top up the fifo ----
        bool idle = true;
        if (decoder_ && !eof_) {
            idle = !pump();
        }

        if (idle) {
            std::unique_lock<std::mutex> lk(wakeMutex_);
            wake_.wait_for(lk, std::chrono::milliseconds(4));
        }
    }
}

bool AudioFileStreamer::pump() {
    const size_t space = fifo_.availableToWrite();
    if (space == 0)
        return false; // fifo full — sleep briefly

    const size_t want = std::min(space, kDecodeChunkFrames);
    const size_t got = decoder_->readChunk(scratch_.data(), want);
    if (got > 0)
        fifo_.write(scratch_.data(), got);
    if (decoder_->atEnd()) {
        eof_ = true;
        stats_.streamEnded.store(true);
    }
    return true;
}

} // namespace onda
