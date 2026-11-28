// SPDX-License-Identifier: GPL-3.0-only
#include "PiperVoice.h"
#include "LocalWorkerJob.h"
#include "veyra/Log.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <format>
#include <utility>

namespace veyra::ui {

PiperVoice::PiperVoice(const QString& dataDirectory, QObject* parent)
    : QObject(parent), root_(asr::root(dataDirectory)), assets_(root_, asr::piperAssets()) {
    process_.setStandardErrorFile(QProcess::nullDevice());
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    connect(&assets_, &ModelAssets::changed, this, [this] {
        if (!assets_.downloading() && assets_.installed()) {
            failed_ = false;
            if (active_ && process_.state() == QProcess::NotRunning) start();
        }
        emit changed();
    });
    connect(&process_, &QProcess::started, this, [this] {
        if (!active_ || stopping_) { process_.kill(); return; }
        job_ = attachLocalWorkerJob(DWORD(process_.processId()));
        if (!job_) { stop(); failed_ = true; error_ = QStringLiteral("job"); emit changed(); return; }
        send({{"folder", root_}, {"threads", 2}});
        send({{"volume", double(volume_)}});
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &PiperVoice::readOutput);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !active_) return;
        failed_ = true;
        error_ = QStringLiteral("process-start");
        log::warn("live-voice", "piper worker failed to start");
        emit changed();
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (job_) CloseHandle(std::exchange(job_, nullptr));
        ready_ = false;
        emit speakingChanged(false);
        if (std::exchange(stopping_, false) || !active_) return;
        failed_ = true;
        if (error_.isEmpty()) error_ = QStringLiteral("exit-%1").arg(code);
        log::warn("live-voice", std::format("piper worker stopped: {}", error_.toStdString()));
        emit changed();
    });
}

PiperVoice::~PiperVoice() {
    process_.disconnect(this);
    active_ = false;
    stop();
    if (process_.state() != QProcess::NotRunning) process_.waitForFinished(1500);
}

QString PiperVoice::status() const {
    if (assets_.downloading() || (!assets_.installed() && !assets_.status().isEmpty())) return assets_.status();
    if (failed_) return tr("Piper 语音无法启动（%1）").arg(error_);
    return {};
}

void PiperVoice::download() {
    if (process_.state() == QProcess::NotRunning) assets_.download();
}

void PiperVoice::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    failed_ = false;
    error_.clear();
    if (active_) {
        assets_.refreshInstalled();
        if (assets_.installed()) start();
    } else {
        stop();
    }
    emit changed();
}

void PiperVoice::start() {
    if (process_.state() != QProcess::NotRunning) return;
    ready_ = false;
    stopping_ = false;
    lines_.clear();
    process_.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--tts-worker")});
    log::info("live-voice", "starting piper worker");
}

void PiperVoice::stop() {
    ready_ = false;
    if (job_) CloseHandle(std::exchange(job_, nullptr));
    if (process_.state() == QProcess::NotRunning) return;
    stopping_ = true;
    // Closing stdin ends the worker loop; kill as a backstop for a stuck synth.
    process_.closeWriteChannel();
    if (!process_.waitForFinished(300)) process_.kill();
}

void PiperVoice::send(const QJsonObject& message) {
    if (process_.state() != QProcess::Running) return;
    process_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void PiperVoice::readOutput() {
    lines_ += process_.readAllStandardOutput();
    for (qsizetype end; (end = lines_.indexOf('\n')) >= 0;) {
        const auto message = QJsonDocument::fromJson(lines_.first(end).trimmed()).object();
        lines_.remove(0, end + 1);
        if (message.contains(QStringLiteral("speaking"))) {
            emit speakingChanged(message.value(QStringLiteral("speaking")).toBool());
        } else if (message.contains(QStringLiteral("ready"))) {
            ready_ = true;
            log::info("live-voice", "piper voice ready");
        } else if (message.contains(QStringLiteral("error"))) {
            error_ = message.value(QStringLiteral("error")).toString();
            log::warn("live-voice", std::format("piper worker error={} code={}", error_.toStdString(),
                message.value(QStringLiteral("code")).toInteger()));
        }
    }
}

// Lines sent before the worker is ready wait in the pipe; the worker reads them
// once the model is loaded, and its age limit drops any that went stale.
void PiperVoice::speak(const QString& line) { send({{"speak", line}}); }
void PiperVoice::cancel() { send({{"cancel", true}}); }
void PiperVoice::setVolume(float volume) {
    volume_ = volume;
    send({{"volume", double(volume)}});
}

}
