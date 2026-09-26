#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../audio/AudioFifo.h"
#include "../model/DeckStats.h"

namespace onda {

class IDecoder;
using DecoderFactory = std::function<std::unique_ptr<IDecoder>()>;

/// One background thread per deck. Keeps the deck's AudioFifo topped up
/// with decoded, device-rate stereo audio. Owned permanently by the
/// engine — never created or destroyed on the RT path.
class AudioFileStreamer {
public:
    explicit AudioFileStreamer(DecoderFactory decoderFactory,
                               size_t fifoFrames = 48000 * 8); // ~8 s @48k stereo
    ~AudioFileStreamer();

    AudioFileStreamer(const AudioFileStreamer&) = delete;
    AudioFileStreamer& operator=(const AudioFileStreamer&) = delete;

    /// UI thread only. Replaces any loaded track; the deck keeps its
    /// transport state but will find silence until the new file is ready.
    void requestLoad(const std::string& path, double targetSampleRate);
    void requestUnload();

    /// RT reads from this; UI/streamer write into it.
    AudioFifo& fifo() noexcept { return fifo_; }
    DeckStats& stats() noexcept { return stats_; }

private:
    void threadMain();
    bool pump(); // decode one chunk into the fifo; false = nothing to do

    DecoderFactory factory_;

    enum class Cmd : int { None, Load, Unload };
    std::atomic<Cmd> pendingCmd_{Cmd::None};
    std::mutex pathMutex_;
    std::string pendingPath_;
    std::atomic<double> targetRate_{48000.0};

    AudioFifo fifo_;
    DeckStats stats_;

    // ---- streamer-thread only ----
    std::unique_ptr<IDecoder> decoder_;
    std::vector<float> scratch_;
    bool eof_{false};

    std::thread thread_;
    std::atomic<bool> exit_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
};

} // namespace onda
