#pragma once

#include <vector>

#include "core/decode/IDecoder.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

namespace onda {

/// IDecoder backed by libavformat/libavcodec + libswresample.
/// Outputs interleaved stereo float at the requested sample rate.
/// Thread-affinity: streamer thread only.
class FFmpegDecoder : public IDecoder {
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder() override { close(); }

    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    bool open(const std::string& path, double targetSampleRate) override;
    StreamInfo info() const override { return info_; }
    size_t readChunk(float* interleavedOut, size_t maxFrames) override;
    bool seekToFrame(int64_t frame) override;
    bool atEnd() const override { return eof_; }
    void close() override;

private:
    bool nextDecodedFrame();           // pull next AVFrame from the codec
    void convertPendingToFifo();       // push decoded frame through swr into pending_
    size_t drainPending(float* out, size_t maxFrames);

    AVFormatContext* fmt_    { nullptr };
    AVCodecContext*  codec_  { nullptr };
    AVPacket*        pkt_    { nullptr };
    AVFrame*         frame_  { nullptr };
    SwrContext*      swr_    { nullptr };
    int              streamIndex_ { -1 };

    StreamInfo info_;
    double outRate_ { 0.0 };
    bool eof_      { false };  // decoder drained
    bool flushing_ { false };  // flush packet sent to codec

    // Decoded-but-unread samples (interleaved stereo float)
    std::vector<float> pending_;
    size_t pendingRead_ { 0 };
    std::vector<float> convScratch_;
};

} // namespace onda
