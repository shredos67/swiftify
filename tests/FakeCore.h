#pragma once

#include "core/PlaybackCore.h"
#include "spotify/SpotifyError.h"

#include <swiftify_core.h>

#include <QCoroSignal>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace swiftify::testing {

/// an async call the test decides the outcome of
class PendingCall : public QObject {
    Q_OBJECT

public:
    PendingCall(QString name, QString argument)
        : name(std::move(name))
        , argument(std::move(argument))
    {
    }

    void succeed(const QString &payload = {}) { emit finished(true, false, payload); }
    void fail(const QString &message = QStringLiteral("failed")) { emit finished(false, false, message); }
    void cancel() { emit finished(false, true, {}); }

    const QString name;
    const QString argument;

signals:
    void finished(bool ok, bool cancelled, const QString &payload);
};

/// a core that records what it was told and holds every async call until the
/// test settles it. "sounding" is what the speakers would be doing.
class FakeCore final : public PlaybackCore {
    Q_OBJECT

public:
    using PlaybackCore::PlaybackCore;

    /// the core started a track and nobody has paused it since
    bool sounding = false;
    bool sessionValid = true;
    bool stalled = false;
    bool endOfTrack = false;
    quint32 position = 0;
    quint32 duration = 0;
    float volumeValue = 0.75f;
    bool spectrumEnabled = false;
    /// levels that move as music's would, for measuring what moving bars
    /// cost; still by default
    bool musicLevels = false;
    QStringList log;
    QList<std::shared_ptr<PendingCall>> pending;
    quint64 coreGeneration = 0;

    /// the oldest unsettled call with this name
    std::shared_ptr<PendingCall> take(const QString &name)
    {
        for (qsizetype index = 0; index < pending.size(); ++index) {
            if (pending.at(index)->name == name) {
                return pending.takeAt(index);
            }
        }
        return nullptr;
    }

    /// both complete with what the test settles them with, as the login
    QCoro::Task<QString> connectSession(QString accessToken, QString) override
    {
        co_return co_await await(QStringLiteral("connect"), accessToken);
    }
    QCoro::Task<QString> connectStoredSession(QString login, QString) override
    {
        co_return co_await await(QStringLiteral("connectStored"), login);
    }
    void disconnectSession() override { log << QStringLiteral("disconnect"); }
    void reset() override
    {
        ++coreGeneration;
        const auto calls = std::exchange(pending, {});
        for (const auto &call : calls) {
            call->cancel();
        }
        sounding = false;
    }
    quint64 generation() const override { return coreGeneration; }

    QCoro::Task<> playTrack(QString uri) override
    {
        co_await await(QStringLiteral("playTrack"), uri);
        // librespot starts the track once loaded, whoever still wants it
        sounding = true;
    }
    void preloadTrack(const QString &, bool) override {}
    QCoro::Task<> crossfadeTrack(QString uri, quint32) override
    {
        co_await await(QStringLiteral("crossfadeTrack"), uri);
        sounding = true;
    }
    QCoro::Task<> playFile(QString path) override
    {
        co_await await(QStringLiteral("playFile"), path);
        sounding = true;
    }
    QCoro::Task<> restartAudioOutput() override
    {
        co_await await(QStringLiteral("restartAudioOutput"), {});
        stalled = false;
    }
    QCoro::Task<QString> lyricsJson(QString uri) override { co_return co_await await(QStringLiteral("lyrics"), uri); }
    QCoro::Task<QString> playlistJson(QString uri) override { co_return co_await await(QStringLiteral("playlist"), uri); }
    QCoro::Task<QString> artistTopTracksJson(QString uri) override { co_return co_await await(QStringLiteral("topTracks"), uri); }

    void play() override
    {
        log << QStringLiteral("play");
        sounding = true;
    }
    void pause() override
    {
        log << QStringLiteral("pause");
        sounding = false;
    }
    void seek(quint32 positionMs) override
    {
        log << QStringLiteral("seek %1").arg(positionMs);
        position = positionMs;
    }
    void setVolume(float volume) override { volumeValue = volume; }
    float volume() const override { return volumeValue; }
    void setStreamQuality(int) override {}
    void setAudioCache(const QString &, qint64) override {}
    void setPlaybackShaping(float, float, bool) override {}
    void setEqualizer(const QList<float> &) override {}

    bool sessionIsValid() const override { return sessionValid; }
    bool audioOutputStalled() const override { return stalled; }
    quint32 playbackPositionMs() const override { return position; }
    quint32 playbackDurationMs() const override { return duration; }
    bool takeEndOfTrack() override { return std::exchange(endOfTrack, false); }

    void setSpectrumEnabled(bool enabled) override { spectrumEnabled = enabled; }
    /// something else started what plays, and takes the skips
    bool remote = false;
    bool skipRemote(bool forward) override
    {
        if (remote) {
            log << (forward ? QStringLiteral("skip next") : QStringLiteral("skip previous"));
        }
        return remote;
    }

    int spectrumLevels(float *out, int capacity) const override
    {
        std::fill(out, out + capacity, 0.0f);
        if (musicLevels) {
            static const QElapsedTimer clock = [] {
                QElapsedTimer started;
                started.start();
                return started;
            }();
            const double seconds = double(clock.elapsed()) / 1000.0;
            for (int band = 0; band < capacity; ++band) {
                out[band] = float(0.45 + 0.35 * std::sin(seconds * (3.0 + band * 0.37) + band));
            }
        }
        return capacity;
    }

private:
    QCoro::Task<QString> await(QString name, QString argument)
    {
        log << (argument.isEmpty() ? name : name + u' ' + argument);
        const auto call = std::make_shared<PendingCall>(name, argument);
        pending.append(call);
        const auto [ok, cancelled, payload] = co_await qCoro(call.get(), &PendingCall::finished);
        if (cancelled) {
            throw Cancelled();
        }
        if (!ok) {
            // as RustCore tells a replaced request from a failed one
            throw SpotifyError(payload == QLatin1StringView(SWIFTIFY_CORE_SUPERSEDED) ? SpotifyError::Kind::Superseded
                                                                                     : SpotifyError::Kind::Core,
                               payload);
        }
        co_return payload;
    }
};

} // namespace swiftify::testing
