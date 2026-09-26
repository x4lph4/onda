#include "FFmpegDecoder.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace onda {

bool FFmpegDecoder::open(const std::string& path, double targetSampleRate) {
    close();
    outRate_ = targetSampleRate;

    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(fmt_, nullptr) < 0) { close(); return false; }

    streamIndex_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex_ < 0) { close(); return false; }

    AVStream* st = fmt_->streams[streamIndex_];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (codec == nullptr) { close(); return false; }

    codec_ = avcodec_alloc_context3(codec);
    if (codec_ == nullptr || avcodec_parameters_to_context(codec_, st->codecpar) < 0 ||
        avcodec_open2(codec_, codec, nullptr) < 0) {
        close();
        return false;
    }

    pkt_   = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (pkt_ == nullptr || frame_ == nullptr) { close(); return false; }

    // Input layout may be unset for some codecs — fall back to channel count.
    AVChannelLayout inLayout{};
    if (codec_->ch_layout.nb_channels > 0)
        inLayout = codec_->ch_layout;
    else
        av_channel_layout_default(&inLayout, codec_->ch_layout.nb_channels != 0
                                             ? codec_->ch_layout.nb_channels : 2);

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, (int)outRate_,
                            &inLayout, codec_->sample_fmt, codec_->sample_rate,
                            0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
        close();
        return false;
    }

    info_.sampleRate = codec_->sample_rate;
    info_.channels   = codec_->ch_layout.nb_channels;
    info_.codecName  = codec->name;
    if (st->duration != AV_NOPTS_VALUE)
        info_.totalFrames = (int64_t)(st->duration * av_q2d(st->time_base) * info_.sampleRate);
    else if (fmt_->duration != AV_NOPTS_VALUE)
        info_.totalFrames = (int64_t)(fmt_->duration / (double)AV_TIME_BASE * info_.sampleRate);

    eof_ = flushing_ = false;
    pending_.clear();
    pendingRead_ = 0;
    return true;
}

bool FFmpegDecoder::nextDecodedFrame() {
    while (true) {
        const int ret = avcodec_receive_frame(codec_, frame_);
        if (ret == 0)
            return true;
        if (ret == AVERROR_EOF) {
            eof_ = true;
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            eof_ = true; // decode error — treat as end
            return false;
        }

        // EAGAIN: feed another packet.
        if (!flushing_) {
            while (true) {
                const int r = av_read_frame(fmt_, pkt_);
                if (r < 0) {
                    avcodec_send_packet(codec_, nullptr); // flush once
                    flushing_ = true;
                    break;
                }
                if (pkt_->stream_index == streamIndex_) {
                    avcodec_send_packet(codec_, pkt_);
                    av_packet_unref(pkt_);
                    break;
                }
                av_packet_unref(pkt_);
            }
        } else {
            eof_ = true;
            return false;
        }
    }
}

void FFmpegDecoder::convertPendingToFifo() {
    const int inRate = (int)info_.sampleRate;
    const int64_t delayed = swr_get_delay(swr_, inRate);
    const int outCap = (int)av_rescale_rnd(delayed + frame_->nb_samples,
                                           (int64_t)outRate_, inRate, AV_ROUND_UP);
    convScratch_.resize((size_t)outCap * 2);
    uint8_t* dst = (uint8_t*)convScratch_.data();
    const int conv = swr_convert(swr_, &dst, outCap,
                                 (const uint8_t**)frame_->extended_data,
                                 frame_->nb_samples);
    if (conv > 0) {
        if (pendingRead_ > 0) { // compact before appending
            pending_.erase(pending_.begin(), pending_.begin() + (ptrdiff_t)pendingRead_);
            pendingRead_ = 0;
        }
        pending_.insert(pending_.end(), convScratch_.begin(),
                        convScratch_.begin() + (size_t)conv * 2);
    }
}

size_t FFmpegDecoder::drainPending(float* out, size_t maxFrames) {
    const size_t availFrames = (pending_.size() - pendingRead_) / 2;
    const size_t n = std::min(availFrames, maxFrames);
    std::memcpy(out, pending_.data() + pendingRead_, n * 2 * sizeof(float));
    pendingRead_ += n * 2;
    if (pendingRead_ == pending_.size()) {
        pending_.clear();
        pendingRead_ = 0;
    }
    return n;
}

size_t FFmpegDecoder::readChunk(float* out, size_t maxFrames) {
    size_t produced = drainPending(out, maxFrames);
    while (produced < maxFrames && !eof_) {
        if (!nextDecodedFrame())
            break;
        convertPendingToFifo();
        produced += drainPending(out + produced * 2, maxFrames - produced);
    }
    return produced;
}

bool FFmpegDecoder::seekToFrame(int64_t frame) {
    if (fmt_ == nullptr || streamIndex_ < 0)
        return false;
    AVStream* st = fmt_->streams[streamIndex_];
    const int64_t ts = av_rescale_q(frame, av_make_q(1, (int)info_.sampleRate), st->time_base);
    if (av_seek_frame(fmt_, streamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    avcodec_flush_buffers(codec_);
    if (swr_ != nullptr) {
        swr_close(swr_);
        swr_init(swr_);
    }
    pending_.clear();
    pendingRead_ = 0;
    eof_ = flushing_ = false;
    return true;
}

void FFmpegDecoder::close() {
    if (swr_ != nullptr)   { swr_free(&swr_); }
    if (frame_ != nullptr) { av_frame_free(&frame_); }
    if (pkt_ != nullptr)   { av_packet_free(&pkt_); }
    if (codec_ != nullptr) { avcodec_free_context(&codec_); }
    if (fmt_ != nullptr)   { avformat_close_input(&fmt_); }
    streamIndex_ = -1;
}

} // namespace onda
