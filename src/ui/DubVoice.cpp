// SPDX-License-Identifier: GPL-3.0-only
#include "DubVoice.h"

#include "DubAudio.h"
#include "LiveTranslation.h"
#include "veyra/Log.h"
#include "veyra/sink/RecordingAudioTap.h"
#include "veyra/sink/VoiceFilter.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <condition_variable>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace veyra::ui {
namespace {
constexpr qint64 maxLagUs = 6'000'000;     // older sentences are no longer worth speaking
constexpr double maxQueuedSeconds = 4.0;   // translated voice not yet played
constexpr size_t maxPending = 3;
const QString openAiSpeechUrl = QStringLiteral("https://api.openai.com/v1/audio/speech");

QString settingKey(const char* name) { return QLatin1String(name); }
}

// Windows speech synthesis (OneCore voices, offline). WinRT runs in its own
// multithreaded apartment; the UI thread only queues text and receives PCM.
class DubVoice::WindowsSynth {
public:
    struct Voice { QString name, id, language; };
    using Voices = std::function<void(std::vector<Voice>)>;
    using Done = std::function<void(quint64, std::vector<float>, QString)>;

    WindowsSynth(Voices voices, Done done) : voices_(std::move(voices)), done_(std::move(done)), thread_([this] { run(); }) {}
    ~WindowsSynth() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    void submit(quint64 id, QString text, QString voiceId) {
        {
            std::lock_guard lock(mutex_);
            job_ = Job{id, text.toStdWString(), voiceId.toStdWString()};
        }
        wake_.notify_all();
    }

private:
    struct Job { quint64 id = 0; std::wstring text, voice; };
    void run() {
        using namespace winrt::Windows::Media::SpeechSynthesis;
        using namespace winrt::Windows::Storage::Streams;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error&) {
            // Already initialised as MTA on this thread: fine.
        }
        std::vector<Voice> list;
        std::optional<SpeechSynthesizer> synth;
        try {
            for (const auto& voice : SpeechSynthesizer::AllVoices())
                list.push_back({QString::fromWCharArray(voice.DisplayName().c_str()),
                                QString::fromWCharArray(voice.Id().c_str()),
                                QString::fromWCharArray(voice.Language().c_str())});
            synth.emplace();
        } catch (const winrt::hresult_error& e) {
            log::warn("dub", std::format("Windows speech synthesis unavailable hr=0x{:08X}", unsigned(e.code().value)));
        }
        voices_(list);
        std::wstring selected;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || job_; });
                if (stop_) break;
                job = std::move(*job_);
                job_.reset();
            }
            if (!synth) {
                done_(job.id, {}, QStringLiteral("SpeechSynthesizer"));
                continue;
            }
            try {
                if (job.voice != selected) {
                    for (const auto& voice : SpeechSynthesizer::AllVoices())
                        if (voice.Id() == job.voice) { synth->Voice(voice); break; }
                    selected = job.voice;
                }
                // Dialogue lines must keep up with the game: slightly faster than default.
                try { synth->Options().SpeakingRate(1.12); } catch (const winrt::hresult_error&) {}
                const auto stream = synth->SynthesizeTextToStreamAsync(winrt::hstring(job.text)).get();
                const auto size = uint32_t(stream.Size());
                DataReader reader(stream);
                reader.LoadAsync(size).get();
                std::vector<uint8_t> bytes(size);
                reader.ReadBytes(bytes);
                const auto wave = dub::parseWave(bytes.data(), bytes.size());
                if (!wave) {
                    done_(job.id, {}, QStringLiteral("WAVE"));
                    continue;
                }
                done_(job.id, dub::resampleTo48k(wave->samples, wave->rate), {});
            } catch (const winrt::hresult_error& e) {
                done_(job.id, {}, QString::fromWCharArray(e.message().c_str()).left(200));
            }
        }
        synth.reset();
    }

    Voices voices_;
    Done done_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Job> job_;
    bool stop_ = false;
    std::thread thread_;
};

DubVoice::DubVoice(const QString& directory, LiveTranslation* translation, QObject* parent)
    : QObject(parent), translation_(translation),
      settings_(QDir(directory).filePath(QStringLiteral("live-dubbing.ini")), QSettings::IniFormat),
      network_(new QNetworkAccessManager(this)) {
    clock_.start();
    balance_ = std::clamp(settings_.value(settingKey("balance"), 0.75).toDouble(), 0.0, 1.0);
    volume_ = std::clamp(settings_.value(settingKey("volume"), 1.0).toDouble(), 0.0, double(sink::kMaxDubGain));
    provider_ = std::clamp(settings_.value(settingKey("provider"), int(WindowsVoice)).toInt(), 0, 1);
    openAiVoice_ = settings_.value(settingKey("openAiVoice"), QStringLiteral("coral")).toString().left(40);
    openAiModel_ = settings_.value(settingKey("openAiModel"), QStringLiteral("gpt-4o-mini-tts")).toString().left(80);
    // Not restored as enabled: dubbing starts only when the user turns it on.
    applyFilter();
    windows_ = std::make_unique<WindowsSynth>(
        [this](std::vector<WindowsSynth::Voice> voices) {
            QMetaObject::invokeMethod(this, [this, voices = std::move(voices)] {
                voiceNames_.clear(); voiceIds_.clear(); voiceLanguages_.clear();
                const auto saved = settings_.value(settingKey("windowsVoiceId")).toString();
                windowsVoice_ = -1;
                for (const auto& v : voices) {
                    if (v.id == saved) windowsVoice_ = int(voiceIds_.size());
                    voiceNames_ << QStringLiteral("%1 (%2)").arg(v.name, v.language);
                    voiceIds_ << v.id;
                    voiceLanguages_ << v.language;
                }
                emit voicesChanged();
                emit changed();
            }, Qt::QueuedConnection);
        },
        [this](quint64 id, std::vector<float> pcm, QString error) {
            QMetaObject::invokeMethod(this, [this, id, pcm = std::move(pcm), error] {
                deliver(id, pcm, error);
            }, Qt::QueuedConnection);
        });
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &DubVoice::refresh);
}

DubVoice::~DubVoice() {
    timer_.stop();
    if (reply_) reply_->abort();
    sink::setVoiceFilterSettings({false, float(balance_), float(volume_)});
    sink::dubVoiceQueue().clear();
    windows_.reset(); // joins the synthesis thread before members go away
}

void DubVoice::applyFilter() {
    sink::setVoiceFilterSettings({enabled_, float(balance_), float(volume_)});
}

void DubVoice::setStatus(QString value) {
    if (status_ == value) return;
    status_ = std::move(value);
    emit statusChanged();
}

void DubVoice::setEnabled(bool value) {
    if (enabled_ == value) return;
    enabled_ = value;
    reset();
    applyFilter();
    if (value) {
        timer_.start();
        refresh();
    } else {
        timer_.stop();
        setStatus({});
    }
    log::info("dub", std::format("enabled={} provider={} balance={:.2f} volume={:.2f}", value, provider_, balance_, volume_));
    emit changed();
}

void DubVoice::setBalance(double value) {
    value = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
    if (qFuzzyCompare(balance_ + 1, value + 1)) return;
    balance_ = value;
    settings_.setValue(settingKey("balance"), balance_);
    applyFilter();
    emit changed();
}

void DubVoice::setVolume(double value) {
    value = std::isfinite(value) ? std::clamp(value, 0.0, double(sink::kMaxDubGain)) : 0.0;
    if (qFuzzyCompare(volume_ + 1, value + 1)) return;
    volume_ = value;
    settings_.setValue(settingKey("volume"), volume_);
    applyFilter();
    emit changed();
}

void DubVoice::setProvider(int value) {
    value = std::clamp(value, 0, 1);
    if (provider_ == value) return;
    provider_ = value;
    settings_.setValue(settingKey("provider"), provider_);
    reset();
    emit changed();
}

void DubVoice::setWindowsVoice(int value) {
    if (value < -1 || value >= voiceIds_.size() || windowsVoice_ == value) return;
    windowsVoice_ = value;
    settings_.setValue(settingKey("windowsVoiceId"), value >= 0 ? voiceIds_[value] : QString{});
    emit changed();
}

void DubVoice::setOpenAiVoice(QString value) {
    value = value.trimmed().left(40);
    if (value.isEmpty() || openAiVoice_ == value) return;
    openAiVoice_ = std::move(value);
    settings_.setValue(settingKey("openAiVoice"), openAiVoice_);
    emit changed();
}

void DubVoice::setOpenAiModel(QString value) {
    value = value.trimmed().left(80);
    if (value.isEmpty() || openAiModel_ == value) return;
    openAiModel_ = std::move(value);
    settings_.setValue(settingKey("openAiModel"), openAiModel_);
    emit changed();
}

void DubVoice::setApiKey(const QString& value) {
    if (value.contains(QLatin1Char('\r')) || value.contains(QLatin1Char('\n'))) return;
    const auto key = value.trimmed().left(2048).toUtf8();
    if (apiKey_ == key) return;
    apiKey_ = key;
    emit changed();
}

void DubVoice::preview() {
    if (!enabled_) return; // the renderer only plays the voice while the filter runs
    const bool french = translation_ && translation_->targetLanguage().startsWith(QLatin1String("fr"));
    Pending item;
    item.id = ++nextId_;
    item.text = french ? QStringLiteral("Ceci est un essai de la voix de doublage.")
                       : QStringLiteral("This is a test of the dubbing voice.");
    item.language = translation_ ? translation_->targetLanguage() : QString{};
    item.endUs = sink::recordingClockUs();
    item.preview = true;
    pending_.push_front(std::move(item));
    pump();
}

void DubVoice::speak(const QString& text, const QString& language, qint64 endUs) {
    if (!enabled_ || text.trimmed().isEmpty()) return;
    if (sink::recordingClockUs() - endUs > maxLagUs) {
        ++skipped_;
        emit statusChanged();
        return;
    }
    while (pending_.size() >= maxPending) {
        pending_.pop_front();
        ++skipped_;
    }
    pending_.push_back({++nextId_, text.trimmed().left(600), language, endUs, false});
    pump();
}

void DubVoice::reset() {
    ++generation_;
    pending_.clear();
    current_.reset();
    if (reply_) reply_->abort();
    reply_ = nullptr;
    sink::dubVoiceQueue().clear();
}

void DubVoice::pump() {
    if (current_ || pending_.empty()) return;
    if (!enabled_) {
        pending_.clear();
        return;
    }
    if (provider_ == OpenAi) {
        if (apiKey_.isEmpty()) {
            pending_.clear();
            fail(tr("请填写 OpenAI API 密钥"));
            return;
        }
        current_ = pending_.front();
        pending_.pop_front();
        requestOpenAi(*current_);
        return;
    }
    const auto& item = pending_.front();
    QString voice;
    if (windowsVoice_ >= 0 && windowsVoice_ < voiceIds_.size()) {
        voice = voiceIds_[windowsVoice_];
    } else {
        // Automatic: the first installed voice of the translation language.
        const auto prefix = item.language.section(QLatin1Char('-'), 0, 0);
        for (qsizetype i = 0; i < voiceLanguages_.size(); ++i)
            if (voiceLanguages_[i].startsWith(prefix, Qt::CaseInsensitive)) { voice = voiceIds_[i]; break; }
    }
    // No voice of the translation language: do not read French text with,
    // say, an English voice. The dialog explains how to add one.
    if (voice.isEmpty()) {
        pending_.clear();
        fail(tr("没有可用的 Windows 语音"));
        return;
    }
    current_ = pending_.front();
    pending_.pop_front();
    windows_->submit(current_->id, current_->text, voice);
}

void DubVoice::requestOpenAi(const Pending& item) {
    QNetworkRequest request{QUrl(openAiSpeechUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey_);
    request.setTransferTimeout(20000);
    QJsonObject body{{QStringLiteral("model"), openAiModel_},
                     {QStringLiteral("voice"), openAiVoice_},
                     {QStringLiteral("input"), item.text},
                     // Raw 24 kHz signed 16-bit little-endian mono.
                     {QStringLiteral("response_format"), QStringLiteral("pcm")}};
    if (openAiModel_.startsWith(QLatin1String("gpt-")))
        body.insert(QStringLiteral("instructions"),
                    QStringLiteral("Video game dub: natural, expressive, slightly fast delivery."));
    auto* reply = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    reply_ = reply;
    const auto generation = generation_;
    const auto id = item.id;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, id] {
        reply->deleteLater();
        if (reply_ == reply) reply_ = nullptr;
        if (generation != generation_) return;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || code != 200) {
            QString message = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("error")).toObject()
                                  .value(QStringLiteral("message")).toString();
            if (message.isEmpty()) message = reply->errorString();
            deliver(id, {}, message.left(200));
            return;
        }
        auto pcm = dub::pcm16ToFloat(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()));
        deliver(id, dub::resampleTo48k(pcm, 24000), {});
    });
}

void DubVoice::deliver(quint64 id, std::vector<float> pcm48, const QString& error) {
    if (!current_ || current_->id != id) return; // reset meanwhile
    const auto item = *current_;
    current_.reset();
    if (!error.isEmpty()) {
        fail(provider_ == OpenAi ? tr("OpenAI 语音请求失败：%1").arg(error) : tr("语音合成失败：%1").arg(error));
        log::warn("dub", std::format("synthesis failed provider={} error={}", provider_, error.toStdString()));
    } else if (!item.preview && sink::recordingClockUs() - item.endUs > maxLagUs) {
        ++skipped_;
    } else if (!item.preview && sink::dubVoiceQueue().queuedSeconds() > maxQueuedSeconds) {
        ++skipped_; // the voice is already behind: drop rather than drift further
    } else if (!pcm48.empty()) {
        dub::fadeEdges(pcm48);
        // The clip starts after the voice already queued ahead of it.
        const double aheadMs = sink::dubVoiceQueue().queuedSeconds() * 1000.0;
        if (sink::dubVoiceQueue().push(pcm48.data(), pcm48.size())) {
            if (!item.preview) {
                ++spoken_;
                const double delayMs = double(sink::recordingClockUs() - item.endUs) / 1000.0 + aheadMs;
                averageDelayMs_ = spoken_ == 1 ? delayMs : averageDelayMs_ * 0.8 + delayMs * 0.2;
            }
        } else {
            ++skipped_;
        }
    }
    emit statusChanged();
    pump();
}

void DubVoice::fail(QString message) {
    errorUntil_ = clock_.elapsed() + 8000; // keep the error readable before the normal status returns
    setStatus(std::move(message));
}

void DubVoice::refresh() {
    reduction_ = int(sink::lastVoiceReduction());
    if (!translation_ || !translation_->enabled() || translation_->recognitionSource() != LiveTranslation::VeyraAudio)
        setStatus(tr("请先开启实时翻译（识别来源：Veyra 音频）"));
    else if (clock_.elapsed() >= errorUntil_)
        setStatus(spoken_ ? tr("配音运行中") : tr("正在等待翻译…"));
    emit statusChanged();
}

} // namespace veyra::ui
