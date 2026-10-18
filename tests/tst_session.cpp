#include "FakeCore.h"

#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "core/Async.h"
#include "library/LibraryStore.h"
#include "spotify/SpotifyJson.h"
#include "spotify/CredentialStore.h"
#include "spotify/OAuthFlow.h"
#include "spotify/SpotifySession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

using namespace swiftify;
using swiftify::testing::FakeCore;

namespace {

/// a token endpoint's answer, given when the test lets it go
class CannedReply : public QNetworkReply {
    Q_OBJECT

public:
    CannedReply(const QNetworkRequest &request, int status, QByteArray body, QObject *parent)
        : QNetworkReply(parent)
        , m_body(std::move(body))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::PostOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (status >= 400) {
            setError(QNetworkReply::ProtocolInvalidOperationError, QStringLiteral("HTTP %1").arg(status));
        }
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void finishNow()
    {
        setFinished(true);
        emit metaDataChanged();
        emit readyRead();
        emit finished();
    }

    void abort() override {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const qint64 count = std::min<qint64>(maximum, m_body.size() - m_offset);
        memcpy(data, m_body.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_body;
    qint64 m_offset = 0;
};

/// spotify's accounts service, as far as tokens go: every refresh rotates
/// the refresh token, and spending one it already replaced revokes the whole
/// login it belonged to
class FakeAccounts : public QNetworkAccessManager {
    Q_OBJECT

public:
    static constexpr auto kHost = "accounts.test";

    int expiresIn = 3600;
    /// replies wait for release() rather than coming back on their own
    bool holding = false;
    int requests = 0;
    QStringList spent;
    QSet<QString> revokedLogins;
    /// the web api's answers by path. any other path is not found
    QHash<QString, QByteArray> api;
    /// every web api call, as its method, path and query
    QStringList apiCalls;

    /// a new login, and its first refresh token
    QString login()
    {
        return issue(QStringLiteral("login%1").arg(++m_logins));
    }
    bool isLive(const QString &refreshToken) const
    {
        return m_live.contains(refreshToken) && !revokedLogins.contains(m_loginOf.value(refreshToken));
    }
    void release()
    {
        for (const QPointer<CannedReply> &reply : std::exchange(m_held, {})) {
            if (reply) {
                reply->finishNow();
            }
        }
    }

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *data) override
    {
        if (request.url().host() == QLatin1StringView("api.spotify.com")) {
            const QString query = request.url().query(QUrl::FullyDecoded);
            apiCalls << QStringLiteral("%1 %2%3").arg(
                request.attribute(QNetworkRequest::CustomVerbAttribute).toString(), request.url().path(),
                query.isEmpty() ? QString() : u'?' + query);
            const auto found = api.constFind(request.url().path());
            return found == api.cend() ? reply(request, 404, R"({"error":{"status":404,"message":"Not found"}})")
                                       : reply(request, 200, *found);
        }
        if (request.url().host() != QLatin1StringView(kHost)) {
            return QNetworkAccessManager::createRequest(operation, request, data);
        }
        ++requests;
        const QUrlQuery form(QString::fromUtf8(data ? data->readAll() : QByteArray()));
        const QString grant = form.queryItemValue(QStringLiteral("grant_type"));
        if (grant == QLatin1StringView("authorization_code")) {
            if (form.queryItemValue(QStringLiteral("code")) != QLatin1StringView("approved")) {
                return reply(request, 400, R"({"error":"invalid_grant","error_description":"Invalid authorization code"})");
            }
            return issued(request, login());
        }
        const QString token = form.queryItemValue(QStringLiteral("refresh_token"));
        spent << token;
        const QString owner = m_loginOf.value(token);
        if (!isLive(token)) {
            // a token spotify already replaced: the login goes with it
            if (!owner.isEmpty()) {
                revokedLogins.insert(owner);
            }
            return reply(request, 400, R"({"error":"invalid_grant","error_description":"Refresh token revoked"})");
        }
        m_live.remove(token);
        return issued(request, issue(owner));
    }

private:
    QString issue(const QString &owner)
    {
        const QString token = QStringLiteral("refresh%1").arg(++m_tokens);
        m_live.insert(token);
        m_loginOf.insert(token, owner);
        return token;
    }

    QNetworkReply *issued(const QNetworkRequest &request, const QString &refreshToken)
    {
        const QJsonObject body {{QStringLiteral("access_token"), QStringLiteral("access%1").arg(++m_access)},
                                {QStringLiteral("refresh_token"), refreshToken},
                                {QStringLiteral("expires_in"), expiresIn}};
        return reply(request, 200, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }

    QNetworkReply *reply(const QNetworkRequest &request, int status, const QByteArray &body)
    {
        auto *answer = new CannedReply(request, status, body, this);
        if (holding) {
            m_held.append(answer);
        } else {
            QTimer::singleShot(0, answer, [answer] { answer->finishNow(); });
        }
        return answer;
    }

    int m_logins = 0;
    int m_tokens = 0;
    int m_access = 0;
    QSet<QString> m_live;
    QHash<QString, QString> m_loginOf;
    QList<QPointer<CannedReply>> m_held;
};

class Gate : public QObject {
    Q_OBJECT

signals:
    void opened();
};

/// a keychain that can be made slow: a write waits until the test opens it
class SlowStore : public CredentialStore {
public:
    bool holding = false;
    QHash<Account, StoredCredentials> items;
    Gate gate;

    QCoro::Task<std::optional<StoredCredentials>> load(Account account) override
    {
        const auto found = items.constFind(account);
        co_return found == items.cend() ? std::nullopt : std::optional(*found);
    }
    QCoro::Task<> save(Account account, StoredCredentials credentials) override
    {
        if (holding) {
            co_await qCoro(&gate, &Gate::opened);
        }
        items.insert(account, credentials);
    }
    QCoro::Task<> removeAll() override
    {
        items.clear();
        co_return;
    }
};

QCoro::Task<> fetchLibraryToken(SpotifySession &session, QStringList &tokens)
{
    try {
        tokens << co_await session.withLibraryAccessToken<QString>(
            [](QString accessToken) -> QCoro::Task<QString> { co_return accessToken; });
    } catch (const SpotifyError &error) {
        tokens << QStringLiteral("error: ") + error.message();
    }
}

void settle()
{
    for (int pass = 0; pass < 10; ++pass) {
        QCoreApplication::processEvents();
    }
}

} // namespace

/// spotify rotates a refresh token every time one is spent and revokes the
/// login when one it already replaced is spent again. the session spends them
/// one at a time, as seldom as it can, keeps every one it is given, and signs
/// itself in again when spotify revokes a login all the same.
class SessionTest : public QObject {
    Q_OBJECT

private:
    struct Rig {
        ErrorCenter errors {nullptr};
        SlowStore store;
        FakeAccounts accounts;
        FakeCore core;
        SpotifySession session {&core, &store, &accounts, &errors};
        QList<QUrl> opened;

        Rig()
        {
            OAuthFlow::setTokenEndpoint(QUrl(QStringLiteral("http://%1/api/token").arg(FakeAccounts::kHost)));
            session.setBackgroundOpener([this](const QUrl &url) { opened << url; });
        }

        void storeLibrary(const QString &refreshToken, const QString &accessToken = {}, int minutesLeft = 0)
        {
            store.items.insert(CredentialStore::Account::Library,
                               {QStringLiteral("library-client"), refreshToken, OAuthFlow::kLibraryAuthorizationVersion,
                                {}, accessToken, QDateTime::currentDateTimeUtc().addSecs(minutesLeft * 60)});
        }
        void storePlayback(const QString &refreshToken, const QString &login = {})
        {
            store.items.insert(CredentialStore::Account::Playback,
                               {QString::fromLatin1(OAuthFlow::kPlaybackClientId), refreshToken,
                                OAuthFlow::kPlaybackAuthorizationVersion, login, {}, {}});
        }
        StoredCredentials stored(CredentialStore::Account account) const { return store.items.value(account); }
    };

private slots:
    void savedLoginsKeepWhatTheyHold()
    {
        const StoredCredentials credentials {QStringLiteral("client"), QStringLiteral("refresh"), 2,
                                             QStringLiteral("{\"username\":\"me\"}"), QStringLiteral("access"),
                                             QDateTime::fromMSecsSinceEpoch(1'800'000'000'000, QTimeZone::UTC)};
        const auto read = StoredCredentials::fromJson(credentials.toJson());
        QVERIFY(read);
        QCOMPARE(read->refreshToken, credentials.refreshToken);
        QCOMPARE(read->playbackLogin, credentials.playbackLogin);
        QCOMPARE(read->accessToken, credentials.accessToken);
        QCOMPARE(read->accessTokenExpiration, credentials.accessTokenExpiration);
        // what an older build wrote still reads
        const auto old = StoredCredentials::fromJson(R"({"clientID":"c","refreshToken":"r","authorizationVersion":2})");
        QVERIFY(old);
        QVERIFY(old->playbackLogin.isEmpty());
        QVERIFY(!old->accessTokenExpiration.isValid());
        // a playback login whose refresh token is gone still plays
        QVERIFY(StoredCredentials::fromJson(R"({"clientID":"c","refreshToken":"","playbackLogin":"x"})"));
        QVERIFY(!StoredCredentials::fromJson(R"({"clientID":"c","refreshToken":""})"));
    }

    void aLaunchSpendsNoRefreshTokenWhileItsLoginsHold()
    {
        Rig rig;
        rig.storeLibrary(rig.accounts.login(), QStringLiteral("still-good"), 30);
        rig.storePlayback(rig.accounts.login(), QStringLiteral("librespot-login"));
        rig.session.restoreSavedSessions();
        settle();
        // librespot's own login, no token
        const auto call = rig.core.take(QStringLiteral("connectStored"));
        QVERIFY(call);
        QCOMPARE(call->argument, QStringLiteral("librespot-login"));
        call->succeed(QStringLiteral("librespot-login"));
        QTRY_VERIFY(rig.session.isConnected());
        QVERIFY(rig.session.hasLibraryToken());
        QVERIFY(!rig.session.isEvaluatingStoredLogin());
        QCOMPARE(rig.accounts.requests, 0);
    }

    void playbackKeepsLibrespotsLoginAndReconnectsWithIt()
    {
        Rig rig;
        rig.storePlayback(rig.accounts.login());
        rig.session.restoreSavedSessions();
        settle();
        // no login of librespot's yet: the token, refreshed once
        auto call = rig.core.take(QStringLiteral("connect"));
        QVERIFY(call);
        QCOMPARE(rig.accounts.requests, 1);
        call->succeed(QStringLiteral("login-a"));
        QTRY_VERIFY(rig.session.isConnected());
        settle();
        const StoredCredentials kept = rig.stored(CredentialStore::Account::Playback);
        QCOMPARE(kept.playbackLogin, QStringLiteral("login-a"));
        QVERIFY(rig.accounts.isLive(kept.refreshToken));

        // a reconnect, as after a failed play or a new output, spends nothing
        bool done = false;
        const auto reconnect = [&](bool force) -> QCoro::Task<> {
            co_await rig.session.ensureFreshPlaybackConnection(force);
            done = true;
        };
        detach(reconnect(true));
        settle();
        call = rig.core.take(QStringLiteral("connectStored"));
        QVERIFY(call);
        call->succeed(QStringLiteral("login-a"));
        QTRY_VERIFY(done);
        QCOMPARE(rig.accounts.requests, 1);

        // and when librespot will not take its login, the access token it
        // still has does, unrefreshed
        done = false;
        detach(reconnect(true));
        settle();
        call = rig.core.take(QStringLiteral("connectStored"));
        QVERIFY(call);
        call->fail(QStringLiteral("Login failed with reason: Bad credentials"));
        settle();
        call = rig.core.take(QStringLiteral("connect"));
        QVERIFY(call);
        QCOMPARE(call->argument, QStringLiteral("access1"));
        call->succeed(QStringLiteral("login-b"));
        QTRY_VERIFY(done);
        QCOMPARE(rig.accounts.requests, 1);
        QTRY_COMPARE(rig.stored(CredentialStore::Account::Playback).playbackLogin, QStringLiteral("login-b"));
    }

    /// the revocations seen: a refresh spent the old token while the new one
    /// was still being written, as every request after a long sleep did
    void requestsDuringASlowWriteSpendTheNewToken()
    {
        Rig rig;
        // tokens that are always about to run out, so every request refreshes
        rig.accounts.expiresIn = 30;
        rig.storeLibrary(rig.accounts.login());
        rig.session.restoreSavedSessions();
        QTRY_VERIFY(rig.session.hasLibraryToken());
        QCOMPARE(rig.accounts.requests, 1);

        rig.store.holding = true;
        rig.accounts.holding = true;
        QStringList tokens;
        detach(fetchLibraryToken(rig.session, tokens));
        settle();
        QCOMPARE(rig.accounts.requests, 2);
        // spotify answers; its token is being written when more requests come
        rig.accounts.release();
        settle();
        for (int request = 0; request < 3; ++request) {
            detach(fetchLibraryToken(rig.session, tokens));
        }
        settle();
        rig.accounts.release();
        QTRY_COMPARE(tokens.size(), 4);
        for (const QString &token : std::as_const(tokens)) {
            QVERIFY2(token.startsWith(QLatin1StringView("access")), qPrintable(token));
        }
        QVERIFY(rig.accounts.revokedLogins.isEmpty());
        // never the same token twice
        QCOMPARE(QSet<QString>(rig.accounts.spent.begin(), rig.accounts.spent.end()).size(), rig.accounts.spent.size());

        // and what is written, once written, is the token in use
        rig.store.holding = false;
        for (int write = 0; write < 4; ++write) {
            emit rig.store.gate.opened();
            settle();
        }
        QVERIFY(rig.accounts.isLive(rig.stored(CredentialStore::Account::Library).refreshToken));
    }

    void quittingWaitsForARotatedTokenToBeKept()
    {
        Rig rig;
        rig.accounts.expiresIn = 30;
        rig.storeLibrary(rig.accounts.login());
        rig.session.restoreSavedSessions();
        QTRY_VERIFY(rig.session.hasLibraryToken());

        rig.accounts.holding = true;
        QStringList tokens;
        detach(fetchLibraryToken(rig.session, tokens));
        settle();
        // the app quits while spotify is answering
        QTimer::singleShot(50, &rig.accounts, [&] { rig.accounts.release(); });
        rig.session.flushCredentialSaves();
        QVERIFY(rig.accounts.isLive(rig.stored(CredentialStore::Account::Library).refreshToken));
    }

    void aRevokedLoginSignsItselfInAgain()
    {
        Rig rig;
        const QString revoked = rig.accounts.login();
        rig.accounts.revokedLogins.insert(QStringLiteral("login1"));
        rig.storeLibrary(revoked);
        rig.session.restoreSavedSessions();
        // the login page opens in the background, and nothing is shown or
        // asked meanwhile
        QTRY_COMPARE(rig.opened.size(), 1);
        QVERIFY(rig.session.isRepairingLogin());
        QVERIFY(!rig.session.isLoginInProgress());
        QVERIFY(rig.session.isEvaluatingStoredLogin());
        QVERIFY(rig.errors.message().isEmpty());

        // spotify, already approved, sends the browser straight back
        const QString state = QUrlQuery(rig.opened.first()).queryItemValue(QStringLiteral("state"));
        QTcpSocket browser;
        browser.connectToHost(QHostAddress::LocalHost, OAuthFlow::kRedirectPort);
        QVERIFY(browser.waitForConnected(2000));
        browser.write(QStringLiteral("GET /login?code=approved&state=%1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
                          .arg(state)
                          .toUtf8());
        browser.flush();
        QTRY_VERIFY(rig.session.isLibraryConnected());
        QVERIFY(!rig.session.isRepairingLogin());
        QVERIFY(!rig.session.isEvaluatingStoredLogin());
        QVERIFY(rig.errors.message().isEmpty());
        QTRY_VERIFY(rig.accounts.isLive(rig.stored(CredentialStore::Account::Library).refreshToken));
    }

    void aRevokedPlaybackTokenDoesNotStopPlay()
    {
        Rig rig;
        const QString token = rig.accounts.login();
        rig.accounts.revokedLogins.insert(QStringLiteral("login1"));
        rig.storePlayback(token, QStringLiteral("librespot-login"));
        rig.session.restoreSavedSessions();
        settle();
        const auto call = rig.core.take(QStringLiteral("connectStored"));
        QVERIFY(call);
        call->succeed(QStringLiteral("librespot-login"));
        QTRY_VERIFY(rig.session.isConnected());
        QCOMPARE(rig.accounts.requests, 0);
        QVERIFY(rig.opened.isEmpty());
    }

    void aPlaylistSomeoneElseMadeIsReadThroughThePlaybackSession()
    {
        Rig rig;
        QTemporaryDir directory;
        Settings settings(directory.filePath(QStringLiteral("swiftify.conf")));
        LibraryStore library(&rig.session, &settings, &rig.errors);
        const auto page = [](const char *uri, const char *name) {
            const QJsonObject track {{QStringLiteral("type"), QStringLiteral("track")},
                                     {QStringLiteral("uri"), QLatin1StringView(uri)},
                                     {QStringLiteral("name"), QLatin1StringView(name)}};
            const QJsonArray items {QJsonObject {{QStringLiteral("item"), track}}};
            return QJsonDocument(QJsonObject {{QStringLiteral("items"), items}}).toJson(QJsonDocument::Compact);
        };
        const auto names = [&library] {
            QStringList result;
            for (const Song &song : library.playlistSongs()->songs()) {
                result << song.name;
            }
            return result;
        };
        rig.accounts.api.insert(QStringLiteral("/v1/me"), R"({"id":"me","display_name":"Me"})");
        rig.accounts.api.insert(QStringLiteral("/v1/playlists/mine/items"), page("spotify:track:a", "From The Web Api"));
        rig.storeLibrary(rig.accounts.login(), QStringLiteral("access"), 60);
        rig.storePlayback(rig.accounts.login(), QStringLiteral("librespot-login"));
        rig.session.restoreSavedSessions();
        QTRY_COMPARE(library.profileId(), QStringLiteral("me"));
        settle();
        const auto connect = rig.core.take(QStringLiteral("connectStored"));
        QVERIFY(connect);
        connect->succeed(QStringLiteral("librespot-login"));
        QTRY_VERIFY(rig.session.isConnected());

        // the web api only gives its name and cover
        Playlist theirs;
        theirs.id = QStringLiteral("theirs");
        theirs.uri = QStringLiteral("spotify:playlist:theirs");
        theirs.ownerId = QStringLiteral("someone");
        library.selectPlaylist(theirs);
        settle();
        const auto read = rig.core.take(QStringLiteral("playlist"));
        QVERIFY(read);
        QCOMPARE(read->argument, theirs.uri);
        read->succeed(QString::fromUtf8(page("spotify:track:b", "Through The Session")));
        QTRY_COMPARE(names(), QStringList {QStringLiteral("Through The Session")});
        QVERIFY(!library.isLoadingSongs());

        Playlist mine = theirs;
        mine.id = QStringLiteral("mine");
        mine.uri = QStringLiteral("spotify:playlist:mine");
        mine.ownerId = QStringLiteral("me");
        library.selectPlaylist(mine);
        QTRY_COMPARE(names(), QStringList {QStringLiteral("From The Web Api")});
        QVERIFY(!rig.core.take(QStringLiteral("playlist")));
    }

    void anArtistsTopSongsAreReadThroughThePlaybackSession()
    {
        Rig rig;
        QTemporaryDir directory;
        Settings settings(directory.filePath(QStringLiteral("swiftify.conf")));
        LibraryStore library(&rig.session, &settings, &rig.errors);
        const auto names = [&library] {
            QStringList result;
            for (const Song &song : library.artistTopSongs()->songs()) {
                result << song.name;
            }
            return result;
        };
        rig.accounts.api.insert(QStringLiteral("/v1/me"), R"({"id":"me","display_name":"Me"})");
        rig.accounts.api.insert(QStringLiteral("/v1/artists/sade/albums"),
                                R"({"items":[{"id":"diamond","name":"Diamond Life","uri":"spotify:album:diamond"}]})");
        rig.accounts.api.insert(QStringLiteral("/v1/artists/gone/albums"),
                                R"({"items":[{"id":"left","name":"Left Behind","uri":"spotify:album:left"}]})");
        rig.storeLibrary(rig.accounts.login(), QStringLiteral("access"), 60);
        rig.storePlayback(rig.accounts.login(), QStringLiteral("librespot-login"));
        rig.session.restoreSavedSessions();
        QTRY_COMPARE(library.profileId(), QStringLiteral("me"));
        settle();
        const auto connect = rig.core.take(QStringLiteral("connectStored"));
        QVERIFY(connect);
        connect->succeed(QStringLiteral("librespot-login"));
        QTRY_VERIFY(rig.session.isConnected());

        Artist sade;
        sade.id = QStringLiteral("sade");
        sade.name = QStringLiteral("Sade");
        library.selectArtist(sade);
        settle();
        const auto read = rig.core.take(QStringLiteral("topTracks"));
        QVERIFY(read);
        QCOMPARE(read->argument, QStringLiteral("spotify:artist:sade"));
        read->succeed(QStringLiteral(
            R"({"tracks":[{"type":"track","uri":"spotify:track:a","name":"Smooth Operator"},)"
            R"({"type":"track","uri":"spotify:track:b","name":"Your Love Is King"}]})"));
        QTRY_COMPARE(names(), (QStringList {QStringLiteral("Smooth Operator"), QStringLiteral("Your Love Is King")}));
        QTRY_COMPARE(library.artistAlbums()->items().size(), 1);
        QVERIFY(!rig.accounts.apiCalls.join(u'\n').contains(QStringLiteral("top-tracks")));

        // the page still opens without them
        Artist gone;
        gone.id = QStringLiteral("gone");
        gone.name = QStringLiteral("Gone");
        library.selectArtist(gone);
        settle();
        const auto failed = rig.core.take(QStringLiteral("topTracks"));
        QVERIFY(failed);
        failed->fail(QStringLiteral("Spotify would not send the artist"));
        QTRY_COMPARE(library.artistAlbums()->items().size(), 1);
        QCOMPARE(library.artistAlbums()->items().first().name, QStringLiteral("Left Behind"));
        QVERIFY(names().isEmpty());
        QCOMPARE(rig.errors.message(), QString());
    }

    void aPlaylistIsUnfollowedThroughTheLibrary()
    {
        Rig rig;
        QTemporaryDir directory;
        Settings settings(directory.filePath(QStringLiteral("swiftify.conf")));
        LibraryStore library(&rig.session, &settings, &rig.errors);
        rig.accounts.api.insert(QStringLiteral("/v1/me"), R"({"id":"me","display_name":"Me"})");
        rig.accounts.api.insert(QStringLiteral("/v1/me/library"), {});
        rig.accounts.api.insert(QStringLiteral("/v1/me/playlists"), R"({"items":[]})");
        rig.storeLibrary(rig.accounts.login(), QStringLiteral("access"), 60);
        rig.session.restoreSavedSessions();
        QTRY_COMPARE(library.profileId(), QStringLiteral("me"));
        QVERIFY(rig.session.hasFullLibraryAccess());

        QSignalSpy deleted(&library, &LibraryStore::playlistDeleted);
        Playlist theirs;
        theirs.id = QStringLiteral("theirs");
        theirs.uri = QStringLiteral("spotify:playlist:theirs");
        theirs.name = QStringLiteral("Theirs");
        theirs.ownerId = QStringLiteral("someone");
        library.deletePlaylist(theirs);
        QTRY_COMPARE(deleted.size(), 1);
        QCOMPARE(deleted.first().first().toString(), theirs.id);
        QVERIFY(rig.accounts.apiCalls.contains(QStringLiteral("DELETE /v1/me/library?uris=spotify:playlist:theirs")));
        QVERIFY(!rig.accounts.apiCalls.join(u'\n').contains(QStringLiteral("followers")));
        QCOMPARE(rig.errors.message(), QString());
    }

    void aPlaylistDescriptionReadsAsPlainText()
    {
        const auto description = [](const char *html) {
            return json::playlist(QJsonObject {{QStringLiteral("id"), QStringLiteral("x")},
                                               {QStringLiteral("description"), QLatin1StringView(html)}})
                .description;
        };
        QCOMPARE(description("JoJo&#x27;s ops &amp; some other bangers."),
                 QStringLiteral("JoJo's ops & some other bangers."));
        QCOMPARE(description(R"(more at <a href="spotify:playlist:y">Part 2</a> &#39;n &quot;more&quot;)"),
                 QStringLiteral("more at Part 2 'n \"more\""));
        // not an entity, left as it is
        QCOMPARE(description("rock &roll; &bogus; a&b"), QStringLiteral("rock &roll; &bogus; a&b"));
    }

    void aLogoutForgetsALoginStillBeingWritten()
    {
        Rig rig;
        rig.storeLibrary(rig.accounts.login());
        rig.store.holding = true;
        rig.session.restoreSavedSessions();
        QTRY_VERIFY(rig.session.hasLibraryToken());
        QSignalSpy loggedOut(&rig.session, &SpotifySession::loggedOut);
        rig.session.logOut();
        settle();
        rig.store.holding = false;
        emit rig.store.gate.opened();
        QTRY_COMPARE(loggedOut.count(), 1);
        settle();
        QVERIFY(rig.store.items.isEmpty());
    }
};

QTEST_GUILESS_MAIN(SessionTest)
#include "tst_session.moc"
