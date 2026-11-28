// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Reads translated captions aloud with a Windows speech voice
// (Windows.Media.SpeechSynthesis, the OneCore voices installed with a Windows
// language: nothing to download or bundle).
//
// The voice plays on its own shared-mode WASAPI stream to the default output,
// not through Veyra's AudioRenderer, so it never reaches the recording tap that
// feeds speech recognition (no feedback loop) and never shifts the A/V clock.
//
// Plain C++ on purpose: WinRT headers stay out of Qt translation units.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace veyra::ui {

class SpeechVoice {
public:
    // Called on the worker thread whenever playback starts or the queue runs dry.
    using SpeakingChanged = std::function<void(bool speaking)>;

    explicit SpeechVoice(SpeakingChanged onSpeaking);
    ~SpeechVoice();
    SpeechVoice(const SpeechVoice&) = delete;
    SpeechVoice& operator=(const SpeechVoice&) = delete;

    // Display name of the Windows voice used for a BCP-47 language
    // ("fr", "fr-FR", "zh-CN"...), empty when Windows has none installed.
    static std::wstring voiceNameFor(const std::wstring& language);

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
