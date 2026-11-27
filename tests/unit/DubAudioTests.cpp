// SPDX-License-Identifier: GPL-3.0-only
#include "../../src/ui/DubAudio.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using namespace veyra::ui::dub;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, uint16_t(v)); put16(b, uint16_t(v >> 16)); }
void tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

std::vector<uint8_t> wave16(unsigned channels, uint32_t rate, const std::vector<int16_t>& samples, bool extraChunk) {
    std::vector<uint8_t> data;
    for (auto s : samples) put16(data, uint16_t(s));
    std::vector<uint8_t> b;
    tag(b, "RIFF"); put32(b, 0); tag(b, "WAVE");
    if (extraChunk) { tag(b, "LIST"); put32(b, 3); b.insert(b.end(), {1, 2, 3, 0}); } // odd size, padded
    tag(b, "fmt "); put32(b, 16); put16(b, 1); put16(b, uint16_t(channels)); put32(b, rate);
    put32(b, rate * channels * 2); put16(b, uint16_t(channels * 2)); put16(b, 16);
    tag(b, "data"); put32(b, uint32_t(data.size())); b.insert(b.end(), data.begin(), data.end());
    return b;
}

void wave() {
    const auto bytes = wave16(2, 22050, {16384, -16384, 32767, 32767, -32768, -32768}, true);
    const auto pcm = parseWave(bytes.data(), bytes.size());
    require(pcm && pcm->rate == 22050, "16-bit stereo WAVE parsed after an odd-sized chunk");
    require(pcm->samples.size() == 3, "stereo frames are downmixed to mono");
    require(std::abs(pcm->samples[0]) < 1e-6f && pcm->samples[1] > 0.99f && pcm->samples[2] == -1.0f, "downmix values");
    auto broken = bytes;
    broken.resize(broken.size() - 2);
    require(!parseWave(broken.data(), broken.size()), "truncated data chunk rejected");
    require(!parseWave(bytes.data(), 8), "short buffer rejected");
}

void resample() {
    std::vector<float> in(24000);
    for (size_t i = 0; i < in.size(); ++i) in[i] = 0.5f * float(std::sin(2 * std::numbers::pi * 1000.0 * double(i) / 24000.0));
    const auto out = resampleTo48k(in, 24000);
    require(out.size() == 48000, "24 kHz -> 48 kHz doubles the length");
    double peak = 0, sum = 0;
    int crossings = 0;
    for (size_t i = 2000; i < 46000; ++i) {
        peak = std::max(peak, double(std::abs(out[i])));
        sum += double(out[i]) * out[i];
        if ((out[i - 1] < 0) != (out[i] < 0)) ++crossings;
    }
    require(std::abs(peak - 0.5) < 0.01, "amplitude kept");
    require(std::abs(std::sqrt(sum / 44000) - 0.5 / std::sqrt(2.0)) < 0.005, "RMS kept");
    require(std::abs(crossings - 1833) <= 3, "1 kHz stays 1 kHz"); // 44000 samples at 48 kHz = 916.7 cycles
    const auto same = resampleTo48k(in, 48000);
    require(same == in, "48 kHz input is returned unchanged");
    require(resampleTo48k(in, 0).empty(), "invalid rate rejected");

    std::vector<float> pcm(1000, 1.0f);
    fadeEdges(pcm);
    require(pcm.front() == 0.0f && pcm.back() == 0.0f && pcm[500] == 1.0f, "clip edges faded, middle untouched");
}
} // namespace

int main() try {
    wave();
    resample();
    std::cout << "dub audio tests passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAILED: " << e.what() << "\n";
    return 1;
}
