#pragma once

#include "core/PlaybackCore.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QWebSocket>

#include <memory>
#include <optional>

namespace swiftify {

class SoloistAudioRoute;

/// where soloist is and the key it starts with
struct SoloistConfig {
    QString program;
    QString apiKey;
    QString dataDirectory;
    QString deviceName = QStringLiteral("Swiftify");
};

/// plays spotify songs through spotify soloist, spotify's own connect client,
/// for accounts spotify no longer gives librespot the keys for. soloist runs
/// as a child process and is driven over its local websocket api.
///
/// what soloist can't do is left to `fallback`, the rust core: the login,
/// lyrics, and the person's own music files. where the sound server allows,
/// soloist plays into a sink of swiftify's own, recorded and played on through
/// the rust core, so the equalizer and the visualizers reach it too; tempo
/// and pitch can't apply to audio that arrives as it plays.
/// swiftify keeps its own queue, so soloist is handed one song at a time, and
/// the next one goes in as the last ends, before soloist's autoplay would.
class SoloistCore : public PlaybackCore {
    Q_OBJECT

public:
    SoloistCore(PlaybackCore *fallback, SoloistConfig config, QObject *parent = nullptr);
    ~SoloistCore() override;

    /// soloist on the PATH (or SWIFTIFY_SOLOIST) and a key in SOLOIST_API_KEY
    /// or ~/.config/soloist/api-key; nothing if either is missing, or if
    /// SWIFTIFY_PLAYBACK=librespot asks for the rust core alone
    static std::optional<SoloistConfig> detect();

    QCoro::Task<QString> connectSession(QString accessToken, QString clientId) override;
    QCoro::Task<QString> connectStoredSession(QString login, QString clientId) override;
    void disconnectSession() override;
    void reset() override;
    quint64 generation() const override;

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
    float volume() const override { return m_volume; }
    void setStreamQuality(int kbps) override;
    void setAudioCache(const QString &path, qint64 limitBytes) override;
    void setPlaybackShaping(float tempo, float pitchSemitones, bool preservesPitch) override;
    void setEqualizer(const QList<float> &gainsDb) override;

    bool sessionIsValid() const override;
    bool audioOutputStalled() const override;
    quint32 playbackPositionMs() const override;
    quint32 playbackDurationMs() const override;
    bool takeEndOfTrack() override;

    void setSpectrumEnabled(bool enabled) override;
    int spectrumLevels(float *out, int capacity) const override;

    bool skipRemote(bool forward) override;

signals:
    /// anything soloist reported, or a reset; what waiting calls wake on
    void stateChanged();

private:
    enum class Status { Idle, Playing, Paused, Buffering };

    void startDaemon();
    void connectSocket();
    /// looks for soloist's port until the socket opens or time runs out
    void watchForSocket(bool restartClock);
    void handleMessage(const QString &text);
    void applyPlaybackState(const QJsonObject &state);
    void applyItem(const QJsonObject &item);
    void applyPosition(const QJsonObject &position);
    void send(const QString &command, QJsonObject fields = {});
    /// starts soloist if it is not running and waits until it is logged in
    QCoro::Task<> ensureReady(quint64 resets);
    /// waits for the next stateChanged, at most `timeoutMs`; false on timeout
    QCoro::Task<bool> waitForChange(int timeoutMs);
    void throwIfReset(quint64 resets) const;
    /// the position now, from soloist's last anchor
    qint64 estimatedPositionMs() const;
    /// the song swiftify asked for has played to its end
    bool endReached() const;
    /// keeps whatever soloist plays between a song's end and the next one's
    /// start (its autoplay) from being heard, where swiftify plays it on
    void gate(bool closed);
    void stopSoloist();
    /// notices soloist playing something swiftify didn't ask for (picked in
    /// spotify on a phone), and play or pause pressed elsewhere
    void followRemote();
    /// the song soloist is playing, from what it says of it
    Song songFromItem() const;
    /// soloist's own volume: all the way up when swiftify plays it on, and
    /// sets the volume itself
    int soloistVolume() const;
    /// hands what soloist plays to the rust core, if it is routed through it
    void playRouted();

    PlaybackCore *m_fallback;
    SoloistConfig m_config;
    std::unique_ptr<SwiftifyLiveBuffer, void (*)(SwiftifyLiveBuffer *)> m_liveBuffer;
#ifdef SWIFTIFY_DESKTOP_INTEGRATION
    std::unique_ptr<SoloistAudioRoute> m_route;
#endif
    /// soloist plays into m_route's sink
    bool m_routed = false;
    QProcess m_process;
    QWebSocket m_socket;
    QTimer m_portWatch;
    QElapsedTimer m_portWait;
    QString m_processOutput;

    bool m_socketOpen = false;
    /// soloist has said whether it is logged in, which it does on connecting
    bool m_authKnown = false;
    bool m_loggedIn = false;
    bool m_active = false;
    Status m_status = Status::Idle;
    QString m_itemUri;
    QJsonObject m_item;
    qint64 m_durationMs = 0;
    qint64 m_anchorPositionMs = 0;
    qint64 m_anchorTimestampMs = 0;
    double m_anchorSpeed = 0.0;
    QString m_repeat;
    QString m_lastError;
    QString m_failure;
    /// bumped by every message, so a wait can tell old state from new
    quint64 m_messages = 0;

    float m_volume = 0.75f;
    /// a file of the person's own is playing, through the rust core
    bool m_localActive = false;
    /// the song swiftify last asked soloist for, and whether it has started
    QString m_expectedUri;
    bool m_expectedStarted = false;
    bool m_endOfTrack = false;
    /// when the audio of the song ends, worked out once soloist stops its
    /// clock to buffer what follows, about 0.8s before; 0 until then
    qint64 m_endAtMs = 0;
    bool m_gated = false;
    qint64 m_gatedAtMs = 0;
    /// something else started what plays, and soloist goes on by its queue
    bool m_remote = false;
    /// swiftify's own plays still waiting for their song to start: soloist's
    /// state meanwhile is the old song's, and nobody else's doing
    int m_playsInFlight = 0;
    /// the playing state last reported, so a report is made on a change only
    std::optional<bool> m_reportedPlaying;
    quint64 m_requests = 0;
    quint64 m_resets = 0;
};

} // namespace swiftify
