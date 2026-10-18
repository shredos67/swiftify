#include "core/RustCore.h"

#include "spotify/SpotifyError.h"

#include <swiftify_core.h>

#include <QCoroSignal>
#include <QDebug>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace swiftify {

/// one async call in flight
class CoreRequest : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

signals:
    void finished(bool ok, bool cancelled, const QString &payload);
};

namespace {

/// takes ownership of a string from the core
QString takeString(char *value)
{
    if (!value) {
        return {};
    }
    QString result = QString::fromUtf8(value);
    swiftify_string_free(value);
    return result;
}

void throwIfError(char *error)
{
    if (error) {
        throw SpotifyError(SpotifyError::Kind::Core, takeString(error));
    }
}

} // namespace

RustCore::RustCore(QObject *parent)
    : PlaybackCore(parent)
{
    open();
}

RustCore::~RustCore()
{
    close();
}

QList<float> RustCore::equalizerFrequencies()
{
    float frequencies[16];
    const size_t count = swiftify_equalizer_frequencies(frequencies, std::size(frequencies));
    return QList<float>(frequencies, frequencies + count);
}

void RustCore::open()
{
    m_core = swiftify_core_new();
    if (!m_core) {
        qFatal("spotify-core could not start");
    }
#ifdef Q_OS_WIN
    // no descriptor to watch on windows: the core calls back from whichever
    // thread finished, and the drain is queued onto this one
    swiftify_core_set_completion_callback(m_core, [](void *context) {
        QMetaObject::invokeMethod(static_cast<RustCore *>(context), &RustCore::drainCompletions, Qt::QueuedConnection);
    }, this);
#else
    m_notifier = std::make_unique<QSocketNotifier>(swiftify_core_completion_fd(m_core), QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &RustCore::drainCompletions);
#endif
    // a fresh core after a reset gets what the last one was told; both were
    // taken once already, so a refusal now has nothing new to say
    if (m_streamKbps > 0) {
        swiftify_string_free(swiftify_core_set_stream_quality(m_core, uint32_t(m_streamKbps)));
    }
    if (!m_cachePath.isEmpty()) {
        swiftify_string_free(swiftify_core_set_audio_cache(m_core, m_cachePath.toUtf8().constData(),
                                                           quint64(std::max<qint64>(0, m_cacheBytes))));
    }
}

void RustCore::close()
{
#ifdef Q_OS_WIN
    // waits for a callback under way, so none reaches this after
    swiftify_core_set_completion_callback(m_core, nullptr, nullptr);
#endif
    m_notifier.reset();
    // nothing will complete these now. waking them as cancelled is what lets
    // their callers unwind, as swift's task cancellation would.
    const auto pending = std::exchange(m_pending, {});
    for (const auto &request : pending) {
        emit request->finished(false, true, {});
    }
    swiftify_core_free(m_core);
    m_core = nullptr;
}

void RustCore::reset()
{
    close();
    ++m_generation;
    open();
}

void RustCore::drainCompletions()
{
#ifndef Q_OS_WIN
    char buffer[64];
    while (::read(swiftify_core_completion_fd(m_core), buffer, sizeof buffer) > 0) {
    }
#endif

    uint64_t id = 0;
    bool ok = false;
    char *payload = nullptr;
    while (swiftify_core_take_completion(m_core, &id, &ok, &payload)) {
        const QString text = takeString(payload);
        if (const auto request = m_pending.take(id)) {
            emit request->finished(ok, false, text);
        }
    }
}

template <typename Start>
QCoro::Task<QString> RustCore::request(Start start)
{
    const quint64 id = ++m_nextRequestId;
    const auto pending = std::make_shared<CoreRequest>();
    m_pending.insert(id, pending);
    start(id);
    const auto [ok, cancelled, payload] = co_await qCoro(pending.get(), &CoreRequest::finished);
    if (cancelled) {
        throw Cancelled();
    }
    if (!ok) {
        throw SpotifyError(payload == QLatin1StringView(SWIFTIFY_CORE_SUPERSEDED) ? SpotifyError::Kind::Superseded
                                                                                 : SpotifyError::Kind::Core,
                           payload);
    }
    co_return payload;
}

QCoro::Task<QString> RustCore::connectSession(QString accessToken, QString clientId)
{
    const QByteArray token = accessToken.toUtf8();
    const QByteArray client = clientId.toUtf8();
    co_return co_await request(
        [&](quint64 id) { swiftify_core_connect(m_core, id, token.constData(), client.constData()); });
}

QCoro::Task<QString> RustCore::connectStoredSession(QString login, QString clientId)
{
    const QByteArray stored = login.toUtf8();
    const QByteArray client = clientId.toUtf8();
    co_return co_await request(
        [&](quint64 id) { swiftify_core_connect_stored(m_core, id, stored.constData(), client.constData()); });
}

void RustCore::disconnectSession()
{
    swiftify_core_disconnect(m_core);
}

QCoro::Task<> RustCore::playTrack(QString spotifyUri)
{
    const QByteArray uri = spotifyUri.toUtf8();
    co_await request([&](quint64 id) { swiftify_core_play_track(m_core, id, uri.constData()); });
}

void RustCore::preloadTrack(const QString &spotifyUri, bool preloadStandby)
{
    const QByteArray uri = spotifyUri.toUtf8();
    throwIfError(swiftify_core_preload_track(m_core, uri.constData(), preloadStandby ? 1 : 0));
}

QCoro::Task<> RustCore::crossfadeTrack(QString spotifyUri, quint32 durationMs)
{
    const QByteArray uri = spotifyUri.toUtf8();
    co_await request([&](quint64 id) {
        swiftify_core_crossfade_track(m_core, id, uri.constData(), uint32_t(durationMs));
    });
}

QCoro::Task<> RustCore::playFile(QString path)
{
    const QByteArray file = path.toUtf8();
    co_await request([&](quint64 id) { swiftify_core_play_file(m_core, id, file.constData()); });
}

QCoro::Task<> RustCore::restartAudioOutput()
{
    co_await request([&](quint64 id) { swiftify_core_restart_audio_output(m_core, id); });
}

QCoro::Task<QString> RustCore::lyricsJson(QString spotifyUri)
{
    const QByteArray uri = spotifyUri.toUtf8();
    co_return co_await request([&](quint64 id) { swiftify_core_lyrics_json(m_core, id, uri.constData()); });
}

QCoro::Task<QString> RustCore::playlistJson(QString playlistUri)
{
    const QByteArray uri = playlistUri.toUtf8();
    co_return co_await request([&](quint64 id) { swiftify_core_playlist_json(m_core, id, uri.constData()); });
}

QCoro::Task<QString> RustCore::artistTopTracksJson(QString artistUri)
{
    const QByteArray uri = artistUri.toUtf8();
    co_return co_await request([&](quint64 id) { swiftify_core_artist_top_tracks_json(m_core, id, uri.constData()); });
}

void RustCore::play() { throwIfError(swiftify_core_play(m_core)); }
void RustCore::pause() { throwIfError(swiftify_core_pause(m_core)); }
void RustCore::seek(quint32 positionMs) { throwIfError(swiftify_core_seek(m_core, positionMs)); }
void RustCore::setVolume(float volume) { throwIfError(swiftify_core_set_volume(m_core, volume)); }
float RustCore::volume() const { return swiftify_core_volume(m_core); }
void RustCore::setStreamQuality(int kbps)
{
    m_streamKbps = kbps;
    throwIfError(swiftify_core_set_stream_quality(m_core, uint32_t(kbps)));
}

void RustCore::setAudioCache(const QString &path, qint64 limitBytes)
{
    m_cachePath = path;
    m_cacheBytes = limitBytes;
    throwIfError(swiftify_core_set_audio_cache(m_core, path.toUtf8().constData(), quint64(std::max<qint64>(0, limitBytes))));
}

void RustCore::setPlaybackShaping(float tempo, float pitchSemitones, bool preservesPitch)
{
    throwIfError(swiftify_core_set_playback_shaping(m_core, tempo, pitchSemitones, preservesPitch));
}

void RustCore::setEqualizer(const QList<float> &gainsDb)
{
    throwIfError(swiftify_core_set_equalizer(m_core, gainsDb.constData(), size_t(gainsDb.size())));
}

bool RustCore::sessionIsValid() const { return swiftify_core_session_is_valid(m_core); }
bool RustCore::audioOutputStalled() const { return swiftify_core_audio_output_stalled(m_core); }
quint32 RustCore::playbackPositionMs() const { return swiftify_core_playback_position_ms(m_core); }
quint32 RustCore::playbackDurationMs() const { return swiftify_core_playback_duration_ms(m_core); }
bool RustCore::takeEndOfTrack() { return swiftify_core_take_end_of_track(m_core); }

bool RustCore::playLiveBuffer(SwiftifyLiveBuffer *buffer)
{
    if (char *error = swiftify_core_play_live(m_core, buffer)) {
        qWarning().noquote() << "Live audio could not play:" << takeString(error);
        return false;
    }
    return true;
}

quint32 RustCore::liveOutputRate() const
{
    return swiftify_core_output_rate(m_core);
}

void RustCore::stopLiveBuffer()
{
    swiftify_core_stop_live(m_core);
}

void RustCore::setSpectrumEnabled(bool enabled)
{
    swiftify_core_set_spectrum_enabled(m_core, enabled);
}

int RustCore::spectrumLevels(float *out, int capacity) const
{
    return int(swiftify_core_spectrum_levels(m_core, out, size_t(std::max(capacity, 0))));
}

} // namespace swiftify

#include "RustCore.moc"
