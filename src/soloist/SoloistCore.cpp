#include "soloist/SoloistCore.h"

#include "app/Paths.h"
#include "spotify/SpotifyError.h"
#ifdef SWIFTIFY_DESKTOP_INTEGRATION
#include "soloist/SoloistAudioRoute.h"
#endif

#include <swiftify_core.h>

#include <QCoroSignal>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#endif

Q_LOGGING_CATEGORY(lcSoloist, "swiftify.soloist")

namespace swiftify {

namespace {

/// how long soloist gets to start and open its websocket
constexpr int kStartTimeoutMs = 15'000;
/// how long a song gets to start, as the rust core allows
constexpr int kLoadTimeoutMs = 30'000;
/// a song whose clock has run this close to its end has ended
constexpr qint64 kEndSlackMs = 30;
/// what is still on its way through swiftify's output when soloist is done
/// with a song, when it plays soloist on; the end waits for it
constexpr qint64 kRouteDelayMs = 80;
/// a song that has not started this long after it was asked for is asked
/// for again, once
constexpr int kReplayAfterMs = 5'000;
/// a position this close to the end is near it
constexpr qint64 kNearEndMs = 3'000;
/// what of soloist's output is kept, to say why it stopped
constexpr qsizetype kOutputTail = 4000;

const QString kPairingMessage = QStringLiteral(
    "Soloist isn't paired with your Spotify account yet. Open the Spotify desktop app on this computer "
    "(or the phone app), pick “%1” in the devices list once, then press play here again.");

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

SoloistCore::SoloistCore(PlaybackCore *fallback, SoloistConfig config, QObject *parent)
    : PlaybackCore(parent)
    , m_fallback(fallback)
    , m_config(std::move(config))
    , m_liveBuffer(nullptr, &swiftify_live_buffer_free)
{
    m_process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_LINUX
    // soloist goes with a swiftify that crashes or is killed too. one left
    // behind holds the data directory, and the next soloist will not start
    m_process.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif
    connect(&m_process, &QProcess::readyRead, this, [this] {
        const QString output = QString::fromUtf8(m_process.readAll());
        for (const QStringView line : QStringView(output).split(u'\n', Qt::SkipEmptyParts)) {
            qCDebug(lcSoloist).noquote() << line;
        }
        m_processOutput = (m_processOutput + output).right(kOutputTail);
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_portWatch.stop();
        m_socket.abort();
        m_socketOpen = false;
        m_authKnown = false;
        m_loggedIn = false;
        m_active = false;
        m_status = Status::Idle;
        // exit code 10 is a build past its 90 days
        const QString last = m_processOutput.trimmed().section(u'\n', -3);
        m_failure = code == 10 && status == QProcess::NormalExit
            ? QStringLiteral("This Soloist build has expired. Download a new one from developer.spotify.com.")
            : QStringLiteral("Soloist stopped (exit code %1). %2").arg(code).arg(last);
        qCWarning(lcSoloist).noquote() << m_failure;
        emit stateChanged();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_portWatch.stop();
            m_failure = QStringLiteral("Soloist could not start: %1").arg(m_process.errorString());
            qCWarning(lcSoloist).noquote() << m_failure;
            emit stateChanged();
        }
    });

    m_portWatch.setInterval(100);
    connect(&m_portWatch, &QTimer::timeout, this, &SoloistCore::connectSocket);

    connect(&m_socket, &QWebSocket::connected, this, [this] {
        qCInfo(lcSoloist) << "Connected to Soloist";
        m_socketOpen = true;
        emit stateChanged();
    });
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        const bool wasOpen = m_socketOpen;
        m_socketOpen = false;
        m_authKnown = false;
        // soloist still running lost its socket, so it is opened again
        if (m_process.state() != QProcess::NotRunning && !m_portWatch.isActive()) {
            watchForSocket(wasOpen);
        }
        emit stateChanged();
    });
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this] {
        // a socket that would not open is tried again until soloist's start
        // runs out of time
        if (!m_socketOpen && m_process.state() != QProcess::NotRunning && !m_portWatch.isActive()
            && m_socket.state() == QAbstractSocket::UnconnectedState) {
            watchForSocket(false);
        }
    });
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &SoloistCore::handleMessage);

    // started at once, so the device is there to pair with before anything plays
    QTimer::singleShot(0, this, &SoloistCore::startDaemon);
}

SoloistCore::~SoloistCore()
{
    m_portWatch.stop();
    m_socket.disconnect(this);
    m_socket.abort();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.disconnect(this);
        m_process.terminate();
        if (!m_process.waitForFinished(2000)) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }
}

std::optional<SoloistConfig> SoloistCore::detect()
{
    if (qEnvironmentVariable("SWIFTIFY_PLAYBACK") == QLatin1StringView("librespot")) {
        return std::nullopt;
    }

    SoloistConfig config;
    config.program = qEnvironmentVariable("SWIFTIFY_SOLOIST");
    if (config.program.isEmpty()) {
        config.program = QStandardPaths::findExecutable(QStringLiteral("soloist"));
    }
    if (config.program.isEmpty()) {
        // a launcher's PATH often leaves out ~/.local/bin, where soloist's
        // guide installs it without sudo
        config.program = QStandardPaths::findExecutable(QStringLiteral("soloist"),
                                                        {QDir::homePath() + QStringLiteral("/.local/bin")});
    }
    if (config.program.isEmpty()) {
        qCInfo(lcSoloist) << "Soloist not found; playing through librespot";
        return std::nullopt;
    }

    config.apiKey = qEnvironmentVariable("SOLOIST_API_KEY").trimmed();
    if (config.apiKey.isEmpty()) {
        QFile file(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                   + QStringLiteral("/soloist/api-key"));
        if (file.open(QIODevice::ReadOnly)) {
            config.apiKey = QString::fromUtf8(file.readAll()).trimmed();
        }
    }
    if (config.apiKey.isEmpty()) {
        qCWarning(lcSoloist) << "Soloist found, but no API key in SOLOIST_API_KEY or ~/.config/soloist/api-key;"
                                " playing through librespot";
        return std::nullopt;
    }

    config.dataDirectory = paths::dataDirectory() + QStringLiteral("/soloist");
    return config;
}

// --- the daemon -----------------------------------------------------------------

void SoloistCore::startDaemon()
{
    if (m_process.state() != QProcess::NotRunning) {
        return;
    }
    m_failure.clear();
    m_processOutput.clear();
    QDir().mkpath(m_config.dataDirectory);
#ifdef SWIFTIFY_DESKTOP_INTEGRATION
    if (!m_route && qEnvironmentVariable("SWIFTIFY_SOLOIST_ROUTE") != QLatin1StringView("off")) {
        // recorded at the rate swiftify's output plays at, so it is played
        // on as it is rather than resampled again
        const quint32 rate = m_fallback->liveOutputRate() ? m_fallback->liveOutputRate() : 48000;
        m_liveBuffer.reset(swiftify_live_buffer_new(rate, SoloistAudioRoute::kChannels));
        m_route = std::make_unique<SoloistAudioRoute>(m_liveBuffer.get(), rate);
        m_routed = m_route->start();
        if (m_routed) {
            qCInfo(lcSoloist) << "Soloist plays through Swiftify, for the equalizer and the visualizers";
        } else {
            qCWarning(lcSoloist).noquote() << "Soloist plays straight to the speakers:" << m_route->error();
            m_route.reset();
        }
    }
#endif
    // the port soloist writes once its websocket is up; an old one would point
    // at a soloist long gone
    QFile::remove(m_config.dataDirectory + QStringLiteral("/ws.port"));
    QFile::remove(m_config.dataDirectory + QStringLiteral("/ws.addr"));

    QStringList arguments{
        QStringLiteral("--device-name"), m_config.deviceName,
        QStringLiteral("--api-key"), m_config.apiKey,
        QStringLiteral("--data-dir"), m_config.dataDirectory,
        QStringLiteral("--ws"), QStringLiteral("127.0.0.1:0"),
        QStringLiteral("--initial-volume"), QString::number(soloistVolume()),
    };
#ifdef SWIFTIFY_DESKTOP_INTEGRATION
    if (m_routed) {
        arguments << QStringLiteral("--pipewire-device") << SoloistAudioRoute::sinkName();
    }
#endif
    qCInfo(lcSoloist).noquote() << "Starting" << m_config.program;
    m_process.start(m_config.program, arguments);
    watchForSocket(true);
}

void SoloistCore::watchForSocket(bool restartClock)
{
    if (restartClock) {
        m_portWait.start();
    }
    m_portWatch.start();
}

void SoloistCore::connectSocket()
{
    if (m_portWait.elapsed() > kStartTimeoutMs) {
        m_portWatch.stop();
        m_failure = QStringLiteral("Soloist started but never opened its control socket.");
        emit stateChanged();
        return;
    }
    QFile file(m_config.dataDirectory + QStringLiteral("/ws.port"));
    bool ok = false;
    const int port = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed().toInt(&ok) : 0;
    if (!ok || port <= 0) {
        return;
    }
    m_portWatch.stop();
    m_socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(port)));
}

void SoloistCore::send(const QString &command, QJsonObject fields)
{
    // every command used here needs a login; one sent before would only come
    // back as an error, and fail the next play
    if (!m_socketOpen || !m_loggedIn) {
        return;
    }
    fields.insert(QStringLiteral("type"), QStringLiteral("command"));
    fields.insert(QStringLiteral("command"), command);
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact)));
}

void SoloistCore::handleMessage(const QString &text)
{
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1StringView("auth_state")) {
        m_authKnown = true;
        const bool wasLoggedIn = m_loggedIn;
        m_loggedIn = message.value(QStringLiteral("logged_in")).toBool();
        m_active = message.value(QStringLiteral("is_active")).toBool();
        qCInfo(lcSoloist) << "Logged in:" << m_loggedIn << "active:" << m_active;
        if (m_loggedIn && !wasLoggedIn) {
            // the volume set while soloist waited for its login
            send(QStringLiteral("set_volume"), {{QStringLiteral("volume"), soloistVolume()}});
        }
    } else if (type == QLatin1StringView("playback_state")) {
        applyPlaybackState(message);
    } else if (type == QLatin1StringView("track_changed")) {
        applyItem(message.value(QStringLiteral("item")).toObject());
    } else if (type == QLatin1StringView("playback_changed")) {
        applyPlaybackState(message);
    } else if (type == QLatin1StringView("position_sync")) {
        applyPosition(message.value(QStringLiteral("position")).toObject());
    } else if (type == QLatin1StringView("device_changed")) {
        m_active = message.value(QStringLiteral("is_active")).toBool();
    } else if (type == QLatin1StringView("options_changed")) {
        m_repeat = message.value(QStringLiteral("options")).toObject().value(QStringLiteral("repeat")).toString();
    } else if (type == QLatin1StringView("error")) {
        m_lastError = message.value(QStringLiteral("message")).toString();
        qCWarning(lcSoloist).noquote() << "Soloist:" << m_lastError;
    } else {
        return;
    }
    ++m_messages;
    followRemote();
    emit stateChanged();
}

void SoloistCore::followRemote()
{
    // a play of swiftify's own is under way, or a song's end is being handed
    // over: what soloist says now is not someone else's doing
    // (a gate stays shut after swiftify's queue has run out; only just after
    // a song's end is what plays soloist's own doing)
    if (m_playsInFlight > 0 || (m_gated && nowMs() - m_gatedAtMs < 2'000) || m_endOfTrack || !m_loggedIn) {
        return;
    }
    const bool sounding = m_status == Status::Playing || m_status == Status::Buffering;
    if (sounding && !m_itemUri.isEmpty() && m_itemUri != m_expectedUri) {
        qCInfo(lcSoloist) << "Playing" << m_itemUri << "as started from elsewhere";
        m_remote = true;
        m_expectedUri = m_itemUri;
        m_expectedStarted = false;
        m_endAtMs = 0;
        if (m_localActive) {
            try {
                m_fallback->pause();
            } catch (const SpotifyError &) {
            }
            m_localActive = false;
        }
        playRouted();
        gate(false);
        m_reportedPlaying = true;
        emit remoteTrackStarted(songFromItem());
        return;
    }
    // a file of the person's own playing leaves soloist paused, which is
    // not a pause anyone pressed
    if (m_localActive || !(m_status == Status::Playing || m_status == Status::Paused)) {
        return;
    }
    const bool playing = m_status == Status::Playing;
    if (m_reportedPlaying != playing) {
        m_reportedPlaying = playing;
        emit remotePlayingChanged(playing);
    }
}

Song SoloistCore::songFromItem() const
{
    const auto name = [](const QJsonObject &entity) {
        return entity.value(QStringLiteral("decorations")).toObject().value(QStringLiteral("identity")).toObject()
            .value(QStringLiteral("name")).toString();
    };
    const QJsonObject decorations = m_item.value(QStringLiteral("decorations")).toObject();
    Song song;
    song.uri = m_itemUri;
    song.name = name(m_item);
    song.durationMs = int(m_durationMs);
    QStringList artists;
    for (const QJsonValue creator : decorations.value(QStringLiteral("creators")).toArray()) {
        const QJsonObject entity = creator.toObject().value(QStringLiteral("entity")).toObject();
        Artist artist;
        artist.name = name(entity);
        artist.uri = entity.value(QStringLiteral("uri")).toString();
        artist.id = artist.uri.section(u':', 2);
        artists << artist.name;
        song.artistItems << artist;
    }
    song.artists = artists.join(QStringLiteral(", "));
    const QJsonObject album = decorations.value(QStringLiteral("parent")).toObject().value(QStringLiteral("entity")).toObject();
    song.albumName = name(album);
    song.albumUri = album.value(QStringLiteral("uri")).toString();
    song.albumId = song.albumUri.section(u':', 2);
    // the largest cover soloist offers
    for (const char *size : {"xlarge", "large", "default", "small"}) {
        for (const QJsonValue cover : decorations.value(QStringLiteral("visual_identity")).toObject()
                 .value(QStringLiteral("cover")).toArray()) {
            if (song.artworkUrl.isEmpty() && cover.toObject().value(QStringLiteral("size")).toString() == QLatin1StringView(size)) {
                song.artworkUrl = QUrl(cover.toObject().value(QStringLiteral("url")).toString());
            }
        }
    }
    return song;
}

bool SoloistCore::skipRemote(bool forward)
{
    if (!m_remote || m_localActive) {
        return false;
    }
    send(forward ? QStringLiteral("skip_next") : QStringLiteral("skip_prev"));
    return true;
}

void SoloistCore::applyPlaybackState(const QJsonObject &state)
{
    if (const QJsonValue status = state.value(QStringLiteral("status")); status.isString()) {
        const QString name = status.toString();
        m_status = name == QLatin1StringView("playing") ? Status::Playing
            : name == QLatin1StringView("paused")       ? Status::Paused
            : name == QLatin1StringView("buffering")    ? Status::Buffering
                                                        : Status::Idle;
    }
    if (state.contains(QStringLiteral("item"))) {
        applyItem(state.value(QStringLiteral("item")).toObject());
    }
    if (state.contains(QStringLiteral("position"))) {
        applyPosition(state.value(QStringLiteral("position")).toObject());
    }
    if (state.contains(QStringLiteral("is_active"))) {
        m_active = state.value(QStringLiteral("is_active")).toBool();
    }
    if (state.contains(QStringLiteral("options"))) {
        m_repeat = state.value(QStringLiteral("options")).toObject().value(QStringLiteral("repeat")).toString();
    }
}

void SoloistCore::applyItem(const QJsonObject &item)
{
    const QString uri = item.value(QStringLiteral("uri")).toString();
    // something else taking over at the end of the song swiftify asked for
    // (soloist's autoplay) means that song has ended
    if (m_expectedStarted && m_itemUri == m_expectedUri && uri != m_expectedUri && m_durationMs > 0
        && (m_endAtMs > 0 || std::max(m_anchorPositionMs, estimatedPositionMs()) >= m_durationMs - kNearEndMs)) {
        m_endOfTrack = true;
    }
    if (uri != m_itemUri) {
        m_endAtMs = 0;
    }
    m_itemUri = uri;
    m_item = item;
    // soloist 1.3 nests what the reference shows at the top under decorations
    QJsonObject playback = item.value(QStringLiteral("decorations")).toObject().value(QStringLiteral("playback")).toObject();
    if (playback.isEmpty()) {
        playback = item.value(QStringLiteral("playback")).toObject();
    }
    m_durationMs = qint64(playback.value(QStringLiteral("duration_ms")).toDouble());
}

void SoloistCore::applyPosition(const QJsonObject &position)
{
    if (position.isEmpty()) {
        return;
    }
    const qint64 positionMs = qint64(position.value(QStringLiteral("position_ms")).toDouble());
    const bool expected = m_expectedStarted && m_itemUri == m_expectedUri && m_durationMs > 0;
    // a song started over from near its end (repeat, set from another app)
    // has ended too. the last position reported counts, not an estimate:
    // the clock was stopped while buffering
    if (expected && std::max(m_anchorPositionMs, estimatedPositionMs()) >= m_durationMs - kNearEndMs
        && positionMs < kNearEndMs) {
        m_endOfTrack = true;
    }
    m_anchorPositionMs = positionMs;
    m_anchorTimestampMs = qint64(position.value(QStringLiteral("timestamp_ms")).toDouble());
    m_anchorSpeed = position.value(QStringLiteral("speed")).toDouble();
    if (m_anchorTimestampMs <= 0) {
        m_anchorTimestampMs = nowMs();
    }
    // soloist stops its clock about 0.8s before a song's end to buffer what
    // follows, while the rest of the song plays on. the song ends when that
    // rest has played, not when the clock stops
    if (expected && m_anchorSpeed == 0.0 && m_status == Status::Buffering
        && positionMs >= m_durationMs - kNearEndMs) {
        m_endAtMs = m_anchorTimestampMs + (m_durationMs - positionMs);
    } else if (m_anchorSpeed > 0.0) {
        m_endAtMs = 0;
    }
}

qint64 SoloistCore::estimatedPositionMs() const
{
    const qint64 elapsed = std::max<qint64>(0, nowMs() - m_anchorTimestampMs);
    const qint64 position = m_anchorPositionMs + qint64(double(elapsed) * m_anchorSpeed);
    return m_durationMs > 0 ? std::clamp<qint64>(position, 0, m_durationMs) : std::max<qint64>(position, 0);
}

bool SoloistCore::endReached() const
{
    if (!m_expectedStarted || m_itemUri != m_expectedUri || m_durationMs <= 0) {
        return false;
    }
    if (m_endAtMs > 0) {
        return nowMs() >= m_endAtMs + (m_routed ? kRouteDelayMs : 0);
    }
    if (m_status == Status::Idle) {
        return m_anchorPositionMs >= m_durationMs - kNearEndMs;
    }
    return m_status == Status::Playing && m_anchorSpeed > 0.0 && estimatedPositionMs() >= m_durationMs - kEndSlackMs;
}

void SoloistCore::gate(bool closed)
{
    if (closed == m_gated || (closed && !m_routed)) {
        return;
    }
    m_gated = closed;
    m_gatedAtMs = nowMs();
    try {
        m_fallback->setVolume(closed ? 0.0f : m_volume);
    } catch (const SpotifyError &) {
    }
}

QCoro::Task<bool> SoloistCore::waitForChange(int timeoutMs)
{
    const auto result = co_await qCoro(this, &SoloistCore::stateChanged, std::chrono::milliseconds(std::max(timeoutMs, 1)));
    co_return result.has_value();
}

void SoloistCore::throwIfReset(quint64 resets) const
{
    if (resets != m_resets) {
        throw Cancelled();
    }
}

QCoro::Task<> SoloistCore::ensureReady(quint64 resets)
{
    if (m_process.state() == QProcess::NotRunning) {
        startDaemon();
    } else if (!m_socketOpen && !m_portWatch.isActive() && m_socket.state() == QAbstractSocket::UnconnectedState) {
        // soloist runs but its socket gave up, so it gets another go
        m_failure.clear();
        watchForSocket(true);
    }
    QElapsedTimer waited;
    waited.start();
    while (!(m_socketOpen && m_authKnown)) {
        throwIfReset(resets);
        if (!m_failure.isEmpty()) {
            throw SpotifyError(SpotifyError::Kind::Core, m_failure);
        }
        const qint64 remaining = kStartTimeoutMs - waited.elapsed();
        if (remaining <= 0) {
            throw SpotifyError(SpotifyError::Kind::Core, QStringLiteral("Soloist did not start in time."));
        }
        co_await waitForChange(int(remaining));
    }
    throwIfReset(resets);
    if (!m_loggedIn) {
        throw SpotifyError(SpotifyError::Kind::Session, kPairingMessage.arg(m_config.deviceName));
    }
}

int SoloistCore::soloistVolume() const
{
    return m_routed ? 100 : int(std::lround(std::clamp(m_volume, 0.0f, 1.0f) * 100));
}

void SoloistCore::playRouted()
{
    if (m_routed && !m_fallback->playLiveBuffer(m_liveBuffer.get())) {
        qCWarning(lcSoloist) << "Swiftify's output would not take Soloist's audio";
    }
}

void SoloistCore::stopSoloist()
{
    if (m_status == Status::Playing || m_status == Status::Buffering) {
        send(QStringLiteral("pause"));
    }
}

// --- playback --------------------------------------------------------------------

QCoro::Task<> SoloistCore::playTrack(QString spotifyUri)
{
    const quint64 resets = m_resets;
    const quint64 request = ++m_requests;
    // soloist's state is the old song's until this one starts, however it ends
    ++m_playsInFlight;
    const auto settled = qScopeGuard([this] { --m_playsInFlight; });
    m_remote = false;
    co_await ensureReady(resets);
    if (request != m_requests) {
        throw SpotifyError(SpotifyError::Kind::Superseded, QStringLiteral("superseded"));
    }

    if (m_localActive) {
        try {
            m_fallback->pause();
        } catch (const SpotifyError &) {
        }
        m_localActive = false;
    }
    m_expectedUri = spotifyUri;
    m_expectedStarted = false;
    m_endOfTrack = false;
    m_lastError.clear();
    if (!m_active) {
        send(QStringLiteral("activate"));
    }
    // swiftify's queue decides what follows, not soloist's repeat
    if (!m_repeat.isEmpty() && m_repeat != QLatin1StringView("off")) {
        send(QStringLiteral("set_repeat_track"), {{QStringLiteral("enabled"), false}});
        send(QStringLiteral("set_repeat_context"), {{QStringLiteral("enabled"), false}});
    }
    // swiftify's output takes soloist's sink over again: after a file, the
    // file output played that instead
    playRouted();
    send(QStringLiteral("set_volume"), {{QStringLiteral("volume"), soloistVolume()}});
    send(QStringLiteral("play"), {{QStringLiteral("uri"), spotifyUri}});
    const quint64 sentAt = m_messages;
    bool replayed = false;

    QElapsedTimer waited;
    waited.start();
    for (;;) {
        throwIfReset(resets);
        if (request != m_requests) {
            throw SpotifyError(SpotifyError::Kind::Superseded, QStringLiteral("superseded"));
        }
        if (!m_lastError.isEmpty()) {
            throw SpotifyError(SpotifyError::Kind::Core, QStringLiteral("Soloist: %1").arg(m_lastError));
        }
        if (!m_failure.isEmpty()) {
            throw SpotifyError(SpotifyError::Kind::Core, m_failure);
        }
        // news from after the command, so a song played again counts only
        // once it has started over
        if (m_messages > sentAt && m_itemUri == spotifyUri && m_status == Status::Playing
            && estimatedPositionMs() < 10'000) {
            break;
        }
        // a song asked for just as soloist moved on by itself can be left
        // loading for good; asking again starts it
        if (!replayed && waited.elapsed() >= kReplayAfterMs) {
            replayed = true;
            qCInfo(lcSoloist) << "Asking again for" << spotifyUri;
            send(QStringLiteral("play"), {{QStringLiteral("uri"), spotifyUri}});
        }
        const qint64 remaining = (replayed ? kLoadTimeoutMs : kReplayAfterMs) - waited.elapsed();
        if (remaining <= 0) {
            throw SpotifyError(SpotifyError::Kind::Core, QStringLiteral("timed out while loading the Spotify track"));
        }
        co_await waitForChange(int(remaining));
    }
    m_expectedStarted = true;
    m_endAtMs = 0;
    gate(false);
}

void SoloistCore::preloadTrack(const QString &, bool)
{
    // soloist buffers ahead by itself
}

QCoro::Task<> SoloistCore::crossfadeTrack(QString spotifyUri, quint32)
{
    // soloist takes one song at a time, so there is no fading one into the
    // next here. the next song waits for this one to finish instead of
    // cutting it short.
    const quint64 resets = m_resets;
    const quint64 request = ++m_requests;
    while (!m_localActive && m_expectedStarted && m_itemUri == m_expectedUri && !endReached()
           && m_status != Status::Idle) {
        co_await waitForChange(200);
        throwIfReset(resets);
        if (request != m_requests) {
            throw SpotifyError(SpotifyError::Kind::Superseded, QStringLiteral("superseded"));
        }
    }
    co_await playTrack(spotifyUri);
}

QCoro::Task<> SoloistCore::playFile(QString path)
{
    ++m_requests;
    stopSoloist();
    m_expectedUri.clear();
    m_expectedStarted = false;
    m_endOfTrack = false;
    m_endAtMs = 0;
    m_remote = false;
    gate(false);
    m_localActive = true;
    co_await m_fallback->playFile(path);
}

QCoro::Task<> SoloistCore::restartAudioOutput()
{
    if (m_localActive || m_routed) {
        co_await m_fallback->restartAudioOutput();
    }
    if (m_routed && !m_localActive) {
        playRouted();
    }
}

void SoloistCore::play()
{
    if (m_localActive) {
        m_fallback->play();
        return;
    }
    if (!m_socketOpen) {
        throw SpotifyError(SpotifyError::Kind::Core, m_failure.isEmpty() ? QStringLiteral("Soloist isn't running.") : m_failure);
    }
    gate(false);
    playRouted();
    send(QStringLiteral("play"));
}

void SoloistCore::pause()
{
    if (m_localActive) {
        m_fallback->pause();
        return;
    }
    stopSoloist();
}

void SoloistCore::seek(quint32 positionMs)
{
    if (m_localActive) {
        m_fallback->seek(positionMs);
        return;
    }
    send(QStringLiteral("seek"), {{QStringLiteral("position_ms"), qint64(positionMs)}});
    // shown at once; soloist's position_sync confirms it
    m_anchorPositionMs = positionMs;
    m_anchorTimestampMs = nowMs();
    m_endOfTrack = false;
    m_endAtMs = 0;
}

void SoloistCore::setVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    try {
        // held at nothing while the gap between songs is gated
        m_fallback->setVolume(m_gated ? 0.0f : m_volume);
    } catch (const SpotifyError &) {
    }
    if (!m_routed) {
        send(QStringLiteral("set_volume"), {{QStringLiteral("volume"), soloistVolume()}});
    }
}

bool SoloistCore::audioOutputStalled() const
{
    return (m_localActive || m_routed) && m_fallback->audioOutputStalled();
}

quint32 SoloistCore::playbackPositionMs() const
{
    if (m_localActive) {
        return m_fallback->playbackPositionMs();
    }
    // an end not yet taken reads as the end, even once soloist has started
    // the song over; with the clock stopped for the last moments, the time
    // still runs on toward the end
    if (m_endOfTrack) {
        return quint32(m_durationMs);
    }
    if (m_endAtMs > 0) {
        return quint32(std::max<qint64>(0, m_durationMs - std::max<qint64>(0, m_endAtMs - nowMs())));
    }
    return quint32(estimatedPositionMs());
}

quint32 SoloistCore::playbackDurationMs() const
{
    return m_localActive ? m_fallback->playbackDurationMs() : quint32(m_durationMs);
}

bool SoloistCore::takeEndOfTrack()
{
    if (m_localActive) {
        return m_fallback->takeEndOfTrack();
    }
    if (!m_endOfTrack && endReached()) {
        m_endOfTrack = true;
    }
    if (!m_endOfTrack) {
        return false;
    }
    qCDebug(lcSoloist) << "End of" << m_expectedUri;
    m_endOfTrack = false;
    m_endAtMs = 0;
    // until the next song has started, or play is pressed again
    gate(true);
    // once per song: whatever plays next sets it up again
    m_expectedStarted = false;
    return true;
}

// --- left to the rust core -------------------------------------------------------

QCoro::Task<QString> SoloistCore::connectSession(QString accessToken, QString clientId)
{
    co_return co_await m_fallback->connectSession(accessToken, clientId);
}

QCoro::Task<QString> SoloistCore::connectStoredSession(QString login, QString clientId)
{
    co_return co_await m_fallback->connectStoredSession(login, clientId);
}

void SoloistCore::disconnectSession()
{
    m_fallback->disconnectSession();
}

void SoloistCore::reset()
{
    stopSoloist();
    ++m_resets;
    m_expectedUri.clear();
    m_expectedStarted = false;
    m_endOfTrack = false;
    m_endAtMs = 0;
    m_localActive = false;
    m_remote = false;
    m_reportedPlaying.reset();
    gate(false);
    m_fallback->reset();
    emit stateChanged();
}

quint64 SoloistCore::generation() const
{
    return m_fallback->generation();
}

QCoro::Task<QString> SoloistCore::lyricsJson(QString spotifyUri)
{
    co_return co_await m_fallback->lyricsJson(spotifyUri);
}

QCoro::Task<QString> SoloistCore::playlistJson(QString playlistUri)
{
    co_return co_await m_fallback->playlistJson(playlistUri);
}

QCoro::Task<QString> SoloistCore::artistTopTracksJson(QString artistUri)
{
    co_return co_await m_fallback->artistTopTracksJson(artistUri);
}

void SoloistCore::setStreamQuality(int kbps)
{
    m_fallback->setStreamQuality(kbps);
}

void SoloistCore::setAudioCache(const QString &path, qint64 limitBytes)
{
    m_fallback->setAudioCache(path, limitBytes);
}

void SoloistCore::setPlaybackShaping(float tempo, float pitchSemitones, bool preservesPitch)
{
    m_fallback->setPlaybackShaping(tempo, pitchSemitones, preservesPitch);
}

void SoloistCore::setEqualizer(const QList<float> &gainsDb)
{
    m_fallback->setEqualizer(gainsDb);
}

bool SoloistCore::sessionIsValid() const
{
    return m_fallback->sessionIsValid();
}

void SoloistCore::setSpectrumEnabled(bool enabled)
{
    m_fallback->setSpectrumEnabled(enabled);
}

int SoloistCore::spectrumLevels(float *out, int capacity) const
{
    // unrouted, soloist's audio goes straight to pipewire, past the analyser
    return m_localActive || m_routed ? m_fallback->spectrumLevels(out, capacity) : 0;
}

} // namespace swiftify
