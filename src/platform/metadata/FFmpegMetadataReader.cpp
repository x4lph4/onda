#include "FFmpegMetadataReader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace onda {

namespace {

struct CtxGuard {
    AVFormatContext* p { nullptr };
    ~CtxGuard() { if (p) avformat_close_input(&p); }
};

std::string lowerExt(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

int firstInt(const std::string& s) {
    // "3/12" -> 3, "2001" -> 2001
    int v = 0; bool any = false;
    for (char c : s) {
        if (!std::isdigit((unsigned char)c)) break;
        v = v * 10 + (c - '0'); any = true;
    }
    return any ? v : 0;
}

} // namespace

std::string FFmpegMetadataReader::tag(const void* dict, const char* key) {
    const AVDictionaryEntry* e =
        av_dict_get((const AVDictionary*)dict, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

std::string FFmpegMetadataReader::tagAny(
        const void* dict, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const std::string v = tag(dict, k);
        if (!v.empty()) return v;
    }
    return "";
}

int FFmpegMetadataReader::tagInt(const void* dict, const char* key) {
    return firstInt(tag(dict, key));
}

bool FFmpegMetadataReader::read(const std::string& path,
                                library::TrackMeta& out) noexcept {
    CtxGuard g;
    if (avformat_open_input(&g.p, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(g.p, nullptr) < 0) return false;

    const int si = av_find_best_stream(g.p, AVMEDIA_TYPE_AUDIO, -1, -1,
                                       nullptr, 0);
    if (si < 0) return false;
    const AVStream* st = g.p->streams[si];
    const AVCodecParameters* cp = st->codecpar;
    if (cp->codec_type != AVMEDIA_TYPE_AUDIO) return false;

    out.channels   = cp->ch_layout.nb_channels;
    out.sampleRate = cp->sample_rate;
    out.bitrate    = (int)(cp->bit_rate > 0 ? cp->bit_rate
                                            : g.p->bit_rate);
    out.durationSec =
        g.p->duration > 0 ? (double)g.p->duration / AV_TIME_BASE : 0.0;

    const AVInputFormat* inf = g.p->iformat;
    out.format = lowerExt(path);
    if (out.format.empty() && inf && inf->name) out.format = inf->name;

    const AVDictionary* m = g.p->metadata;
    out.title       = tag(m, "title");
    out.artist      = tagAny(m, { "artist", "TPE1" });
    out.album       = tag(m, "album");
    out.albumArtist = tagAny(m, { "album_artist", "albumartist", "aART", "TPE2" });
    out.genre       = tag(m, "genre");
    out.year        = firstInt(tagAny(m, { "date", "year", "TDRC" }));
    out.trackNum    = tagInt(m, "track");
    out.discNum     = tagInt(m, "disc");

    for (unsigned i = 0; i < g.p->nb_streams; ++i)
        if (g.p->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            out.artworkHash = 1; // marker — ArtworkManager re-reads lazily
            break;
        }
    if (out.title.empty())
        out.title = std::filesystem::path(path).stem().string();
    return true;
}

bool FFmpegMetadataReader::artwork(const std::string& path,
                                   std::vector<uint8_t>& bytes,
                                   std::string& ext) noexcept {
    CtxGuard g;
    if (avformat_open_input(&g.p, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(g.p, nullptr) < 0) return false;
    int pic = -1;
    for (unsigned i = 0; i < g.p->nb_streams; ++i)
        if (g.p->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
            { pic = (int)i; break; }
    if (pic < 0) return false;

    AVPacket* pkt = av_packet_alloc();
    bool got = false;
    while (av_read_frame(g.p, pkt) >= 0) {
        if (pkt->stream_index == pic && pkt->size > 0) {
            bytes.assign(pkt->data, pkt->data + pkt->size);
            got = true;
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (!got) return false;

    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8)
        ext = "jpg";
    else if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P')
        ext = "png";
    else
        ext = "bin";
    return true;
}

} // namespace onda
