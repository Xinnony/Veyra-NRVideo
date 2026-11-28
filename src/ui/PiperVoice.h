// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "AsrModels.h"

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <windows.h>
#include <algorithm>

namespace veyra::ui {
// The Piper French voice: its on-demand download (pinned sherpa-onnx 1.x
// runtime + voice, SHA-256 verified) and the --tts-worker process that
// synthesizes and plays each line. Started only while it is the active voice.
class PiperVoice final : public QObject {
    Q_OBJECT
public:
    explicit PiperVoice(const QString& dataDirectory, QObject* parent = nullptr);
    ~PiperVoice() override;

    bool installed() const { return assets_.installed(); }
    bool downloading() const { return assets_.downloading(); }
    double progress() const { return double(assets_.received()) / double(std::max<qint64>(1, assets_.totalBytes())); }
    qint64 downloadBytes() const { return assets_.totalBytes(); }
    QString status() const;
    void download();
    void cancelDownload() { assets_.cancel(); }

    void setActive(bool active);
    void speak(const QString& line);
    void cancel();
    void setVolume(float volume);

signals:
    void changed();
    void speakingChanged(bool speaking);

private:
    void start();
    void stop();
    void readOutput();
    void send(const QJsonObject& message);

    const QString root_;
    ModelAssets assets_;
    QProcess process_;
    HANDLE job_ = nullptr;
    QByteArray lines_;
    QString error_;
    float volume_ = 1.0f;
    bool active_ = false, ready_ = false, failed_ = false, stopping_ = false;
};
}
