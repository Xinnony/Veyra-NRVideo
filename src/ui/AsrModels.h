// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "TranslationAssets.h"

namespace veyra::ui {
namespace asr {

inline constexpr auto runtimeVersion = "2.0.2";
inline constexpr int sampleRate = 16000;
inline constexpr int inputRate = 48000;
inline constexpr int modelCount = 2;

enum Model { Parakeet, Whisper };
QString root(const QString& dataDirectory);
std::span<const ModelAsset> assets(int model);
// [0] sherpa-onnx 1.x runtime, [1] Piper French voice, for reading translations
// aloud (same root folder as ASR, separate runtime).
std::span<const ModelAsset> piperAssets();
const char* piperRuntimeVersion();
bool verify(const QString& root, int model, bool contents);
QString whisperLanguage(const QString& language);

}
}
