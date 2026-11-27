// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Live dubbing ("audio filters"): speaks each translated sentence from
// LiveTranslation and drives the renderer's voice filter, which lowers the
// original dialogue and mixes this voice (veyra/sink/VoiceFilter.h).
//
// A sentence is spoken only after it has been recognised and translated, so
// the translated voice follows the original by roughly 1-3 s. Live game
// video is never delayed to hide that; stale sentences are skipped instead.
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QStringList>
#include <QTimer>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace veyra::ui {
class LiveTranslation;

class DubVoice final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(double balance READ balance WRITE setBalance NOTIFY changed)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY changed)
    Q_PROPERTY(int provider READ provider WRITE setProvider NOTIFY changed)
    Q_PROPERTY(QStringList windowsVoices READ windowsVoices NOTIFY voicesChanged)
    Q_PROPERTY(int windowsVoice READ windowsVoice WRITE setWindowsVoice NOTIFY changed)
    Q_PROPERTY(QString openAiVoice READ openAiVoice WRITE setOpenAiVoice NOTIFY changed)
    Q_PROPERTY(QString openAiModel READ openAiModel WRITE setOpenAiModel NOTIFY changed)
    Q_PROPERTY(bool hasApiKey READ hasApiKey NOTIFY changed)
    Q_PROPERTY(int reduction READ reduction NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int spoken READ spoken NOTIFY statusChanged)
    Q_PROPERTY(int skipped READ skipped NOTIFY statusChanged)
    Q_PROPERTY(double averageDelayMs READ averageDelayMs NOTIFY statusChanged)
public:
    enum Provider { WindowsVoice, OpenAi };
    DubVoice(const QString& directory, LiveTranslation* translation, QObject* parent = nullptr);
    ~DubVoice() override;

    bool enabled() const { return enabled_; }
    double balance() const { return balance_; }
    double volume() const { return volume_; }
    int provider() const { return provider_; }
    QStringList windowsVoices() const { return voiceNames_; }
    int windowsVoice() const { return windowsVoice_; }
    QString openAiVoice() const { return openAiVoice_; }
    QString openAiModel() const { return openAiModel_; }
    bool hasApiKey() const { return !apiKey_.isEmpty(); }
    int reduction() const { return reduction_; }
    QString status() const { return status_; }
    int spoken() const { return spoken_; }
    int skipped() const { return skipped_; }
    double averageDelayMs() const { return averageDelayMs_; }

    void setEnabled(bool value);
    void setBalance(double value);
    void setVolume(double value);
    void setProvider(int value);
    void setWindowsVoice(int value);
    void setOpenAiVoice(QString value);
    void setOpenAiModel(QString value);
    // Kept in memory only, like the translation key.
    Q_INVOKABLE void setApiKey(const QString& value);
    Q_INVOKABLE void preview();

    // From LiveTranslation (UI thread).
    void speak(const QString& text, const QString& language, qint64 endUs);
    void reset();

signals:
    void changed();
    void statusChanged();
    void voicesChanged();

private:
    struct Pending {
        quint64 id = 0;
        QString text;
        QString language;
        qint64 endUs = 0;
        bool preview = false;
    };
    class WindowsSynth;

    void applyFilter();
    void setStatus(QString value);
    void pump();
    void requestOpenAi(const Pending& item);
    void deliver(quint64 id, std::vector<float> pcm48, const QString& error);
    void fail(QString message);
    void refresh();

    QPointer<LiveTranslation> translation_;
    QSettings settings_;
    std::unique_ptr<WindowsSynth> windows_;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 errorUntil_ = 0;
    bool enabled_ = false;
    double balance_ = 0.75, volume_ = 1.0;
    int provider_ = WindowsVoice, windowsVoice_ = -1, reduction_ = 0;
    QStringList voiceNames_, voiceIds_, voiceLanguages_;
    QString openAiVoice_, openAiModel_, status_;
    QByteArray apiKey_;
    std::deque<Pending> pending_;
    std::optional<Pending> current_; // being synthesised
    quint64 nextId_ = 0, generation_ = 1;
    int spoken_ = 0, skipped_ = 0;
    double averageDelayMs_ = 0;
};
} // namespace veyra::ui
