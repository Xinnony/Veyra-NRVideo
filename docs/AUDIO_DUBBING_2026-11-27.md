# 音频滤镜：实时配音与原声人声压低（2026-11-27，未构建）

用户需求：像视频滤镜一样给音频加滤镜，把 GTA6 的英语对白实时翻译成法语并朗读，同时压低已被翻译的英语人声（不压低车辆、枪声、漂移、动物、警笛等音效），用一个滑块在原声和配音之间调节。

## 用户确认的选择

- 配音语音：本机免费 + OpenAI 可选。
- 原声压低：多声道用中置声道，立体声用“AI”方法（见下方偏差说明）。
- 允许编译和测试（本机实际无法编译，见“验证”）。

## 结构

1. **识别与翻译沿用现有链路**：`AsrAudioTap` → 本地 Parakeet/Whisper → `LiveTranslation`（本地 / OpenAI / MyMemory）。`LiveTranslation::queueSpeechResult` 新增 `speechTranslated(text, language, endUs)` 信号，只在原文与目标语言不同时发出；`resetCaption()` 发出 `speechReset()`。
2. **`src/ui/DubVoice.{h,cpp}`**（QML：`veyra.dubVoice`）：每句译文合成语音。
   - Windows 语音：WinRT `SpeechSynthesizer`（OneCore，离线），独立 MTA 线程，WAV → 单声道 → 48 kHz。自动模式只选译文语言的语音，没有就报错，不用英语语音读法语。
   - OpenAI：`POST https://api.openai.com/v1/audio/speech`，`response_format=pcm`（24 kHz s16le 单声道），默认 `gpt-4o-mini-tts` / `coral`。密钥只存内存。ChatGPT 订阅不包含 API 额度，界面写明。
   - 有界：待合成最多 3 句，原声结束超过 6 s 的句子跳过，已排队配音超过 4 s 时丢弃新句，不越积越晚。
3. **`include/veyra/sink/VoiceFilter.h` + `src/sink/VoiceFilter.cpp`**：渲染器内的音频滤镜，挂在 `AudioRenderer::pumpOnce` 的录制/ASR 抽头**之后**、用户音量之前。识别和录制仍拿到原声，避免把法语配音再识别一遍。音频线程上不分配内存（配音缓冲按端点块大小只增长一次）、不加锁、不等待。
   - 多声道且有前中置：只把中置声道乘以原声增益（50 ms 斜坡），不增加延迟。
   - 立体声：STFT（1024 点、跳 256、sqrt-Hann WOLA）按左右声道相干性估计居中成分，限 80 Hz–9 kHz 人声频段，并从左右声道减去。固定延迟 1024 帧（21.3 ms），其他声道同步延迟。
   - 配音：单生产者/单消费者无锁环形队列（20 s），一次只由一个渲染器消费。5.1 混入中置，立体声 ±3 dB 混入左右，单声道直接相加；混入后走已有的峰值限幅。
   - 滑块 `balance`：0 只有原声人声，0.5 两者全音量，1 只有配音。`originalVoiceGain = clamp(2(1-b))`，`dubVoiceGain = clamp(2b)`。
4. **界面**：`qml/Veyra/AudioFiltersDialog.qml`（key `audioFilters`）。音轨/声音菜单新增“音频滤镜…”。`i18n/catalog.json` 新增 30 条，9 种语言补齐。设置保存在数据目录的 `live-dubbing.ini`，“启用”不在启动时恢复。

## 与约定的偏差（如实说明）

- **立体声不是 AI 分离**：本机没有可用的分离模型，下载模型需要另行授权并审计来源、许可和哈希，所以先用确定性的 DSP 估计。居中的音效和音乐人声也会在人声频段被削弱，偏离中心的对白只削弱一部分（Python 移植测试：左 1 / 右 0.6 的对白剩 38%）。界面写明“不是 AI 分离”。
- **本机语音用 Windows 自带语音，不是 Piper**：Piper/sherpa-onnx TTS 需要换成带 TTS 的 sherpa 运行库并下载法语模型，涉及下载授权、逐文件哈希和许可审计。Windows 语音无需下载即可使用。
- 立体声模式给声音增加 21.3 ms 延迟，没有向音画同步时钟报告。录制文件和虚拟摄像机的音频不含配音，也不压低原声（抽头在滤镜之前）。

## 验证

- 本机（`G:\Veyra-NRVideo`）没有 Qt、FFmpeg、Veyra 依赖、CMake、MSVC 头文件和 Windows SDK，AGENTS.md 中的 `E:/项目/Veyra` 路径也不存在。**未编译任何 C++，未运行任何 C++ 测试，未在真机上听过。**
- 用纯 Python 逐行移植了立体声 STFT 路径和重采样器（草稿目录，不入库）：
  - 不削弱时完美重建，延迟正好 1024 帧，最大误差 4.4e-16；
  - 居中 1 kHz 剩 6.3e-5；左声道独有的 1 kHz 不变（1.0）；居中 45 Hz 剩 0.991；
  - 24 kHz → 48 kHz：长度 48000，峰值 0.5，RMS 0.35358（理论 0.35355），过零 1833 次（理论 1833）。
- `python -B scripts/i18n/extract.py --check`：3362 条，9 种语言缺译均为 0，占位符问题 0。
- 新增 CTest：`veyra_voice_filter`（`tests/unit/VoiceFilterTests.cpp`）、`veyra_dub_audio`（`tests/unit/DubAudioTests.cpp`），都不依赖 Qt 和音频硬件。

## 构建机上待做

1. 编译 `veyra_sinks`、`veyra_qml_bridge`（新增链接 `windowsapp`）、`veyra_voice_filter_tests`、`veyra_dub_audio_tests`，运行两个新测试和既有 `veyra_audio_gain_tests`、`veyra_live_translation_tests`。
2. 实测：PS5/采集卡立体声和 5.1 各一次，确认对白被压低、车辆/枪声保留，滑块两端和中点，Windows 法语语音和 OpenAI 语音，暂停/拖动进度/切源时配音清空，单句延迟统计。
3. 如需真正的 AI 分离（立体声），选定模型（如 DeepFilterNet 一类实时模型）并取得下载授权后，替换 `VoiceFilter::stereoFrame` 中的相干性掩码。
