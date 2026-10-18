#include "system/DiscordPresence.h"
#include "app/Settings.h"
#include "playback/PlaybackController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJSEngine>
#include <QLoggingCategory>
#include <QSignalBlocker>
#include <QUuid>
#include <QtEndian>
#include <utility>

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

namespace swiftify {
Q_LOGGING_CATEGORY(lcDiscord, "swiftify.discord", QtInfoMsg)
namespace {
DiscordPresence *s_instance = nullptr;

QStringList endpoints()
{
    QStringList result;
#ifdef Q_OS_WIN
    for (int i = 0; i < 10; ++i) result << QStringLiteral("discord-ipc-%1").arg(i);
#else
    QStringList directories {qEnvironmentVariable("XDG_RUNTIME_DIR"), qEnvironmentVariable("TMPDIR"),
                             qEnvironmentVariable("TMP"), qEnvironmentVariable("TEMP"),
                             QDir::tempPath(), QStringLiteral("/tmp")};
#ifdef Q_OS_LINUX
    // Launchers need not inherit Discord's runtime/TMP environment.
    directories << QStringLiteral("/run/user/%1").arg(getuid());
#endif
    result = discord::socketCandidates(directories);
#endif
    return result;
}

QJsonObject command(const QJsonValue &activity)
{
    return {{QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
            {QStringLiteral("args"), QJsonObject {{QStringLiteral("pid"), QCoreApplication::applicationPid()},
                                                {QStringLiteral("activity"), activity}}},
            {QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
}

QString fitText(const QString &value)
{
    QString result = value.trimmed().left(128);
    if (!result.isEmpty() && result.at(result.size() - 1).isHighSurrogate()) result.chop(1);
    result = result.trimmed();
    if (result.size() == 1) result += QChar(0x00a0);
    return result;
}
}

QByteArray discord::frame(quint32 opcode, const QByteArray &payload)
{
    QByteArray result(8, '\0');
    qToLittleEndian(opcode, result.data());
    qToLittleEndian(quint32(payload.size()), result.data() + 4);
    result += payload;
    return result;
}

QStringList discord::socketCandidates(const QStringList &roots)
{
    // Apply sandbox layouts to every root, not only XDG_RUNTIME_DIR: Discord
    // and wrappers may choose different runtime/TMP variables than Swiftify.
    static const QStringList suffixes {QString(),
        QStringLiteral("app/com.discordapp.Discord"),
        QStringLiteral("app/com.discordapp.DiscordCanary"),
        QStringLiteral("app/com.discordapp.DiscordPTB"),
        QStringLiteral("app/dev.vencord.Vesktop"),
        QStringLiteral(".flatpak/com.discordapp.Discord/xdg-run"),
        QStringLiteral(".flatpak/com.discordapp.DiscordCanary/xdg-run"),
        QStringLiteral(".flatpak/dev.vencord.Vesktop/xdg-run"),
        QStringLiteral("snap.discord"), QStringLiteral("snap.discord-canary")};
    QStringList directories;
    for (const QString &root : roots) {
        if (root.isEmpty() || !QDir::isAbsolutePath(root)) continue;
        for (const QString &suffix : suffixes) directories << QDir::cleanPath(QDir(root).filePath(suffix));
    }
    directories.removeDuplicates();
    QStringList result;
    for (int slot = 0; slot < 10; ++slot) {
        for (const auto &directory : directories) {
            const QString path = QDir(directory).filePath(QStringLiteral("discord-ipc-%1").arg(slot));
            if (QFileInfo::exists(path)) result << path;
        }
    }
    return result;
}

discord::FrameResult discord::takeFrame(QByteArray &buffer, quint32 &opcode, QByteArray &payload)
{
    if (buffer.size() < 8) return FrameResult::Incomplete;
    const quint32 size = qFromLittleEndian<quint32>(buffer.constData() + 4);
    if (size > maxFrameSize) return FrameResult::Invalid;
    if (buffer.size() < qsizetype(size) + 8) return FrameResult::Incomplete;
    opcode = qFromLittleEndian<quint32>(buffer.constData());
    payload = buffer.mid(8, size);
    buffer.remove(0, size + 8);
    return FrameResult::Complete;
}

DiscordPresence::DiscordPresence(PlaybackController *player, Settings *settings, QObject *parent,
                                 std::function<QStringList()> socketLocator)
    : QObject(parent), m_player(player), m_settings(settings),
      m_socketLocator(socketLocator ? std::move(socketLocator) : endpoints)
{
    s_instance = this;
    m_retry.setInterval(15000);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(5000);
    m_update.setSingleShot(true);
    m_update.setInterval(250);
    m_socket.setReadBufferSize(discord::maxFrameSize + 8);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        m_endpoints = m_socketLocator();
        m_endpoint = 0;
        if (m_endpoints.isEmpty()) fail(Failure::Unavailable);
        connectNext();
    });
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail(Failure::TimedOut);
    });
    connect(&m_update, &QTimer::timeout, this, &DiscordPresence::update);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        send(0, QJsonDocument(QJsonObject {{QStringLiteral("v"), 1},
            {QStringLiteral("client_id"), m_settings->discordApplicationId()}}).toJson(QJsonDocument::Compact));
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &DiscordPresence::readFrames);
    connect(&m_socket, &QLocalSocket::disconnected, this, &DiscordPresence::disconnected);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError error) {
        qCDebug(lcDiscord) << "IPC socket error" << int(error);
        fail(error == QLocalSocket::SocketAccessError ? Failure::AccessDenied : Failure::Unavailable);
    });
    connect(settings, &Settings::discordEnabledChanged, this, &DiscordPresence::configure);
    connect(settings, &Settings::discordApplicationIdChanged, this, &DiscordPresence::configure);
    connect(settings, &Settings::languageChanged, this, [this] {
        emit statusChanged();
        scheduleUpdate();
    });
    connect(player, &PlaybackController::mediaStateChanged, this, &DiscordPresence::scheduleUpdate);
    connect(player, &PlaybackController::currentSongChanged, this, &DiscordPresence::scheduleUpdate);
    connect(player, &PlaybackController::playingChanged, this, &DiscordPresence::scheduleUpdate);
    connect(player, &PlaybackController::loadingChanged, this, &DiscordPresence::scheduleUpdate);
    connect(player, &PlaybackController::positionChanged, this, &DiscordPresence::scheduleUpdate);
    connect(player, &PlaybackController::durationChanged, this, &DiscordPresence::scheduleUpdate);
    configure();
}

DiscordPresence::~DiscordPresence()
{
    clear();
    if (m_socket.bytesToWrite() > 0) m_socket.waitForBytesWritten(100);
    m_socket.abort();
    s_instance = nullptr;
}

DiscordPresence *DiscordPresence::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void DiscordPresence::setReady(bool ready)
{
    if (m_ready == ready) return;
    m_ready = ready;
    emit connectedChanged();
    emit statusChanged();
}

QString DiscordPresence::status() const
{
    if (!m_settings->discordEnabled()) return tr("Listening activity is private. Enable to share track, artist and progress with Discord.");
    if (m_settings->discordApplicationId().isEmpty()) return tr("Enter your Discord Application ID to connect.");
    if (m_ready) return tr("Connected to Discord");
    switch (m_failure) {
    case Failure::Unavailable: return tr("No reachable Discord IPC socket. Start Discord and enable its RPC integration.");
    case Failure::AccessDenied: return tr("Discord IPC access denied. Check sandbox permissions.");
    case Failure::TimedOut: return tr("Discord IPC handshake timed out.");
    case Failure::InvalidFrame: return tr("Discord returned an invalid IPC frame.");
    case Failure::RejectedConnection: return tr("Discord rejected the connection (code %1).").arg(m_failureCode);
    case Failure::RejectedActivity: return tr("Discord rejected the activity (code %1).").arg(m_failureCode);
    case Failure::None: return tr("Waiting for the Discord desktop app…");
    }
    return {};
}

void DiscordPresence::fail(Failure failure, int code)
{
    m_failure = failure;
    m_failureCode = code;
    // Never log Discord's raw error message or payload (it may echo track data).
    qCDebug(lcDiscord) << "IPC failure" << int(failure) << "code" << code;
    {
        const QSignalBlocker blocker(&m_socket);
        m_socket.abort();
    }
    disconnected();
    emit statusChanged();
}

void DiscordPresence::configure()
{
    m_retry.stop();
    m_deadline.stop();
    m_update.stop();
    clear();
    // Disconnect our socket callbacks while replacing an old connection.
    m_socket.flush();
    const QSignalBlocker blocker(&m_socket);
    m_socket.abort();
    setReady(false);
    m_published = false;
    m_buffer.clear();
    m_endpoints.clear();
    m_failure = Failure::None;
    m_failureCode = -1;
    emit statusChanged();
    if (!m_settings->discordEnabled() || m_settings->discordApplicationId().isEmpty()) return;
    m_endpoints = m_socketLocator();
    m_endpoint = 0;
    m_retry.start();
    QTimer::singleShot(0, this, &DiscordPresence::connectNext);
}

void DiscordPresence::connectNext()
{
    if (m_endpoints.isEmpty() && m_settings->discordEnabled() && !m_settings->discordApplicationId().isEmpty()) {
        m_failure = Failure::Unavailable;
        emit statusChanged();
    }
    if (!m_settings->discordEnabled() || m_settings->discordApplicationId().isEmpty()
        || m_socket.state() != QLocalSocket::UnconnectedState || m_endpoint >= m_endpoints.size()) return;
    m_buffer.clear();
    m_deadline.start();
    m_socket.connectToServer(m_endpoints.at(m_endpoint++));
}

void DiscordPresence::disconnected()
{
    m_deadline.stop();
    setReady(false);
    m_published = false;
    m_buffer.clear();
    if (m_settings->discordEnabled() && !m_settings->discordApplicationId().isEmpty() && !m_retry.isActive()) m_retry.start();
    QTimer::singleShot(0, this, &DiscordPresence::connectNext);
}

void DiscordPresence::send(quint32 opcode, const QByteArray &payload)
{
    if (m_socket.state() != QLocalSocket::ConnectedState) return;
    if (m_socket.bytesToWrite() > discord::maxFrameSize) {
        m_socket.abort();
        return;
    }
    m_socket.write(discord::frame(opcode, payload));
}

void DiscordPresence::readFrames()
{
    m_buffer += m_socket.readAll();
    quint32 opcode;
    QByteArray payload;
    while (true) {
        const auto result = discord::takeFrame(m_buffer, opcode, payload);
        if (result == discord::FrameResult::Incomplete) return;
        if (result == discord::FrameResult::Invalid) { fail(Failure::InvalidFrame); return; }
        if (opcode == 3) { send(4, payload); continue; } // ping / pong
        if (opcode == 4) continue;
        if (opcode == 2) {
            const auto error = QJsonDocument::fromJson(payload).object();
            fail(Failure::RejectedConnection, error.value(QStringLiteral("code")).toInt(-1));
            return;
        }
        if (opcode != 1) { fail(Failure::InvalidFrame); return; }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) { fail(Failure::InvalidFrame); return; }
        const QString event = document.object().value(QStringLiteral("evt")).toString();
        if (event == QLatin1StringView("ERROR")) {
            const int code = document.object().value(QStringLiteral("data")).toObject().value(QStringLiteral("code")).toInt(-1);
            fail(m_ready ? Failure::RejectedActivity : Failure::RejectedConnection, code);
            return;
        }
        if (event == QLatin1StringView("READY") && !m_ready) {
            m_deadline.stop();
            m_retry.stop();
            m_failure = Failure::None;
            m_failureCode = -1;
            setReady(true);
            update();
        }
    }
}

void DiscordPresence::scheduleUpdate()
{
    if (m_ready && !m_update.isActive()) m_update.start();
}

void DiscordPresence::clear()
{
    if (!m_ready || !m_published) return;
    send(1, QJsonDocument(command(QJsonValue::Null)).toJson(QJsonDocument::Compact));
    m_published = false;
}

void DiscordPresence::update()
{
    if (!m_ready) return;
    if (!m_player->hasSong() || m_player->isLoading()) { clear(); return; }
    const Song song = m_player->currentSong();
    if (song.uri == QLatin1StringView(PlaybackController::kLoadingTestUri)) { clear(); return; }
    const QString title = fitText(song.name);
    const bool playing = m_player->isPlaying();
    QJsonObject activity {{QStringLiteral("type"), 2},
                          {QStringLiteral("name"), QStringLiteral("Swiftify")},
                          {QStringLiteral("details"), title.isEmpty() ? QStringLiteral("Listening to music") : title},
                          {QStringLiteral("instance"), false}};
    const QString state = fitText(playing ? song.artists
        : song.artists.trimmed().isEmpty() ? tr("Paused") : tr("Paused") + QStringLiteral(" · ") + song.artists);
    if (!state.isEmpty()) activity.insert(QStringLiteral("state"), state);
    // Never expose local file paths, filenames, or private artwork URLs.

    if (!song.isLocalFile() && song.artworkUrl.scheme() == QLatin1StringView("https")
        && song.artworkUrl.host() == QLatin1StringView("i.scdn.co")) {
        QJsonObject assets {{QStringLiteral("large_image"), song.artworkUrl.toString()}};
        const QString album = fitText(song.albumName);
        if (!album.isEmpty()) assets.insert(QStringLiteral("large_text"), album);
        activity.insert(QStringLiteral("assets"), assets);
    } else if (song.isLocalFile()) {
        const QString cacheKey = song.artists + "|" + song.albumName + "|" + song.name;
        if (m_coverCache.contains(cacheKey)) {
            const QString coverUrl = m_coverCache.value(cacheKey);
            if (!coverUrl.isEmpty()) {
                QJsonObject assets {{QStringLiteral("large_image"), coverUrl}};
                const QString album = fitText(song.albumName);
                if (!album.isEmpty()) assets.insert(QStringLiteral("large_text"), album);
                activity.insert(QStringLiteral("assets"), assets);
            }
        } else {
            fetchDeezerCover(cacheKey, song.artists, song.albumName, song.name);
            return;
        }
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 position = m_player->livePositionMs();
    // The output's first duration arrives on the next player poll. Metadata
    // already has it, so the initial activity needn't lose its progress bar.
    const qint64 duration = m_player->durationMs() > 0 ? m_player->durationMs() : song.durationMs;
    // Publish on metadata/track changes and seeks, not every progress tick.
    
    QJsonObject metadata = activity;
    metadata.insert(QStringLiteral("track"), QString::number(song.instanceId));
    metadata.insert(QStringLiteral("duration"), duration);
    metadata.insert(QStringLiteral("playing"), playing);
    
    if (m_published && metadata == m_lastMetadata
        && (!playing || qAbs(position - (m_lastPosition + now - m_lastUpdate)) < 2000)) return;
        
    if (playing && duration > position) {
        activity.insert(QStringLiteral("timestamps"), QJsonObject {
            {QStringLiteral("start"), (now - position) / 1000},
            {QStringLiteral("end"), (now + duration - position) / 1000}});
    }
    
    send(1, QJsonDocument(command(activity)).toJson(QJsonDocument::Compact));
    
    m_lastMetadata = metadata;
    m_lastPosition = position;
    m_lastUpdate = now;
    m_published = true;
}

void DiscordPresence::fetchDeezerCover(const QString &cacheKey, const QString &artist, const QString &album, const QString &title)
{
    if (m_pendingFetches.contains(cacheKey)) return;
    m_pendingFetches.insert(cacheKey);

    QUrl url("https://api.deezer.com/search");   
    QUrlQuery query;
    query.addQueryItem("q", QStringLiteral("%1 %2").arg(artist, title));
    query.addQueryItem("limit", "10");
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = m_network.get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, cacheKey, album]() {
        reply->deleteLater();
        m_pendingFetches.remove(cacheKey);

        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray responseData = reply->readAll();
            const QJsonObject doc = QJsonDocument::fromJson(responseData).object();
            const QJsonArray data = doc.value(QStringLiteral("data")).toArray();
            
            if (!data.isEmpty()) {
                QString coverUrl;
                for (const QJsonValue &value : data) {
                    const QJsonObject track = value.toObject();
                    const QJsonObject albumObj = track.value(QStringLiteral("album")).toObject();
                    const QString albumTitle = albumObj.value(QStringLiteral("title")).toString();
                    
                    if (albumTitle.compare(album, Qt::CaseInsensitive) == 0) {
                        coverUrl = albumObj.value(QStringLiteral("cover_xl")).toString();
                        if (coverUrl.isEmpty()) coverUrl = albumObj.value(QStringLiteral("cover_big")).toString();
                        if (coverUrl.isEmpty()) coverUrl = albumObj.value(QStringLiteral("cover")).toString();
                        break;
                    }
                }

                if (coverUrl.isEmpty()) {
                    const QJsonObject firstTrack = data.first().toObject();
                    const QJsonObject firstAlbum = firstTrack.value(QStringLiteral("album")).toObject();
                    coverUrl = firstAlbum.value(QStringLiteral("cover_xl")).toString();
                    if (coverUrl.isEmpty()) coverUrl = firstAlbum.value(QStringLiteral("cover_big")).toString();
                    if (coverUrl.isEmpty()) coverUrl = firstAlbum.value(QStringLiteral("cover")).toString();
                }

                if (!coverUrl.isEmpty()) {
                    m_coverCache.insert(cacheKey, coverUrl);
                    scheduleUpdate();
                    return;
                }
            }
        }

        m_coverCache.insert(cacheKey, QString());
        scheduleUpdate();
    });
}

} // namespace swiftify

