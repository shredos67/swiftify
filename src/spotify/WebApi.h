#pragma once

#include "spotify/SpotifyTypes.h"

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QCoroTask>

#include <functional>

class QNetworkAccessManager;

namespace swiftify {

/// the spotify web api calls the library needs, as SpotifyWebAPI.swift makes
/// them. every call throws SpotifyError on failure; a 401 surfaces as Unauthorized
/// so the caller can refresh its token and retry once.
class WebApi {
public:
    static constexpr int kMaximumCoverBytes = 256'000;

    explicit WebApi(QNetworkAccessManager *network);

    QCoro::Task<Profile> profile(QString accessToken);
    QCoro::Task<QList<Playlist>> playlists(QString accessToken);
    QCoro::Task<QList<Song>> playlistSongs(QString playlistId, QString accessToken);
    QCoro::Task<QList<Song>> savedSongs(QString accessToken);
    /// how many songs are saved, from one page of one: the count without the list
    QCoro::Task<int> savedSongTotal(QString accessToken);
    QCoro::Task<QList<Song>> recentlyPlayed(QString accessToken);
    QCoro::Task<SearchResults> search(QString query, QString accessToken);
    QCoro::Task<Song> song(QString trackId, QString accessToken);
    QCoro::Task<QList<Song>> albumSongs(Album album, QString accessToken);
    QCoro::Task<QList<Album>> artistAlbums(Artist artist, QString accessToken);

    QCoro::Task<Playlist> createPlaylist(QString name, QString description, bool isPublic, QString accessToken);
    QCoro::Task<> updatePlaylist(QString id, QString name, QString description, bool isPublic, QString accessToken);
    QCoro::Task<> addToPlaylist(QString songUri, QString playlistId, QString accessToken);
    QCoro::Task<> removeFromPlaylist(QString songUri, QString playlistId, QString accessToken);
    /// moves a range, in spotify's indexes
    QCoro::Task<> reorderPlaylist(QString playlistId, int rangeStart, int insertBefore, int length,
                                  QString accessToken);
    /// a jpeg of at most kMaximumCoverBytes. needs the ugc-image-upload scope.
    QCoro::Task<> setPlaylistImage(QString playlistId, QByteArray jpeg, QString accessToken);
    /// spotify has no delete. removing a playlist from a library is an unfollow.
    QCoro::Task<> deletePlaylist(QString id, QString accessToken);
    /// saves or removes any spotify uri in the library
    QCoro::Task<> setSaved(bool saved, QString uri, QString accessToken);
    /// at most 40 uris are checked
    QCoro::Task<QHash<QString, bool>> savedStatuses(QStringList uris, QString accessToken);

    static QUrl apiUrl(const QString &path, const QUrlQuery &query = {});

private:
    QCoro::Task<QByteArray> requestData(QUrl url, QByteArray method, QString accessToken,
                                        QByteArray body = {}, QByteArray contentType = {});
    QCoro::Task<QJsonDocument> requestJson(QUrl url, QByteArray method, QString accessToken,
                                           QByteArray body = {}, QByteArray contentType = {});
    /// follows `next` until it runs out. a page url seen twice is an error,
    /// rather than a loop.
    QCoro::Task<> collectPages(QUrl firstUrl, QString accessToken,
                               std::function<void(const QJsonObject &page)> onPage);
    /// every page of a list that pages by offset, the first alone for its
    /// total and the rest a few at a time, handed to `onPage` in order. a list
    /// of a thousand songs comes in the time of five pages, not twenty.
    QCoro::Task<> collectOffsetPages(QString path, QUrlQuery query, QString accessToken,
                                     std::function<void(const QJsonObject &page)> onPage);
    /// one page, waited for and asked again once if spotify says to slow down
    QCoro::Task<QJsonObject> requestPage(QUrl url, QString accessToken);

    QNetworkAccessManager *m_network;
};

} // namespace swiftify
