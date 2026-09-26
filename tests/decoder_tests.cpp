#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "platform/decode/FFmpegDecoder.h"
#include "core/engine/AudioFileStreamer.h"
#include "core/engine/DeckEngine.h"

using namespace onda;
using Catch::Approx;

// Minimal PCM16 WAV writer (RIFF) — no external tools needed.
static void writeWav(const std::filesystem::path& p, int sampleRate,
                     int channels, const std::vector<int16_t>& samples) {
    std::ofstream f(p, std::ios::binary);
    const uint32_t dataSize  = (uint32_t)(samples.size() * sizeof(int16_t));
    const uint32_t fmtSize   = 16;
    const uint16_t bits      = 16;
    const uint32_t byteRate  = sampleRate * channels * bits / 8;
    const uint16_t blockAlign = channels * bits / 8;

    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };

    f.write("RIFF", 4); w32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(fmtSize); w16(1); w16((uint16_t)channels);
    w32((uint32_t)sampleRate); w32(byteRate); w16(blockAlign); w16(bits);
    f.write("data", 4); w32(dataSize);
    f.write((const char*)samples.data(), dataSize);
}

TEST_CASE("FFmpegDecoder decodes a PCM16 WAV to device-rate stereo float") {
    const int fileRate = 44100, devRate = 48000, channels = 2;
    const int fileFrames = fileRate / 2; // 0.5 s

    std::vector<int16_t> pcm((size_t)fileFrames * channels);
    for (int i = 0; i < fileFrames; ++i) {
        const float v = 0.5f * std::sin(2.0 * 3.14159265 * 440.0 * i / fileRate);
        pcm[(size_t)i * 2] = pcm[(size_t)i * 2 + 1] = (int16_t)(v * 30000);
    }

    const auto dir = std::filesystem::temp_directory_path() / "onda_tests";
    std::filesystem::create_directories(dir);
    const auto wav = dir / "sine440.wav";
    writeWav(wav, fileRate, channels, pcm);

    FFmpegDecoder dec;
    REQUIRE(dec.open(wav.string(), devRate));

    const auto info = dec.info();
    REQUIRE(info.sampleRate == fileRate);
    REQUIRE(info.channels == 2);
    REQUIRE(info.totalFrames == Approx(fileFrames).margin(fileRate / 50));

    // decode everything: expect ~0.5 s at device rate
    std::vector<float> out(4096 * 2);
    size_t total = 0;
    while (total < 48000) {
        const size_t got = dec.readChunk(out.data(), 4096);
        if (got == 0) break;
        total += got;
        REQUIRE(got <= 4096);
    }
    REQUIRE(total == Approx(24000).margin(400)); // ~0.5 s @48k, ±1 resample tap

    // non-silent content
    double energy = 0;
    for (float v : out) energy += v * v;
    REQUIRE(energy > 0.1);
    REQUIRE(dec.atEnd());
    dec.close();
}

TEST_CASE("FFmpegDecoder rejects a nonexistent file") {
    FFmpegDecoder dec;
    REQUIRE_FALSE(dec.open("C:/definitely/not/here.mp3", 48000));
}

TEST_CASE("Streamer + decoder + deck: full non-UI playback chain") {
    const int fileRate = 48000, channels = 2;
    const int fileFrames = fileRate / 4; // 0.25 s

    std::vector<int16_t> pcm((size_t)fileFrames * channels);
    for (int i = 0; i < fileFrames; ++i) {
        const float v = 0.6f * std::sin(2.0 * 3.14159265 * 220.0 * i / fileRate);
        pcm[(size_t)i * 2] = pcm[(size_t)i * 2 + 1] = (int16_t)(v * 30000);
    }
    const auto dir = std::filesystem::temp_directory_path() / "onda_tests";
    std::filesystem::create_directories(dir);
    const auto wav = dir / "chain.wav";
    writeWav(wav, fileRate, channels, pcm);

    AudioFileStreamer streamer([] { return std::make_unique<FFmpegDecoder>(); });
    DeckEngine deck;
    deck.bind(&streamer.fifo(), &streamer.stats());
    deck.setDeviceRate(48000.0);

    streamer.requestLoad(wav.string(), 48000.0);

    // wait for the streamer thread to reach Ready (poll, bounded)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (streamer.stats().state.load() != (int)DeckState::Ready &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(streamer.stats().state.load() == (int)DeckState::Ready);
    REQUIRE(streamer.stats().durationSec.load() == Approx(0.25).margin(0.05));

    deck.setPlaying(true);
    std::vector<float> mixL(512, 0.0f), mixR(512, 0.0f);
    int blocks = 0;
    while (streamer.stats().positionSec.load() < 0.1 && blocks < 200) {
        deck.addToMix(mixL.data(), mixR.data(), 512);
        ++blocks;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(streamer.stats().positionSec.load() > 0.0);
    REQUIRE(streamer.stats().levelL.load() > 0.1f); // real audio arrived
}
