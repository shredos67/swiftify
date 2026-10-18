#pragma once

#include "core/PlaybackCore.h"

#include <QHash>
#include <QSocketNotifier>

#include <memory>

struct SwiftifyCore;

namespace swiftify {

class CoreRequest;

/// spotify-core through its c abi (spotify-core/include/swiftify_core.h).
class RustCore : public PlaybackCore {
    Q_OBJECT

public:
    explicit RustCore(QObject *parent = nullptr);
    ~RustCore() override;

    static QList<float> equalizerFrequencies();

    QCoro::Task<QString> connectSession(QString accessToken, QString clientId) override;
    QCoro::Task<QString> connectStoredSession(QString login, QString clientId) override;
    void disconnectSession() override;
    void reset() override;
    quint64 generation() const override { return m_generation; }

    QCoro::Task<> playTrack(QString spotifyUri) override;
    void preloadTrack(const QString &spotifyUri, bool preloadStandby) override;
    QCoro::Task<> crossfadeTrack(QString spotifyUri, quint32 durationMs) override;
    QCoro::Task<> playFile(QString path) override;
    QCoro::Task<> restartAudioOutput() override;
    QCoro::Task<QString> lyricsJson(QString spotifyUri) override;
    QCoro::Task<QString> playlistJson(QString playlistUri) override;
    QCoro::Task<QString> artistTopTracksJson(QString artistUri) override;

    void play() override;
    void pause() override;
    void seek(quint32 positionMs) override;
    void setVolume(float volume) override;
    float volume() const override;
    void setStreamQuality(int kbps) override;
    void setAudioCache(const QString &path, qint64 limitBytes) override;
    void setPlaybackShaping(float tempo, float pitchSemitones, bool preservesPitch) override;
    void setEqualizer(const QList<float> &gainsDb) override;

    bool sessionIsValid() const override;
    bool audioOutputStalled() const override;
    quint32 playbackPositionMs() const override;
    quint32 playbackDurationMs() const override;
    bool takeEndOfTrack() override;

    bool playLiveBuffer(SwiftifyLiveBuffer *buffer) override;
    void stopLiveBuffer() override;
    quint32 liveOutputRate() const override;

    void setSpectrumEnabled(bool enabled) override;
    int spectrumLevels(float *out, int capacity) const override;

private:
    void open();
    void close();
    void drainCompletions();
    /// starts a call through `start` and waits for its completion
    template <typename Start>
    QCoro::Task<QString> request(Start start);

    SwiftifyCore *m_core = nullptr;
    std::unique_ptr<QSocketNotifier> m_notifier;
    QHash<quint64, std::shared_ptr<CoreRequest>> m_pending;
    quint64 m_nextRequestId = 0;
    quint64 m_generation = 0;
    /// given again to each core opened (a reset makes a fresh one)
    int m_streamKbps = 0;
    QString m_cachePath;
    qint64 m_cacheBytes = 0;
};

} // namespace swiftify
