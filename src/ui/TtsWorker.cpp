// SPDX-License-Identifier: GPL-3.0-only
#include "TtsWorker.h"
#include "AsrModels.h"
#include "SpeechVoice.h"
// The 1.x header on purpose: its TTS structs differ from the 2.x one used by
// AsrWorker, and this process loads the 1.x runtime (see AsrModels.cpp).
#include "../../third_party/sherpa-onnx-1.13/c-api.h"

#include <QByteArray>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <windows.h>
#include <mmreg.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

namespace veyra::ui::tts {
namespace {

#define VEYRA_TTS_SYMBOLS(F) \
    F(SherpaOnnxGetVersionStr) \
    F(SherpaOnnxCreateOfflineTts) \
    F(SherpaOnnxDestroyOfflineTts) \
    F(SherpaOnnxOfflineTtsGenerate) \
    F(SherpaOnnxDestroyOfflineTtsGeneratedAudio)

struct Runtime {
    HMODULE module = nullptr;
#define DECLARE(name) decltype(&::name) name = nullptr;
    VEYRA_TTS_SYMBOLS(DECLARE)
#undef DECLARE
    ~Runtime() { if (module) FreeLibrary(module); }

    bool load(const QString& path) {
        module = LoadLibraryExW(QDir::toNativeSeparators(path).toStdWString().c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return false;
#define BIND(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(module, #name)); if (!name) return false;
        VEYRA_TTS_SYMBOLS(BIND)
#undef BIND
        return QByteArrayView(SherpaOnnxGetVersionStr()) == asr::piperRuntimeVersion();
    }
};
#undef VEYRA_TTS_SYMBOLS

std::mutex outputMutex;
void writeLine(const QJsonObject& object) {
    const auto line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::lock_guard lock(outputMutex);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

// Piper (VITS) through sherpa-onnx: espeak-ng phonemes, then the ONNX model.
// Mono float samples at the model rate, played by SpeechVoice as IEEE float.
class PiperSynth final : public SpeechSynth {
public:
    PiperSynth(const Runtime& runtime, const SherpaOnnxOfflineTts* tts) : runtime_(runtime), tts_(tts) {}
    bool setLanguage(const std::wstring& language) override {
        // The installed voice is French; other languages fall back to Windows upstream.
        return language.substr(0, language.find(L'-')) == L"fr";
    }
    std::optional<SpeechAudio> synthesize(const std::wstring& text, double rate) override {
        const auto utf8 = QString::fromStdWString(text).toUtf8();
        const auto* audio = runtime_.SherpaOnnxOfflineTtsGenerate(tts_, utf8.constData(), 0, float(rate));
        if (!audio) return std::nullopt;
        SpeechAudio out;
        if (audio->n > 0 && audio->sample_rate > 0) {
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            format.nChannels = 1;
            format.nSamplesPerSec = DWORD(audio->sample_rate);
            format.wBitsPerSample = 32;
            format.nBlockAlign = 4;
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
            const auto* raw = reinterpret_cast<const uint8_t*>(&format);
            out.format.assign(raw, raw + sizeof(format));
            const auto* samples = reinterpret_cast<const uint8_t*>(audio->samples);
            out.pcm.assign(samples, samples + size_t(audio->n) * sizeof(float));
        }
        runtime_.SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        if (out.pcm.empty()) return std::nullopt;
        return out;
    }
private:
    const Runtime& runtime_;
    const SherpaOnnxOfflineTts* tts_;
};

int fail(const char* reason, unsigned long code = 0) {
    writeLine({{"error", QString::fromLatin1(reason)}, {"code", qint64(code)}});
    return 2;
}
} // namespace

int runWorker() {
    std::string line;
    if (!std::getline(std::cin, line)) return 2;
    const auto config = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    const QString folder = config.value(QStringLiteral("folder")).toString();
    const int threads = std::clamp(config.value(QStringLiteral("threads")).toInt(2), 1, 4);
    const auto bundle = asr::piperAssets();
    if (!QDir::isAbsolutePath(folder) || !std::ranges::all_of(bundle, [&](const ModelAsset& asset) {
            return ModelAssets::verify(folder, asset, true);
        }))
        return fail("asset-verification");

    Runtime runtime;
    const QDir runtimeDir(ModelAssets::path(folder, bundle[0]));
    if (!runtime.load(runtimeDir.filePath(QStringLiteral("sherpa-onnx-c-api.dll"))))
        return fail("runtime-load", GetLastError());

    const QDir voiceDir(ModelAssets::path(folder, bundle[1]));
    const auto& files = bundle[1].members;
    const auto model = voiceDir.filePath(QString::fromLatin1(files[0].name)).toUtf8();
    const auto tokens = voiceDir.filePath(QString::fromLatin1(files[1].name)).toUtf8();
    const auto dataDir = voiceDir.filePath(QStringLiteral("espeak-ng-data")).toUtf8();
    SherpaOnnxOfflineTtsConfig ttsConfig{};
    ttsConfig.model.vits.model = model.constData();
    ttsConfig.model.vits.tokens = tokens.constData();
    ttsConfig.model.vits.data_dir = dataDir.constData();
    ttsConfig.model.num_threads = threads;
    ttsConfig.model.provider = "cpu";
    ttsConfig.max_num_sentences = 1;
    const SherpaOnnxOfflineTts* tts = runtime.SherpaOnnxCreateOfflineTts(&ttsConfig);
    if (!tts) return fail("model-load");

    int code = 0;
    {
        SpeechVoice voice([&runtime, tts] { return std::make_unique<PiperSynth>(runtime, tts); },
                          [](bool speaking) { writeLine({{"speaking", speaking}}); });
        voice.setLanguage(L"fr");
        writeLine({{"ready", true}});
        while (std::getline(std::cin, line)) {
            const auto message = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
            if (message.contains(QStringLiteral("speak")))
                voice.speak(message.value(QStringLiteral("speak")).toString().toStdWString());
            else if (message.contains(QStringLiteral("cancel")))
                voice.cancel();
            else if (message.contains(QStringLiteral("volume")))
                voice.setVolume(float(message.value(QStringLiteral("volume")).toDouble(1.0)));
        }
    } // joins the voice thread before the model goes away
    runtime.SherpaOnnxDestroyOfflineTts(tts);
    return code;
}

} // namespace veyra::ui::tts
