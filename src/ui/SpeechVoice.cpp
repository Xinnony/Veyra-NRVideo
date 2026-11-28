// SPDX-License-Identifier: GPL-3.0-only
#include "SpeechVoice.h"

#include "veyra/Log.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace veyra::ui {
namespace {
using namespace winrt::Windows::Media::SpeechSynthesis;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

// A line older than this when its turn comes is skipped: hearing it after the
// scene has moved on is worse than not hearing it.
constexpr auto kMaxLineAge = std::chrono::seconds(8);
constexpr size_t kMaxWaiting = 2;
constexpr size_t kMaxTextChars = 400;

std::wstring lower(std::wstring s) {
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}
std::wstring primary(const std::wstring& tag) { return tag.substr(0, tag.find(L'-')); }

// Exact tag first (fr-FR), then the same primary language (fr -> fr-FR, fr-CA).
std::optional<VoiceInformation> findVoice(const std::wstring& language) {
    if (language.empty()) return std::nullopt;
    const auto wanted = lower(language);
    std::optional<VoiceInformation> sameLanguage;
    for (const auto& voice : SpeechSynthesizer::AllVoices()) {
        const auto tag = lower(std::wstring(voice.Language()));
        if (tag == wanted) return voice;
        if (!sameLanguage && primary(tag) == primary(wanted)) sameLanguage = voice;
    }
    return sameLanguage;
}

std::string narrow(const std::wstring& s) { return winrt::to_string(s); }

struct Wav {
    std::vector<uint8_t> format; // WAVEFORMATEX (+ extension)
    std::vector<uint8_t> pcm;
};

// SynthesizeTextToStreamAsync returns a RIFF/WAVE file; keep fmt and data.
std::optional<Wav> parseWav(const std::vector<uint8_t>& file) {
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) || std::memcmp(file.data() + 8, "WAVE", 4))
        return std::nullopt;
    Wav wav;
    for (size_t at = 12; at + 8 <= file.size();) {
        uint32_t size = 0;
        std::memcpy(&size, file.data() + at + 4, 4);
        const size_t body = at + 8;
        const size_t available = std::min<size_t>(size, file.size() - body);
        if (!std::memcmp(file.data() + at, "fmt ", 4)) wav.format.assign(file.begin() + body, file.begin() + body + available);
        else if (!std::memcmp(file.data() + at, "data", 4)) wav.pcm.assign(file.begin() + body, file.begin() + body + available);
        at = body + size + (size & 1);
    }
    if (wav.format.size() < sizeof(PCMWAVEFORMAT) || wav.pcm.empty()) return std::nullopt;
    if (wav.format.size() < sizeof(WAVEFORMATEX)) wav.format.resize(sizeof(WAVEFORMATEX), 0);
    return wav;
}
} // namespace

struct SpeechVoice::Impl {
    struct Line {
        std::wstring text;
        Clock::time_point queuedAt;
    };

    SpeakingChanged onSpeaking;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Line> waiting;
    std::wstring language;
    std::atomic<float> volume{1.0f};
    std::atomic<uint64_t> generation{0};
    bool stop = false, speaking = false;
    std::thread worker;

    void setSpeaking(bool value) {
        if (speaking == value) return;
        speaking = value;
        if (onSpeaking) onSpeaking(value);
    }

    void run() {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
        SpeechSynthesizer synth{nullptr};
        std::wstring voiceLanguage;
        bool haveVoice = false, missingLogged = false;
        for (;;) {
            Line line;
            std::wstring wantedLanguage;
            uint64_t lineGeneration = 0;
            size_t stillWaiting = 0;
            {
                std::unique_lock lock(mutex);
                if (waiting.empty()) setSpeaking(false);
                wake.wait(lock, [this] { return stop || !waiting.empty(); });
                if (stop) break;
                line = std::move(waiting.front());
                waiting.pop_front();
                stillWaiting = waiting.size();
                wantedLanguage = language;
                lineGeneration = generation.load();
            }
            if (Clock::now() - line.queuedAt > kMaxLineAge) continue;
            try {
                if (!synth) synth = SpeechSynthesizer();
                if (wantedLanguage != voiceLanguage) {
                    voiceLanguage = wantedLanguage;
                    const auto voice = findVoice(voiceLanguage);
                    haveVoice = voice.has_value();
                    missingLogged = false;
                    if (voice) {
                        synth.Voice(*voice);
                        log::info("live-voice", std::format("voice={} language={}", winrt::to_string(voice->DisplayName()),
                            winrt::to_string(voice->Language())));
                    }
                }
                if (!haveVoice) {
                    if (!missingLogged) log::warn("live-voice", std::format("no Windows voice for {}", narrow(voiceLanguage)));
                    missingLogged = true;
                    continue;
                }
                // A little faster than normal, more when lines are waiting, so the
                // voice keeps pace with dialogue instead of drifting behind it.
                synth.Options().SpeakingRate(stillWaiting ? 1.3 : 1.1);
                const auto stream = synth.SynthesizeTextToStreamAsync(winrt::hstring(line.text)).get();
                const auto size = uint32_t(stream.Size());
                winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
                reader.LoadAsync(size).get();
                std::vector<uint8_t> file(size);
                reader.ReadBytes(file);
                if (generation.load() != lineGeneration) continue;
                const auto wav = parseWav(file);
                if (!wav) { log::warn("live-voice", "speech synthesis returned no PCM"); continue; }
                {
                    std::lock_guard lock(mutex);
                    setSpeaking(true);
                }
                play(*wav, lineGeneration);
            } catch (const winrt::hresult_error& e) {
                log::warn("live-voice", std::format("speech synthesis failed hr=0x{:08x} {}", uint32_t(e.code()),
                    winrt::to_string(e.message())));
            } catch (...) {
                log::warn("live-voice", "speech synthesis failed");
            }
        }
        std::lock_guard lock(mutex);
        setSpeaking(false);
    }

    // One short-lived shared stream per line: the default output may change
    // between lines (headset plugged in), and opening it costs a few ms.
    void play(const Wav& wav, uint64_t lineGeneration) {
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioStreamVolume> streamVolume;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
            FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            log::warn("live-voice", "no default audio output");
            return;
        }
        const auto* format = reinterpret_cast<const WAVEFORMATEX*>(wav.format.data());
        constexpr REFERENCE_TIME kBuffer = 2'000'000; // 200 ms
        const HRESULT init = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, kBuffer, 0, format, nullptr);
        UINT32 bufferFrames = 0;
        if (FAILED(init) || FAILED(client->GetBufferSize(&bufferFrames)) ||
            FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
            log::warn("live-voice", std::format("audio output init failed hr=0x{:08x}", uint32_t(init)));
            return;
        }
        const UINT32 frameBytes = format->nBlockAlign;
        if (!frameBytes) return;
        const UINT32 totalFrames = UINT32(wav.pcm.size() / frameBytes);
        float appliedVolume = -1.0f;
        UINT32 channels = 0;
        if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&streamVolume)))) streamVolume->GetChannelCount(&channels);
        auto applyVolume = [&] {
            const float wanted = std::clamp(volume.load(), 0.0f, 1.0f);
            if (!streamVolume || !channels || wanted == appliedVolume) return;
            std::vector<float> levels(channels, wanted);
            streamVolume->SetAllVolumes(channels, levels.data());
            appliedVolume = wanted;
        };
        applyVolume();
        UINT32 written = 0;
        bool started = false;
        while (written < totalFrames && generation.load() == lineGeneration) {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            const UINT32 frames = std::min(bufferFrames - padding, totalFrames - written);
            if (frames) {
                BYTE* data = nullptr;
                if (FAILED(render->GetBuffer(frames, &data))) break;
                std::memcpy(data, wav.pcm.data() + size_t(written) * frameBytes, size_t(frames) * frameBytes);
                render->ReleaseBuffer(frames, 0);
                written += frames;
            }
            if (!started) { client->Start(); started = true; }
            applyVolume();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Let the tail play out unless the line was cancelled.
        for (int i = 0; i < 100 && started && generation.load() == lineGeneration; ++i) {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)) || !padding) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (started) client->Stop();
    }
};

SpeechVoice::SpeechVoice(SpeakingChanged onSpeaking) : p_(std::make_unique<Impl>()) {
    p_->onSpeaking = std::move(onSpeaking);
}

SpeechVoice::~SpeechVoice() {
    {
        std::lock_guard lock(p_->mutex);
        p_->stop = true;
        p_->waiting.clear();
        ++p_->generation;
    }
    p_->wake.notify_all();
    if (p_->worker.joinable()) p_->worker.join();
}

std::wstring SpeechVoice::voiceNameFor(const std::wstring& language) {
    try {
        const auto voice = findVoice(language);
        return voice ? std::wstring(voice->DisplayName()) : std::wstring{};
    } catch (...) {
        return {};
    }
}

void SpeechVoice::setLanguage(std::wstring language) {
    std::lock_guard lock(p_->mutex);
    p_->language = std::move(language);
}

void SpeechVoice::setVolume(float volume) { p_->volume = std::clamp(volume, 0.0f, 1.0f); }

void SpeechVoice::speak(std::wstring text) {
    if (text.empty()) return;
    if (text.size() > kMaxTextChars) text.resize(kMaxTextChars);
    {
        std::lock_guard lock(p_->mutex);
        if (p_->stop) return;
        while (p_->waiting.size() >= kMaxWaiting) p_->waiting.pop_front();
        p_->waiting.push_back({std::move(text), Clock::now()});
        if (!p_->worker.joinable()) p_->worker = std::thread([impl = p_.get()] { impl->run(); });
    }
    p_->wake.notify_one();
}

void SpeechVoice::cancel() {
    std::lock_guard lock(p_->mutex);
    p_->waiting.clear();
    ++p_->generation;
}

} // namespace veyra::ui
