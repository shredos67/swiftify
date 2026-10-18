#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <QNetworkAccessManager>
#include <QHash>
#include <QSet>

class QQmlEngine;
class QJSEngine;

namespace swiftify {

class PlaybackController;
class Settings;

namespace discord {
constexpr quint32 maxFrameSize = 1024 * 1024;
QByteArray frame(quint32 opcode, const QByteArray &payload);
// A complete frame is removed from buffer. Invalid lengths abort the connection.
enum class FrameResult { Incomplete, Complete, Invalid };
FrameResult takeFrame(QByteArray &buffer, quint32 &opcode, QByteArray &payload);
QStringList socketCandidates(const QStringList &roots);
}

/// Local Discord IPC only. No token, SDK, external service, or background thread.
/// Presence is opt-in; pauses keep a labeled track without a ticking timer.
/// Disabling clears it and closes the connection.
class DiscordPresence : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Discord)
    QML_SINGLETON
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged FINAL)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)
public:
    DiscordPresence(PlaybackController *player, Settings *settings, QObject *parent = nullptr,
                    std::function<QStringList()> socketLocator = {});
    ~DiscordPresence() override;
    static DiscordPresence *create(QQmlEngine *, QJSEngine *);
    bool connected() const { return m_ready; }
    QString status() const;

signals:
    void connectedChanged();
    void statusChanged();

private:
    void configure();
    void connectNext();
    void disconnected();
    void readFrames();
    void send(quint32 opcode, const QByteArray &payload);
    void scheduleUpdate();
    void update();
    void clear();
    void setReady(bool ready);
    enum class Failure { None, Unavailable, AccessDenied, TimedOut, InvalidFrame, RejectedConnection, RejectedActivity };
    void fail(Failure failure, int code = -1);

    PlaybackController *m_player;
    Settings *m_settings;
    std::function<QStringList()> m_socketLocator;
    Failure m_failure = Failure::None;
    int m_failureCode = -1;
    QLocalSocket m_socket;
    QTimer m_retry;
    QTimer m_deadline;
    QTimer m_update;
    QStringList m_endpoints;
    qsizetype m_endpoint = 0;
    QByteArray m_buffer;
    bool m_ready = false;
    bool m_published = false;
    QJsonObject m_lastMetadata;
    qint64 m_lastPosition = 0;
    qint64 m_lastUpdate = 0;

    // NEW: Network and caching for Deezer cover art
    QNetworkAccessManager m_network;
    QHash<QString, QString> m_coverCache;
    QSet<QString> m_pendingFetches;
    void fetchDeezerCover(const QString &cacheKey, const QString &artist, const QString &album, const QString &title);
};

} // namespace swiftify