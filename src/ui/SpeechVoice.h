// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Reads translated captions aloud.
//
// SpeechVoice owns the queue, the pacing policy and the playback: each line is
// synthesized by a SpeechSynth on the voice thread and played on its own
// shared-mode WASAPI stream to the default output. That stream is not the
// AudioRenderer, so the voice never reaches the recording tap that feeds
// speech recognition (no feedback loop) and never shifts the A/V clock.
//
// Two synths exist: the Windows one (Windows.Media.SpeechSynthesis, OneCore
// voices, in this process) and Piper (sherpa-onnx, in the --tts-worker process
// because onnxruntime.dll must not be loaded next to the player's own).
//
// Plain C++ on purpose: WinRT headers stay out of Qt translation units.
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace veyra::ui {

struct SpeechAudio {
    std::vector<uint8_t> format; // WAVEFORMATEX (+ extension), any PCM or IEEE float layout
    std::vector<uint8_t> pcm;
};

class SpeechSynth {
public:
    virtual ~SpeechSynth() = default;
    // False when no voice exists for this BCP-47 language.
    virtual bool setLanguage(const std::wstring& language) = 0;
    // rate: 1 = normal speed.
    virtual std::optional<SpeechAudio> synthesize(const std::wstring& text, double rate) = 0;
};

// Built on the voice thread, which has joined the multi-threaded apartment.
std::unique_ptr<SpeechSynth> makeWindowsSynth();

class SpeechVoice {
public:
    // Called on the voice thread whenever playback starts or the queue runs dry.
    using SpeakingChanged = std::function<void(bool speaking)>;
    using SynthFactory = std::function<std::unique_ptr<SpeechSynth>()>;

    SpeechVoice(SynthFactory factory, SpeakingChanged onSpeaking);
    ~SpeechVoice();
    SpeechVoice(const SpeechVoice&) = delete;
    SpeechVoice& operator=(const SpeechVoice&) = delete;

    // Display name of the Windows voice used for a BCP-47 language
    // ("fr", "fr-FR", "zh-CN"...), empty when Windows has none installed.
    static std::wstring windowsVoiceNameFor(const std::wstring& language);

    void setLanguage(std::wstring language);
    // 0..1, applied to the voice stream only.
    void setVolume(float volume);
    // Queues one line. While a line is playing at most two wait; a newer line
    // replaces the oldest waiting one so the voice stays close to the picture.
    void speak(std::wstring text);
    // Drops everything queued and stops the line being played.
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace veyra::ui
