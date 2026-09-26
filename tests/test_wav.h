#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

// Shared minimal PCM16 WAV writer for analysis/decoder tests.
inline void writeTestWav(const std::filesystem::path& p, int sampleRate,
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
