#include "KeyAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <vector>
// <algorithm> needed for std::clamp

#include "../dsp/Fft.h"

namespace onda {

// Krumhansl–Schmuckler probe profiles (C major / C minor).
static const double kMajor[12] = {
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
static const double kMinor[12] = {
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

static const char* kNames[24] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
    "Cm", "C#m", "Dm", "D#m", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "A#m", "Bm" };

// Camelot: majors -> B column, minors -> A column.
static const char* kCamelot[24] = {
    "8B","3B","10B","5B","12B","7B","2B","9B","4B","11B","6B","1B",
    "5A","12A","7A","2A","9A","4A","11A","6A","1A","8A","3A","10A" };

const char* KeyAnalyzer::standardName(int key) {
    return key >= 0 && key < 24 ? kNames[key] : "?";
}
const char* KeyAnalyzer::camelotName(int key) {
    return key >= 0 && key < 24 ? kCamelot[key] : "?";
}

KeyResult KeyAnalyzer::analyze(const float* samples, int64_t numFrames,
                               int channels, double sampleRate) {
    KeyResult r;
    if (samples == nullptr || numFrames <= 0) return r;

    constexpr int kOrder = 13;          // 8192 FFT ≈ 5.9 Hz/bin @ 48 kHz
    constexpr int kHop   = 4096;
    Fft fft;
    fft.prepare(kOrder);
    const int n = fft.size(), bins = n / 2;
    std::vector<float> mags((size_t)bins, 0.0f), mono((size_t)n, 0.0f);
    double chroma[12] = {};

    const double binHz = sampleRate / (double)n;
    // Precompute bin->pitch-class table (55 Hz..5 kHz, A0..~C8).
    std::vector<int> binPc((size_t)bins, -1);
    for (int b = 1; b < bins; ++b) {
        const double f = b * binHz;
        if (f < 55.0 || f > 5000.0) continue;
        const double midi = 69.0 + 12.0 * std::log2(f / 440.0);
        binPc[(size_t)b] = ((int)std::lround(midi) % 12 + 12) % 12;
    }

    int frames = 0;
    for (int64_t hop = 0; hop + n <= numFrames && frames < 1500; hop += kHop, ++frames) {
        float acc = 0.0f;
        for (int i = 0; i < n; ++i) {
            float s = 0.0f;
            for (int c = 0; c < channels; ++c)
                s += samples[(size_t)(hop + i) * (size_t)channels + (size_t)c];
            mono[(size_t)i] = s / (float)channels;
            acc += mono[(size_t)i] * mono[(size_t)i];
        }
        if (acc / n < 1e-5) continue; // skip quiet frames
        fft.magnitudeSpectrum(mono.data(), n, mags.data());
        for (int b = 1; b < bins; ++b) {
            const int pc = binPc[(size_t)b];
            if (pc >= 0) chroma[pc] += (double)mags[(size_t)b] * mags[(size_t)b];
        }
    }
    if (frames == 0) return r;

    double total = 0.0;
    for (double v : chroma) total += v;
    if (total <= 0.0) return r;
    for (double& v : chroma) v /= total;

    // Correlate each rotation of both profiles (Pearson).
    auto pearson = [](const double* x, const double* y) {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += x[i]; my += y[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i) {
            const double dx = x[i] - mx, dy = y[i] - my;
            sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
        }
        return syy > 0.0 && sxx > 0.0 ? sxy / std::sqrt(sxx * syy) : 0.0;
    };

    double rot[12];
    double best = -2.0, second = -2.0; int bestKey = -1;
    for (int mode = 0; mode < 2; ++mode) {
        const double* prof = mode == 0 ? kMajor : kMinor;
        for (int root = 0; root < 12; ++root) {
            for (int i = 0; i < 12; ++i)
                rot[i] = prof[(i - root + 12) % 12];
            const double c = pearson(chroma, rot);
            const int keyIdx = mode == 0 ? root : 12 + root;
            if (c > best) { second = best; best = c; bestKey = keyIdx; }
            else if (c > second) second = c;
        }
    }
    r.key = bestKey;
    r.confidence = best > 0.0
        ? (float)std::clamp((best - second) * 4.0 + best * 0.5, 0.0, 1.0)
        : 0.0f;
    r.standard = standardName(bestKey);
    r.camelot  = camelotName(bestKey);
    return r;
}

} // namespace onda
