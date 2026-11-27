// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// PCM helpers for the translated voice (live dubbing). Pure functions without
// Qt or Windows dependencies; they run on the synthesis worker / UI thread,
// never on the audio thread.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <vector>

namespace veyra::ui::dub {

struct MonoPcm {
    std::vector<float> samples;
    int rate = 0;
};

// Little-endian signed 16-bit PCM (OpenAI "pcm" speech: 24 kHz mono).
inline std::vector<float> pcm16ToFloat(const uint8_t* bytes, size_t size, unsigned channels = 1) {
    std::vector<float> out;
    if (!bytes || !channels) return out;
    const size_t frames = size / (2 * channels);
    out.resize(frames);
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0;
        for (unsigned c = 0; c < channels; ++c) {
            const size_t at = (f * channels + c) * 2;
            sum += float(int16_t(uint16_t(bytes[at]) | uint16_t(bytes[at + 1]) << 8)) / 32768.0f;
        }
        out[f] = sum / float(channels);
    }
    return out;
}

// RIFF/WAVE with 16-bit PCM or 32-bit float, any channel count (downmixed to
// mono). Windows speech synthesis returns this container.
inline std::optional<MonoPcm> parseWave(const uint8_t* bytes, size_t size) {
    const auto u16 = [&](size_t at) { return uint16_t(bytes[at] | bytes[at + 1] << 8); };
    const auto u32 = [&](size_t at) {
        return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
    };
    if (!bytes || size < 12 || std::memcmp(bytes, "RIFF", 4) || std::memcmp(bytes + 8, "WAVE", 4)) return std::nullopt;
    unsigned format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    for (size_t at = 12; at + 8 <= size;) {
        const uint32_t length = u32(at + 4);
        const size_t body = at + 8;
        if (length > size - body) return std::nullopt;
        if (!std::memcmp(bytes + at, "fmt ", 4) && length >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && length >= 40) format = u16(body + 24); // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (!std::memcmp(bytes + at, "data", 4)) {
            if (!channels || channels > 8 || rate < 8000 || rate > 192000) return std::nullopt;
            MonoPcm pcm;
            pcm.rate = int(rate);
            if (format == 1 && bits == 16) {
                pcm.samples = pcm16ToFloat(bytes + body, length, channels);
            } else if (format == 3 && bits == 32) {
                const size_t frames = length / (4 * channels);
                pcm.samples.resize(frames);
                for (size_t f = 0; f < frames; ++f) {
                    float sum = 0;
                    for (unsigned c = 0; c < channels; ++c) {
                        float v;
                        std::memcpy(&v, bytes + body + (f * channels + c) * 4, 4);
                        sum += std::isfinite(v) ? v : 0.0f;
                    }
                    pcm.samples[f] = sum / float(channels);
                }
            } else {
                return std::nullopt;
            }
            return pcm;
        }
        at = body + length + (length & 1);
    }
    return std::nullopt;
}

// Windowed-sinc (Blackman, 32 taps per side) conversion to 48 kHz. The
// cut-off sits below the lower Nyquist frequency so 16/22.05/24 kHz voices do
// not image into the 8-12 kHz range.
inline std::vector<float> resampleTo48k(const std::vector<float>& in, int rate) {
    constexpr int target = 48000, half = 32;
    if (rate == target || in.empty()) return in;
    if (rate <= 0) return {};
    const double ratio = double(rate) / target;
    const double cutoff = 0.94 * std::min(1.0, 1.0 / ratio); // relative to the input Nyquist
    const size_t frames = size_t(std::floor(double(in.size()) / ratio));
    std::vector<float> out(frames);
    for (size_t i = 0; i < frames; ++i) {
        const double centre = double(i) * ratio;
        const long first = long(std::floor(centre)) - half + 1;
        double sum = 0, weights = 0;
        for (long j = first; j < first + 2 * half; ++j) {
            const double x = double(j) - centre;
            const double t = x / half;
            if (t <= -1.0 || t >= 1.0) continue;
            const double window = 0.42 + 0.5 * std::cos(std::numbers::pi * t) + 0.08 * std::cos(2 * std::numbers::pi * t);
            const double arg = std::numbers::pi * cutoff * x;
            const double sinc = std::abs(arg) < 1e-9 ? 1.0 : std::sin(arg) / arg;
            const double w = cutoff * sinc * window;
            weights += w;
            if (j >= 0 && size_t(j) < in.size()) sum += w * in[size_t(j)];
        }
        out[i] = weights > 0 ? float(sum / weights) : 0.0f;
    }
    return out;
}

// Short fades at both ends so a clip never starts or stops with a click.
inline void fadeEdges(std::vector<float>& pcm, size_t frames = 240) {
    const size_t n = std::min(frames, pcm.size() / 2);
    for (size_t i = 0; i < n; ++i) {
        const float g = float(i) / float(n);
        pcm[i] *= g;
        pcm[pcm.size() - 1 - i] *= g;
    }
}

} // namespace veyra::ui::dub
