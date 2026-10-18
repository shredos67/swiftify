#include "library/LibraryStore.h"

#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "core/PlaybackCore.h"
#include "library/LocalLibrary.h"
#include "playback/PlaybackController.h"
#include "spotify/SpotifyJson.h"
#include "spotify/SpotifySession.h"

#include <QCollator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QCoroTimer>

#include <algorithm>
#include <chrono>

namespace swiftify {

Q_LOGGING_CATEGORY(lcLibrary, "swiftify.library")

namespace {

LibraryStore *s_instance = nullptr;

const QString kPinnedKey = QStringLiteral("pinnedPlaylistIDs");
const QString kSearchHistoryKey = QStringLiteral("searchPlaybackHistory");
/// the saved songs' file in the song list cache: no playlist id looks like it
const QString kSavedSongsCacheKey = QStringLiteral("saved-songs");
const QString kCatchphrase = QStringLiteral("i mean i guess bro");
const QString kReconnectToEdit = QStringLiteral("Reconnect your library once to create and edit playlists.");
constexpr int kSearchHistoryLimit = 20;

/// a playlist made on this computer (LocalLibrary)
bool isLocalId(const QString &id)
{
    return id.startsWith(QLatin1StringView("local:"));
}

/// `next`, with each occurrence `previous` already had kept as it was, in
/// order: the rows stay where they are and the song playing stays marked
QList<Song> keepingInstances(const QList<Song> &previous, QList<Song> next)
{
    QHash<QString, QList<quint64>> held;
    for (const Song &song : previous) {
        held[song.uri].append(song.instanceId);
    }
    for (Song &song : next) {
        const auto found = held.find(song.uri);
        if (found != held.end() && !found->isEmpty()) {
            song.instanceId = found->takeFirst();
        }
    }
    return next;
}

bool sameSongs(const QList<Song> &left, const QList<Song> &right)
{
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](const Song &a, const Song &b) {
        return a.uri == b.uri && a.name == b.name && a.artists == b.artists && a.albumName == b.albumName
            && a.artworkUrl == b.artworkUrl && a.durationMs == b.durationMs;
    });
}

/// the items page PlaybackCore::playlistJson completes with
QList<Song> songsFromItems(const QString &payload)
{
    QList<Song> songs;
    const QJsonObject page = QJsonDocument::fromJson(payload.toUtf8()).object();
    for (const QJsonValue &value : page.value(QStringLiteral("items")).toArray()) {
        if (const auto song = json::wrappedTrack(value.toObject())) {
            songs.append(*song);
        }
    }
    return songs;
}

/// the top tracks response PlaybackCore::artistTopTracksJson completes with
QList<Song> songsFromTopTracks(const QString &payload)
{
    QList<Song> songs;
    const QJsonObject response = QJsonDocument::fromJson(payload.toUtf8()).object();
    for (const QJsonValue &value : response.value(QStringLiteral("tracks")).toArray()) {
        if (const auto song = json::track(value.toObject())) {
            songs.append(*song);
        }
    }
    return songs;
}

} // namespace

bool LibraryStore::Destination::operator==(const Destination &other) const
{
    if (page != other.page) {
        return false;
    }
    switch (page) {
    case Page::Playlist: return playlistId == other.playlistId;
    case Page::Album: return album.id == other.album.id;
    case Page::Artist: return artist.id == other.artist.id;
    default: return true;
    }
}

LibraryStore::LibraryStore(SpotifySession *session, Settings *settings, ErrorCenter *errors, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_settings(settings)
    , m_errors(errors)
    , m_playlists(new PlaylistListModel(this))
    , m_pinnedPlaylists(new PlaylistListModel(this))
    , m_playlistSongs(new SongListModel(this))
    , m_displayedPlaylistSongs(new SongListModel(this))
    , m_savedSongs(new SongListModel(this))
    , m_recentlyPlayed(new SongListModel(this))
    , m_recommendedArtists(new ArtistListModel(this))
    , m_recommendedAlbums(new AlbumListModel(this))
    , m_albumSongs(new SongListModel(this))
    , m_artistTopSongs(new SongListModel(this))
    , m_artistAlbums(new AlbumListModel(this))
    , m_searchSongs(new SongListModel(this))
    , m_searchAlbums(new AlbumListModel(this))
    , m_searchArtists(new ArtistListModel(this))
    , m_searchPlaylists(new PlaylistListModel(this))
    , m_searchHistory(new SongListModel(this))
    , m_searchHistoryPreview(new SongListModel(this))
{
    if (!s_instance) {
        s_instance = this;
    }

    const QStringList pinned = m_settings->value(kPinnedKey).toStringList();
    m_pinnedIds = QSet<QString>(pinned.begin(), pinned.end());

    QList<Song> history;
    for (const QJsonValue &value : QJsonDocument::fromJson(m_settings->value(kSearchHistoryKey).toByteArray()).array()) {
        const Song song = Song::fromJson(value.toObject());
        if (song.isValid()) {
            history.append(song);
        }
    }
    setSearchHistory(history);

    connect(m_savedSongs, &SongListModel::countChanged, this, &LibraryStore::savedSongTotalChanged);
    m_session->setInitialLibraryLoader([this] { return loadInitialLibrary(); });
    connect(m_session, &SpotifySession::loggedOut, this, &LibraryStore::reset);
}

LibraryStore::~LibraryStore()
{
    m_songLoad.cancel();
    m_artistLoad.cancel();
    m_search.cancel();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

LibraryStore *LibraryStore::instance()
{
    return s_instance;
}

LibraryStore *LibraryStore::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void LibraryStore::attachPlayer(PlaybackController *player)
{
    connect(player, &PlaybackController::playedFromSearch, this, &LibraryStore::recordSearchPlayback);
}

void LibraryStore::attachLocalFiles(LocalLibrary *localFiles)
{
    m_localFiles = localFiles;
    connect(localFiles, &LocalLibrary::playlistsChanged, this, [this] {
        refreshPinnedPlaylists();
        refreshLocalPlaylist();
    });
    refreshPinnedPlaylists();
}

void LibraryStore::refreshLocalPlaylist()
{
    const QString id = selectedPlaylistId();
    if (!isLocalId(id) || !m_localFiles) {
        return;
    }
    // its name, cover or count may be what changed
    emit selectedPlaylistChanged();
    const QList<Song> next = m_localFiles->songsOf(id);
    if (!sameSongs(next, m_songs)) {
        setSongs(keepingInstances(m_songs, next));
    }
}

void LibraryStore::reportError(const QString &context, const QString &message)
{
    qCWarning(lcLibrary).noquote() << context << ':' << message;
    m_errors->show(message);
}

void LibraryStore::setLoading(bool &flag, bool value)
{
    if (flag != value) {
        flag = value;
        emit loadingChanged();
    }
}

// --- state -----------------------------------------------------------------------------

Playlist LibraryStore::selectedPlaylist() const
{
    if (m_destination.page != Page::Playlist) {
        return {};
    }
    if (isLocalId(m_destination.playlistId)) {
        const Playlist local = m_localFiles ? m_localFiles->playlist(m_destination.playlistId) : Playlist {};
        return local.isValid() ? local : m_destination.playlist;
    }
    for (const Playlist &playlist : m_playlistList) {
        if (playlist.id == m_destination.playlistId) {
            return playlist;
        }
    }
    // opened from search without being in the library
    return m_destination.playlist;
}

bool LibraryStore::isSelectedPlaylistEditable() const
{
    const Playlist playlist = selectedPlaylist();
    return playlist.isValid() && (playlist.isLocal() || playlist.canModify(m_profile.id));
}

QList<Playlist> LibraryStore::editablePlaylistList() const
{
    QList<Playlist> result;
    for (const Playlist &playlist : m_playlistList) {
        if (playlist.canModify(m_profile.id)) {
            result.append(playlist);
        }
    }
    return result;
}

QVariantList LibraryStore::editablePlaylists() const
{
    QVariantList result;
    for (const Playlist &playlist : editablePlaylistList()) {
        result.append(QVariant::fromValue(playlist));
    }
    return result;
}

void LibraryStore::setProfile(const Profile &profile)
{
    m_profile = profile;
    emit profileChanged();
    emit selectedPlaylistChanged();
}

void LibraryStore::setPlaylistList(const QList<Playlist> &playlists)
{
    m_playlistList = playlists;
    m_playlists->setItems(playlists);
    refreshPinnedPlaylists();
    emit selectedPlaylistChanged();
}

void LibraryStore::refreshPinnedPlaylists()
{
    QList<Playlist> pinned;
    for (const Playlist &playlist : m_playlistList) {
        if (m_pinnedIds.contains(playlist.id)) {
            pinned.append(playlist);
        }
    }
    if (m_localFiles) {
        for (const Playlist &playlist : m_localFiles->playlists()->items()) {
            if (m_pinnedIds.contains(playlist.id)) {
                pinned.append(playlist);
            }
        }
    }
    m_pinnedPlaylists->setItems(pinned);
}

void LibraryStore::persistPins()
{
    QStringList ids(m_pinnedIds.begin(), m_pinnedIds.end());
    ids.sort();
    m_settings->setValue(kPinnedKey, ids);
}

void LibraryStore::setSongs(const QList<Song> &songs)
{
    m_songs = songs;
    // written through, so a reorder or a removal is what the page shows when it
    // is opened again, rather than the order the server last sent.
    if (m_destination.page == Page::Playlist) {
        m_openedPlaylists.insert(m_destination.playlistId, songs);
    }
    m_playlistSongs->setSongs(songs);
    refreshDisplayedSongs();
}

void LibraryStore::refreshDisplayedSongs()
{
    const QString query = m_playlistFilterQuery.trimmed();
    QList<Song> shown;
    if (query.isEmpty()) {
        shown = m_songs;
    } else {
        for (const Song &song : m_songs) {
            if (song.name.contains(query, Qt::CaseInsensitive) || song.artists.contains(query, Qt::CaseInsensitive)
                || song.albumName.contains(query, Qt::CaseInsensitive)) {
                shown.append(song);
            }
        }
    }

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const auto byText = [&collator](auto field) {
        return [&collator, field](const Song &left, const Song &right) {
            return collator.compare(field(left), field(right)) < 0;
        };
    };
    const auto byTextDesc = [&collator](auto field) {
        return [&collator, field](const Song &left, const Song &right) {
            return collator.compare(field(left), field(right)) > 0;
        };
    };
    switch (m_sortOrder) {
    case SortOrder::Playlist:
        if (m_sortReversed) {
            std::reverse(shown.begin(), shown.end());
        }
        break;
    case SortOrder::Title:
        if (m_sortReversed) {
            std::stable_sort(shown.begin(), shown.end(), byTextDesc([](const Song &song) { return song.name; }));
        } else {
            std::stable_sort(shown.begin(), shown.end(), byText([](const Song &song) { return song.name; }));
        }
        break;
    case SortOrder::Artist:
        if (m_sortReversed) {
            std::stable_sort(shown.begin(), shown.end(), byTextDesc([](const Song &song) { return song.artists; }));
        } else {
            std::stable_sort(shown.begin(), shown.end(), byText([](const Song &song) { return song.artists; }));
        }
        break;
    case SortOrder::Album:
        if (m_sortReversed) {
            std::stable_sort(shown.begin(), shown.end(), byTextDesc([](const Song &song) { return song.albumName; }));
        } else {
            std::stable_sort(shown.begin(), shown.end(), byText([](const Song &song) { return song.albumName; }));
        }
        break;
    case SortOrder::Duration:
        if (m_sortReversed) {
            std::stable_sort(shown.begin(), shown.end(), [](const Song &left, const Song &right) {
                const int a = left.durationMs > 0 ? left.durationMs : -1;
                const int b = right.durationMs > 0 ? right.durationMs : -1;
                return a > b;
            });
        } else {
            std::stable_sort(shown.begin(), shown.end(), [](const Song &left, const Song &right) {
                const int a = left.durationMs > 0 ? left.durationMs : std::numeric_limits<int>::max();
                const int b = right.durationMs > 0 ? right.durationMs : std::numeric_limits<int>::max();
                return a < b;
            });
        }
        break;
    }
    m_displayedPlaylistSongs->setSongs(shown);
}

void LibraryStore::setPlaylistFilterQuery(const QString &query)
{
    if (m_playlistFilterQuery == query) {
        return;
    }
    m_playlistFilterQuery = query;
    refreshDisplayedSongs();
    emit playlistViewChanged();
}

void LibraryStore::setPlaylistSortOrder(SortOrder order)
{
    if (m_sortOrder == order) {
        return;
    }
    m_sortOrder = order;
    m_sortReversed = false;
    refreshDisplayedSongs();
    emit playlistViewChanged();
}

void LibraryStore::setPlaylistSortReversed(bool reversed)
{
    if (m_sortReversed == reversed) {
        return;
    }
    m_sortReversed = reversed;
    refreshDisplayedSongs();
    emit playlistViewChanged();
}

void LibraryStore::togglePlaylistSortOrder(SortOrder order)
{
    if (m_sortOrder == order) {
        m_sortReversed = !m_sortReversed;
    } else {
        m_sortOrder = order;
        m_sortReversed = false;
    }
    refreshDisplayedSongs();
    emit playlistViewChanged();
}

void LibraryStore::togglePlaylistSortOrder()
{
    m_sortReversed = !m_sortReversed;
    refreshDisplayedSongs();
    emit playlistViewChanged();
}

bool LibraryStore::canReorderPlaylist() const
{
    // dragging is offered only where it means something: on a playlist this
    // account may edit, showing every song, in the playlist's own order.
    return isSelectedPlaylistEditable() && m_sortOrder == SortOrder::Playlist && !m_sortReversed && m_playlistFilterQuery.trimmed().isEmpty();
}

void LibraryStore::refreshRecommendations()
{
    QList<Artist> artists;
    QList<Album> albums;
    QSet<QString> seenArtists;
    QSet<QString> seenAlbums;
    for (const Song &song : m_recentlyPlayed->songs()) {
        Artist artist = song.primaryArtist();
        if (artist.isValid() && !seenArtists.contains(artist.id)) {
            seenArtists.insert(artist.id);
            if (artist.artworkUrl.isEmpty()) {
                artist.artworkUrl = song.artworkUrl;
            }
            artists.append(artist);
        }
        const Album album = song.albumDestination();
        if (album.isValid() && !seenAlbums.contains(album.id)) {
            seenAlbums.insert(album.id);
            albums.append(album);
        }
    }
    m_recommendedArtists->setItems(artists);
    m_recommendedAlbums->setItems(albums);
}

// --- navigation ---------------------------------------------------------------------

void LibraryStore::navigate(const Destination &destination)
{
    const Destination previous = m_destination;
    m_destination = destination;

    // going back must not record the screen it came from, or back would
    // bounce between the last two destinations forever.
    bool record = true;
    if (m_restoreTarget) {
        const Destination target = *m_restoreTarget;
        m_restoreTarget.reset();
        if (target == destination) {
            record = false;
        }
    }
    if (record && !(previous == destination) && (m_history.isEmpty() || !(m_history.last() == previous))) {
        m_history.append(previous);
    }
    emit destinationChanged();
    emit selectedPlaylistChanged();
    emit playlistViewChanged();
}

void LibraryStore::goBack()
{
    if (m_history.isEmpty()) {
        return;
    }
    const Destination destination = m_history.takeLast();
    m_restoreTarget = destination;
    switch (destination.page) {
    case Page::Home: showHome(); break;
    case Page::Search: showSearch(); break;
    case Page::Songs: showSavedSongs(); break;
    case Page::Playlists: showPlaylists(); break;
    case Page::PinnedPlaylists: showPinnedPlaylists(); break;
    case Page::LocalFiles: showLocalFiles(); break;
    case Page::LocalTracks: showLocalTracks(); break;
    case Page::Playlist: {
        if (isLocalId(destination.playlistId)) {
            const Playlist local = m_localFiles ? m_localFiles->playlist(destination.playlistId) : Playlist {};
            if (local.isValid()) {
                selectPlaylist(local);
            } else {
                showLocalFiles();
            }
            break;
        }
        const auto found = std::find_if(m_playlistList.begin(), m_playlistList.end(),
                                        [&](const Playlist &playlist) { return playlist.id == destination.playlistId; });
        if (found != m_playlistList.end()) {
            selectPlaylist(*found);
        } else {
            showPlaylists();
        }
        break;
    }
    case Page::Album: selectAlbum(destination.album); break;
    case Page::Artist: selectArtist(destination.artist); break;
    }
}

void LibraryStore::cancelDetailLoads()
{
    m_songLoad.cancel();
    m_artistLoad.cancel();
    setLoading(m_loadingSongs, false);
    setLoading(m_loadingArtist, false);
}

void LibraryStore::showHome()
{
    cancelDetailLoads();
    navigate({Page::Home});
    if (!m_hasLoadedHome) {
        detach(loadHome());
    }
}

void LibraryStore::showSearch()
{
    cancelDetailLoads();
    navigate({Page::Search});
}

void LibraryStore::showSavedSongs()
{
    cancelDetailLoads();
    navigate({Page::Songs});
    if (!m_hasLoadedSavedSongs) {
        // what was loaded last time shows while the list is fetched again
        if (m_savedSongs->count() == 0) {
            if (const std::optional<QList<Song>> stored = m_songListCache.load(kSavedSongsCacheKey)) {
                m_savedSongs->setSongs(*stored);
            }
        }
        detach(loadSavedSongs());
    }
}

void LibraryStore::showPlaylists()
{
    cancelDetailLoads();
    navigate({Page::Playlists});
}

void LibraryStore::showPinnedPlaylists()
{
    cancelDetailLoads();
    navigate({Page::PinnedPlaylists});
}

void LibraryStore::showLocalFiles()
{
    cancelDetailLoads();
    navigate({Page::LocalFiles});
}

void LibraryStore::showLocalTracks()
{
    cancelDetailLoads();
    navigate({Page::LocalTracks});
}

void LibraryStore::selectPlaylistById(const QString &id)
{
    if (isLocalId(id)) {
        const Playlist local = m_localFiles ? m_localFiles->playlist(id) : Playlist {};
        if (local.isValid()) {
            selectPlaylist(local);
        }
        return;
    }
    for (const Playlist &playlist : m_playlistList) {
        if (playlist.id == id) {
            selectPlaylist(playlist);
            return;
        }
    }
}

void LibraryStore::selectPlaylist(const Playlist &playlist)
{
    cancelDetailLoads();
    m_playlistFilterQuery.clear();
    m_sortOrder = SortOrder::Playlist;
    m_sortReversed = false;
    m_errors->clear();
    // shown at once when it is already known, and refreshed underneath: a
    // playlist can be edited from anywhere.
    const QList<Song> *opened = m_openedPlaylists.find(playlist.id);
    // not opened since the app started: what was loaded last time, if any
    if (!opened && !playlist.isLocal()) {
        if (const std::optional<QList<Song>> stored = m_songListCache.load(playlist.id)) {
            m_openedPlaylists.insert(playlist.id, *stored);
            opened = m_openedPlaylists.find(playlist.id);
        }
    }
    const QList<Song> openedSongs = opened ? *opened : QList<Song> {};
    navigate({Page::Playlist, playlist.id, {}, {}, playlist});
    if (playlist.isLocal()) {
        // nothing to load: the files are already known
        setSongs(keepingInstances(openedSongs, m_localFiles ? m_localFiles->songsOf(playlist.id) : QList<Song> {}));
        return;
    }
    setSongs(openedSongs);
    if (!m_session->hasLibraryToken()) {
        return;
    }
    setLoading(m_loadingSongs, opened == nullptr);
    detach(runPlaylistLoad(playlist, m_songLoad.renew()));
}

QCoro::Task<> LibraryStore::runPlaylistLoad(Playlist playlist, CancelToken token)
{
    const Destination target {Page::Playlist, playlist.id};
    try {
        QList<Song> loaded;
        if (playlist.canModify(m_profile.id)) {
            loaded = co_await m_session->withLibraryAccessToken<QList<Song>>(
                [this, id = playlist.id](QString accessToken) { return m_session->webApi().playlistSongs(id, accessToken); });
        } else {
            // the web api only gives the name and cover of a playlist someone
            // else made, the session still reads its songs
            co_await m_session->ensureFreshPlaybackConnection();
            throwIfCancelled(token);
            loaded = songsFromItems(co_await m_session->core()->playlistJson(playlist.uri));
        }
        throwIfCancelled(token);
        m_songListCache.save(playlist.id, loaded);
        if (!(m_destination == target)) {
            co_return;
        }
        setSongs(loaded);
        setLoading(m_loadingSongs, false);
        m_songLoad.finish(token);
        co_await loadSavedStatuses(loaded);
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        if (m_destination == target && !isCancelled(token)) {
            setLoading(m_loadingSongs, false);
            reportError(QStringLiteral("Loading songs from \"%1\"").arg(playlist.name), error.message());
        }
    }
    m_songLoad.finish(token);
}

void LibraryStore::selectAlbum(const Album &album)
{
    cancelDetailLoads();
    const QList<Song> *opened = m_openedAlbums.find(album.id);
    navigate({Page::Album, {}, album});
    m_albumSongs->setSongs(opened ? *opened : QList<Song> {});
    if (!m_session->hasLibraryToken()) {
        return;
    }
    setLoading(m_loadingSongs, opened == nullptr);
    detach(runAlbumLoad(album, m_songLoad.renew()));
}

QCoro::Task<> LibraryStore::runAlbumLoad(Album album, CancelToken token)
{
    const Destination target {Page::Album, {}, album};
    try {
        const QList<Song> loaded = co_await m_session->withLibraryAccessToken<QList<Song>>(
            [this, album](QString accessToken) { return m_session->webApi().albumSongs(album, accessToken); });
        throwIfCancelled(token);
        if (!(m_destination == target)) {
            co_return;
        }
        m_albumSongs->setSongs(loaded);
        m_openedAlbums.insert(album.id, loaded);
        setLoading(m_loadingSongs, false);
        m_songLoad.finish(token);
        co_await loadSavedStatuses(loaded);
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        if (m_destination == target && !isCancelled(token)) {
            setLoading(m_loadingSongs, false);
            reportError(QStringLiteral("Loading \"%1\"").arg(album.name), error.message());
        }
    }
    m_songLoad.finish(token);
}

void LibraryStore::selectArtist(const Artist &artist)
{
    cancelDetailLoads();
    const ArtistDetail *opened = m_openedArtists.find(artist.id);
    navigate({Page::Artist, {}, {}, artist});
    m_artistAlbums->setItems(opened ? opened->albums : QList<Album> {});
    m_artistTopSongs->setSongs(opened ? opened->topSongs : QList<Song> {});
    m_errors->clear();
    if (!m_session->hasLibraryToken()) {
        return;
    }
    setLoading(m_loadingArtist, opened == nullptr);
    detach(runArtistLoad(artist, m_artistLoad.renew()));
}

QCoro::Task<> LibraryStore::runArtistLoad(Artist artist, CancelToken token)
{
    const Destination target {Page::Artist, {}, {}, artist};
    try {
        // top tracks are the reason you open an artist. albums still load if
        // they fail. the web api no longer has them, the session does
        QList<Song> topSongs;
        bool topSongsLoaded = false;
        try {
            co_await m_session->ensureFreshPlaybackConnection();
            throwIfCancelled(token);
            topSongs = songsFromTopTracks(
                co_await m_session->core()->artistTopTracksJson(QStringLiteral("spotify:artist:%1").arg(artist.id)));
            topSongsLoaded = true;
        } catch (const SpotifyError &error) {
            qCWarning(lcLibrary).noquote() << "loading top songs for" << artist.name << ':' << error.message();
        }
        throwIfCancelled(token);
        if (!(m_destination == target)) {
            co_return;
        }
        if (topSongsLoaded) {
            m_artistTopSongs->setSongs(topSongs);
            m_openedArtists.insert(artist.id, {topSongs, m_artistAlbums->items()});
            co_await loadSavedStatuses(topSongs);
            throwIfCancelled(token);
        }

        const QList<Album> albums = co_await m_session->withLibraryAccessToken<QList<Album>>(
            [this, artist](QString accessToken) { return m_session->webApi().artistAlbums(artist, accessToken); });
        throwIfCancelled(token);
        if (!(m_destination == target)) {
            co_return;
        }
        m_artistAlbums->setItems(albums);
        m_openedArtists.insert(artist.id, {m_artistTopSongs->songs(), albums});
        setLoading(m_loadingArtist, false);
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        if (m_destination == target && !isCancelled(token)) {
            setLoading(m_loadingArtist, false);
            reportError(QStringLiteral("Loading %1").arg(artist.name), error.message());
        }
    }
    m_artistLoad.finish(token);
}

void LibraryStore::goToAlbum(const Song &song)
{
    detach(runSongNavigation(song, true));
}

void LibraryStore::goToArtist(const Song &song)
{
    detach(runSongNavigation(song, false));
}

QCoro::Task<> LibraryStore::runSongNavigation(Song song, bool toAlbum)
{
    try {
        if (toAlbum) {
            Album album = song.albumDestination();
            if (!album.isValid()) {
                const Song resolved = co_await m_session->withLibraryAccessToken<Song>(
                    [this, id = song.spotifyId()](QString accessToken) { return m_session->webApi().song(id, accessToken); });
                album = resolved.albumDestination();
                if (!album.isValid()) {
                    throw SpotifyError::invalidData(QStringLiteral("Spotify did not return an album for this track."));
                }
            }
            selectAlbum(album);
        } else {
            Artist artist = song.primaryArtist();
            if (!artist.isValid()) {
                const Song resolved = co_await m_session->withLibraryAccessToken<Song>(
                    [this, id = song.spotifyId()](QString accessToken) { return m_session->webApi().song(id, accessToken); });
                artist = resolved.primaryArtist();
                if (!artist.isValid()) {
                    throw SpotifyError::invalidData(QStringLiteral("Spotify did not return an artist for this track."));
                }
            }
            selectArtist(artist);
        }
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        reportError(QStringLiteral("Opening %1 for %2").arg(toAlbum ? QStringLiteral("album") : QStringLiteral("artist"), song.uri),
                    error.message());
    }
}

// --- search ------------------------------------------------------------------------

bool LibraryStore::searchIsCatchphrase() const
{
    return m_searchQuery.trimmed().toLower() == kCatchphrase;
}

bool LibraryStore::hasSearchResults() const
{
    return m_searchSongs->count() > 0 || m_searchAlbums->count() > 0 || m_searchArtists->count() > 0
        || m_searchPlaylists->count() > 0;
}

void LibraryStore::setSearchQuery(const QString &query)
{
    if (m_searchQuery == query) {
        return;
    }
    m_searchQuery = query;
    m_search.cancel();

    const QString trimmed = query.trimmed();
    const auto clearResults = [this] {
        m_searchSongs->clear();
        m_searchAlbums->setItems({});
        m_searchArtists->setItems({});
        m_searchPlaylists->setItems({});
        setLoading(m_searching, false);
    };
    if (trimmed.isEmpty() || searchIsCatchphrase()) {
        clearResults();
    } else if (!m_session->hasLibraryToken()) {
        m_errors->show(QStringLiteral("Connect your Spotify library to search the catalog."));
    } else {
        setLoading(m_searching, true);
        detach(runSearch(trimmed, m_search.renew()));
    }
    emit searchChanged();
}

QCoro::Task<> LibraryStore::runSearch(QString query, CancelToken token)
{
    try {
        co_await QCoro::sleepFor(std::chrono::milliseconds(350));
        throwIfCancelled(token);
        const SearchResults results = co_await m_session->withLibraryAccessToken<SearchResults>(
            [this, query](QString accessToken) { return m_session->webApi().search(query, accessToken); });
        throwIfCancelled(token);
        if (m_searchQuery.trimmed() != query) {
            co_return;
        }
        m_searchSongs->setSongs(results.songs);
        m_searchAlbums->setItems(results.albums);
        m_searchArtists->setItems(results.artists);
        m_searchPlaylists->setItems(results.playlists);
        setLoading(m_searching, false);
        emit searchChanged();
        m_search.finish(token);
        co_await loadSavedStatuses(results.songs);
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        if (!isCancelled(token)) {
            setLoading(m_searching, false);
            reportError(QStringLiteral("Searching for \"%1\"").arg(query), error.message());
        }
    }
    m_search.finish(token);
}

void LibraryStore::recordSearchPlayback(const Song &song)
{
    QList<Song> history = m_searchHistory->songs();
    history.removeIf([&](const Song &item) { return item.uri == song.uri; });
    history.prepend(song);
    if (history.size() > kSearchHistoryLimit) {
        history.resize(kSearchHistoryLimit);
    }
    setSearchHistory(history);
    persistSearchHistory();
}

void LibraryStore::clearSearchHistory()
{
    setSearchHistory({});
    persistSearchHistory();
}

void LibraryStore::setSearchHistory(const QList<Song> &songs)
{
    m_searchHistory->setSongs(songs);
    m_searchHistoryPreview->setSongs(songs.mid(0, 8));
}

void LibraryStore::persistSearchHistory()
{
    QJsonArray array;
    for (const Song &song : m_searchHistory->songs()) {
        array.append(song.toJson());
    }
    m_settings->setValue(kSearchHistoryKey, QJsonDocument(array).toJson(QJsonDocument::Compact));
}

// --- saved songs and pins ------------------------------------------------------------

void LibraryStore::togglePinned(const Playlist &playlist)
{
    if (m_pinnedIds.contains(playlist.id)) {
        m_pinnedIds.remove(playlist.id);
    } else {
        m_pinnedIds.insert(playlist.id);
    }
    persistPins();
    refreshPinnedPlaylists();
    ++m_pinsRevision;
    emit pinsChanged();
}

void LibraryStore::toggleSaved(const Song &song)
{
    if (!m_session->hasFullLibraryAccess()) {
        m_errors->show(QStringLiteral("Reconnect your library once to enable saved songs and playlist editing."));
        return;
    }
    if (!m_session->hasLibraryToken()) {
        m_errors->show(QStringLiteral("Connect your Spotify library to save songs."));
        return;
    }
    // never send contradictory save and remove requests for the same song.
    if (m_pendingSavedUris.contains(song.uri)) {
        return;
    }
    m_pendingSavedUris.insert(song.uri);
    ++m_savedStatusRevision;

    const bool shouldSave = !m_savedUris.contains(song.uri);
    if (shouldSave) {
        m_savedUris.insert(song.uri);
        const QList<Song> &saved = m_savedSongs->songs();
        if (std::none_of(saved.begin(), saved.end(), [&](const Song &item) { return item.uri == song.uri; })) {
            QList<Song> updated = saved;
            updated.prepend(song);
            m_savedSongs->setSongs(updated);
        }
    } else {
        m_savedUris.remove(song.uri);
        m_savedSongs->removeAll([&](const Song &item) { return item.uri == song.uri; });
    }
    adjustSavedSongTotal(shouldSave ? 1 : -1);
    ++m_savedRevision;
    emit savedChanged();
    detach(runToggleSaved(song, shouldSave, m_session->generation()));
}

QCoro::Task<> LibraryStore::runToggleSaved(Song song, bool shouldSave, quint64 generation)
{
    try {
        co_await m_session->withLibraryAccessToken<void>([this, shouldSave, uri = song.uri](QString accessToken) {
            return m_session->webApi().setSaved(shouldSave, uri, accessToken);
        });
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        if (shouldSave) {
            m_savedUris.remove(song.uri);
            m_savedSongs->removeAll([&](const Song &item) { return item.uri == song.uri; });
        } else {
            m_savedUris.insert(song.uri);
            QList<Song> updated = m_savedSongs->songs();
            updated.prepend(song);
            m_savedSongs->setSongs(updated);
        }
        adjustSavedSongTotal(shouldSave ? -1 : 1);
        ++m_savedRevision;
        emit savedChanged();
        reportError(QStringLiteral("Updating saved songs"), error.message());
    }
    if (generation == m_session->generation()) {
        m_pendingSavedUris.remove(song.uri);
    }
}

QCoro::Task<> LibraryStore::loadSavedStatuses(QList<Song> songs)
{
    if (m_session->libraryNeedsAuthorizationUpgrade() || !m_session->hasLibraryToken() || songs.isEmpty()) {
        co_return;
    }
    const int revision = m_savedStatusRevision;
    const QSet<QString> pendingAtRequest = m_pendingSavedUris;
    QStringList uris;
    for (const Song &song : songs) {
        uris.append(song.uri);
    }
    try {
        const QHash<QString, bool> statuses = co_await m_session->withLibraryAccessToken<QHash<QString, bool>>(
            [this, uris](QString accessToken) { return m_session->webApi().savedStatuses(uris, accessToken); });
        if (revision != m_savedStatusRevision) {
            co_return;
        }
        bool changed = false;
        for (auto status = statuses.cbegin(); status != statuses.cend(); ++status) {
            if (m_pendingSavedUris.contains(status.key()) || pendingAtRequest.contains(status.key())) {
                continue;
            }
            if (status.value() && !m_savedUris.contains(status.key())) {
                m_savedUris.insert(status.key());
                changed = true;
            } else if (!status.value() && m_savedUris.remove(status.key())) {
                changed = true;
            }
        }
        if (changed) {
            ++m_savedRevision;
            emit savedChanged();
        }
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        qCInfo(lcLibrary).noquote() << "checking saved songs:" << error.message();
    }
}

// --- loading ---------------------------------------------------------------------------

QCoro::Task<> LibraryStore::loadInitialLibrary()
{
    if (!m_session->hasLibraryToken()) {
        co_return;
    }
    if (!m_session->libraryNeedsAuthorizationUpgrade()) {
        try {
            const Profile profile = co_await m_session->withLibraryAccessToken<Profile>(
                [this](QString accessToken) { return m_session->webApi().profile(accessToken); });
            setProfile(profile);
        } catch (const Cancelled &) {
            co_return;
        } catch (const SpotifyError &error) {
            qCWarning(lcLibrary).noquote() << "loading spotify profile:" << error.message();
        }
    }
    co_await loadPlaylists();
    if (!m_session->libraryNeedsAuthorizationUpgrade()) {
        co_await loadHome();
    }
}

QCoro::Task<> LibraryStore::loadPlaylists()
{
    if (!m_session->hasLibraryToken() || m_loadingPlaylists) {
        co_return;
    }
    setLoading(m_loadingPlaylists, true);
    const quint64 generation = m_session->generation();
    try {
        const QList<Playlist> playlists = co_await m_session->withLibraryAccessToken<QList<Playlist>>(
            [this](QString accessToken) { return m_session->webApi().playlists(accessToken); });
        setPlaylistList(playlists);
        m_session->setLibraryStatus(QStringLiteral("Connected"));
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        reportError(QStringLiteral("Loading playlists"), error.message());
    }
    if (generation == m_session->generation()) {
        setLoading(m_loadingPlaylists, false);
    }
}

void LibraryStore::retryLoadingPlaylists()
{
    detach(loadPlaylists());
}

QCoro::Task<> LibraryStore::loadHome()
{
    if (m_session->libraryNeedsAuthorizationUpgrade() || !m_session->hasLibraryToken() || m_loadingHome) {
        co_return;
    }
    setLoading(m_loadingHome, true);
    const quint64 generation = m_session->generation();
    // the favourite songs' card counts them, long before their list is opened
    detach(loadSavedSongTotal());
    try {
        const QList<Song> recent = co_await m_session->withLibraryAccessToken<QList<Song>>(
            [this](QString accessToken) { return m_session->webApi().recentlyPlayed(accessToken); });
        m_recentlyPlayed->setSongs(recent);
        refreshRecommendations();
        m_hasLoadedHome = true;
        co_await loadSavedStatuses(recent);
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        qCWarning(lcLibrary).noquote() << "loading recently played songs:" << error.message();
    }
    if (generation == m_session->generation()) {
        setLoading(m_loadingHome, false);
    }
}

QCoro::Task<> LibraryStore::loadSavedSongs()
{
    if (m_session->libraryNeedsAuthorizationUpgrade()) {
        m_errors->show(QStringLiteral("Reconnect your library once to enable saved songs and playlist editing."));
        co_return;
    }
    if (!m_session->hasLibraryToken() || m_loadingSavedSongs) {
        co_return;
    }
    setLoading(m_loadingSavedSongs, true);
    const quint64 generation = m_session->generation();
    const int revision = m_savedStatusRevision;
    try {
        const QList<Song> loaded = co_await m_session->withLibraryAccessToken<QList<Song>>(
            [this](QString accessToken) { return m_session->webApi().savedSongs(accessToken); });
        if (revision == m_savedStatusRevision) {
            QList<Song> merged;
            for (const Song &song : m_savedSongs->songs()) {
                if (m_pendingSavedUris.contains(song.uri)) {
                    merged.append(song);
                }
            }
            for (const Song &song : loaded) {
                if (!m_pendingSavedUris.contains(song.uri)) {
                    merged.append(song);
                }
            }
            m_savedSongs->setSongs(merged);
            m_songListCache.save(kSavedSongsCacheKey, loaded);
            for (const Song &song : merged) {
                m_savedUris.insert(song.uri);
            }
            m_hasLoadedSavedSongs = true;
            ++m_savedRevision;
            emit savedChanged();
            emit savedSongTotalChanged();
        }
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        reportError(QStringLiteral("Loading saved songs"), error.message());
    }
    if (generation == m_session->generation()) {
        setLoading(m_loadingSavedSongs, false);
    }
}

QCoro::Task<> LibraryStore::loadSavedSongTotal()
{
    if (m_hasLoadedSavedSongs) {
        co_return;
    }
    const int revision = m_savedStatusRevision;
    try {
        const int total = co_await m_session->withLibraryAccessToken<int>(
            [this](QString accessToken) { return m_session->webApi().savedSongTotal(accessToken); });
        // a song saved or removed meanwhile may or may not be counted in it
        if (revision == m_savedStatusRevision && !m_hasLoadedSavedSongs) {
            m_savedSongTotal = total;
            emit savedSongTotalChanged();
        }
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        qCWarning(lcLibrary).noquote() << "counting saved songs:" << error.message();
    }
}

void LibraryStore::adjustSavedSongTotal(int delta)
{
    if (!m_hasLoadedSavedSongs && m_savedSongTotal >= 0) {
        m_savedSongTotal = std::max(0, m_savedSongTotal + delta);
        emit savedSongTotalChanged();
    }
}

// --- editing -------------------------------------------------------------------------------

void LibraryStore::adjustPlaylistSongCount(const QString &id, int delta)
{
    QList<Playlist> playlists = m_playlistList;
    for (Playlist &playlist : playlists) {
        if (playlist.id == id) {
            playlist.songCount = std::max(playlist.songCount + delta, 0);
            setPlaylistList(playlists);
            return;
        }
    }
}

void LibraryStore::addToPlaylist(const Song &song, const Playlist &playlist)
{
    if (playlist.isLocal()) {
        // what changes shows once the library says so (refreshLocalPlaylist)
        if (!song.isLocalFile()) {
            m_errors->show(QStringLiteral("Only music files on this computer can go in “%1”.").arg(playlist.name));
        } else if (m_localFiles) {
            m_localFiles->addSongs(playlist.id, {song});
        }
        return;
    }
    detach(runAdd(song, playlist));
}

QCoro::Task<> LibraryStore::runAdd(Song song, Playlist playlist)
{
    if (!m_session->hasFullLibraryAccess()) {
        m_errors->show(kReconnectToEdit);
        co_return;
    }
    if (!m_session->hasLibraryToken()) {
        m_errors->show(QStringLiteral("Connect your Spotify library to edit playlists."));
        co_return;
    }
    try {
        co_await m_session->withLibraryAccessToken<void>([this, uri = song.uri, id = playlist.id](QString accessToken) {
            return m_session->webApi().addToPlaylist(uri, id, accessToken);
        });
        if (selectedPlaylistId() == playlist.id) {
            QList<Song> songs = m_songs;
            songs.append(song.freshInstance());
            setSongs(songs);
        }
        adjustPlaylistSongCount(playlist.id, 1);
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        reportError(QStringLiteral("Adding to \"%1\"").arg(playlist.name), error.message());
    }
}

void LibraryStore::removeFromSelectedPlaylist(const Song &song)
{
    const Playlist playlist = selectedPlaylist();
    if (playlist.isLocal()) {
        removeFromLocalPlaylist(song, playlist);
    } else if (playlist.isValid()) {
        detach(runRemove(song, playlist));
    }
}

void LibraryStore::removeFromPlaylist(const Song &song, const Playlist &playlist)
{
    if (playlist.isLocal()) {
        removeFromLocalPlaylist(song, playlist);
        return;
    }
    detach(runRemove(song, playlist));
}

void LibraryStore::removeFromLocalPlaylist(const Song &song, const Playlist &playlist)
{
    if (m_localFiles) {
        m_localFiles->removeSong(playlist.id, song.uri);
    }
}

QCoro::Task<> LibraryStore::runRemove(Song song, Playlist playlist)
{
    if (!m_session->hasFullLibraryAccess()) {
        m_errors->show(kReconnectToEdit);
        co_return;
    }
    if (!playlist.canModify(m_profile.id) || !m_session->hasLibraryToken()) {
        co_return;
    }
    try {
        co_await m_session->withLibraryAccessToken<void>([this, uri = song.uri, id = playlist.id](QString accessToken) {
            return m_session->webApi().removeFromPlaylist(uri, id, accessToken);
        });
        // navigation can change while the request is in flight. never remove
        // matching songs from a newly selected playlist.
        if (selectedPlaylistId() == playlist.id) {
            QList<Song> songs = m_songs;
            const qsizetype before = songs.size();
            songs.removeIf([&](const Song &item) { return item.uri == song.uri; });
            const int removed = int(before - songs.size());
            setSongs(songs);
            adjustPlaylistSongCount(playlist.id, -removed);
        } else {
            co_await loadPlaylists();
        }
    } catch (const Cancelled &) {
    } catch (const SpotifyError &error) {
        reportError(QStringLiteral("Removing from \"%1\"").arg(playlist.name), error.message());
    }
}

QString LibraryStore::reorderFailureMessage(const QString &message, bool forbidden, const Playlist &playlist) const
{
    // spotify answers a refused reorder with the bare word "forbidden".
    if (!forbidden) {
        return message;
    }
    if (playlist.ownerId != m_profile.id) {
        return QStringLiteral("Only the owner can reorder “%1”. You can still add and remove songs.").arg(playlist.name);
    }
    return QStringLiteral("Spotify would not reorder “%1”. Reconnect your library in Settings and try again.").arg(playlist.name);
}

void LibraryStore::moveSongs(int from, int count, int to)
{
    // a playlist made here is the person's own, and spotify has no say in it
    const Playlist playlist = selectedPlaylist();
    const bool local = playlist.isLocal();
    if (!local && !m_session->hasFullLibraryAccess()) {
        m_errors->show(kReconnectToEdit);
        return;
    }
    // every refusal below used to be silent, which looked exactly like a
    // broken drag: the row sprang back and nothing else happened.
    if (!playlist.isValid()) {
        m_errors->show(QStringLiteral("Open the playlist once before reordering it."));
        return;
    }
    if (!local && !playlist.canModify(m_profile.id)) {
        m_errors->show(QStringLiteral("“%1” belongs to someone else, so its order cannot be changed.").arg(playlist.name));
        return;
    }
    if (!local && !m_session->hasLibraryRefreshToken()) {
        m_errors->show(kReconnectToEdit);
        return;
    }
    if (count <= 0 || from < 0 || from + count > m_songs.size() || to < 0 || to > m_songs.size()) {
        return;
    }

    QList<Song> reordered = m_songs;
    const QList<Song> moving = reordered.mid(from, count);
    reordered.remove(from, count);
    const int insertAt = to > from ? to - count : to;
    for (int offset = 0; offset < count; ++offset) {
        reordered.insert(insertAt + offset, moving.at(offset));
    }
    const auto uris = [](const QList<Song> &songs) {
        QStringList result;
        for (const Song &song : songs) {
            result.append(song.uri);
        }
        return result;
    };
    if (uris(reordered) == uris(m_songs)) {
        return;
    }

    const QList<Song> previous = m_songs;
    // the rows move in place, so the drag lands without the list resetting.
    m_songs = reordered;
    m_openedPlaylists.insert(playlist.id, reordered);
    m_playlistSongs->moveRows(from, count, to);
    m_displayedPlaylistSongs->moveRows(from, count, to);
    if (local) {
        if (m_localFiles) {
            m_localFiles->reorder(playlist.id, uris(reordered));
        }
        return;
    }
    detach(runMove(playlist, from, count, to, previous, reordered));
}

QCoro::Task<> LibraryStore::runMove(Playlist playlist, int from, int count, int to, QList<Song> previous, QList<Song> reordered)
{
    QString failure;
    bool forbidden = false;
    try {
        co_await m_session->withLibraryAccessToken<void>([this, id = playlist.id, from, to, count](QString accessToken) {
            return m_session->webApi().reorderPlaylist(id, from, to, count, accessToken);
        });
        co_return;
    } catch (const Cancelled &) {
        co_return;
    } catch (const SpotifyError &error) {
        failure = error.message();
        forbidden = error.kind() == SpotifyError::Kind::Forbidden;
    }
    reportError(QStringLiteral("Reordering \"%1\" (owner %2, collaborative %3)")
                    .arg(playlist.name, playlist.ownerId.isEmpty() ? QStringLiteral("unknown") : playlist.ownerId)
                    .arg(playlist.isCollaborative),
                reorderFailureMessage(failure, forbidden, playlist));
    // put the order back rather than refetching the playlist: the move did not
    // happen, and a full reload throws away the rest of the page's state.
    const bool stillShown = selectedPlaylistId() == playlist.id && m_songs.size() == reordered.size()
        && std::equal(m_songs.begin(), m_songs.end(), reordered.begin(),
                      [](const Song &a, const Song &b) { return a.uri == b.uri; });
    if (stillShown) {
        setSongs(previous);
    }
}

void LibraryStore::deletePlaylist(const Playlist &playlist)
{
    if (playlist.isLocal()) {
        deleteLocalPlaylist(playlist);
        return;
    }
    detach(runDelete(playlist));
}

void LibraryStore::deleteLocalPlaylist(const Playlist &playlist)
{
    if (!m_localFiles || !m_localFiles->deletePlaylist(playlist.id)) {
        return;
    }
    if (m_pinnedIds.remove(playlist.id)) {
        persistPins();
        refreshPinnedPlaylists();
        ++m_pinsRevision;
        emit pinsChanged();
    }
    m_openedPlaylists.remove(playlist.id);
    m_songListCache.remove(playlist.id);
    if (selectedPlaylistId() == playlist.id) {
        setSongs({});
        m_playlistFilterQuery.clear();
        navigate({Page::LocalFiles});
    }
    emit playlistDeleted(playlist.id);
}

QCoro::Task<> LibraryStore::runDelete(Playlist playlist)
{
    if (!m_session->hasFullLibraryAccess()) {
        m_errors->show(kReconnectToEdit);
        co_return;
    }
    if (!m_session->hasLibraryToken()) {
        m_errors->show(QStringLiteral("Connect your Spotify library to delete playlists."));
        co_return;
    }
    try {
        co_await m_session->withLibraryAccessToken<void>(
            [this, id = playlist.id](QString accessToken) { return m_session->webApi().deletePlaylist(id, accessToken); });
    } catch (const Cancelled &) {
        co_return;
    } catch (const SpotifyError &error) {
        reportError(QStringLiteral("Deleting \"%1\"").arg(playlist.name), error.message());
        co_return;
    }

    QList<Playlist> playlists = m_playlistList;
    playlists.removeIf([&](const Playlist &item) { return item.id == playlist.id; });
    if (m_pinnedIds.remove(playlist.id)) {
        persistPins();
        ++m_pinsRevision;
        emit pinsChanged();
    }
    m_openedPlaylists.remove(playlist.id);
    m_songListCache.remove(playlist.id);
    setPlaylistList(playlists);
    if (selectedPlaylistId() == playlist.id) {
        setSongs({});
        m_playlistFilterQuery.clear();
        navigate({Page::Playlists});
    }
    emit playlistDeleted(playlist.id);
}

void LibraryStore::savePlaylist(bool isNew, const QString &name, const QString &description, bool isPublic,
                                const QByteArray &coverJpeg)
{
    detach(runSave(isNew, name, description, isPublic, coverJpeg));
}

void LibraryStore::saveLocalPlaylist(bool isNew, const QString &name, const QString &description,
                                     const QByteArray &coverJpeg, const QVariant &firstSong)
{
    const Song song = firstSong.value<Song>();
    bool ok = false;
    if (name.trimmed().isEmpty()) {
        m_errors->show(QStringLiteral("Enter a playlist name."));
    } else if (m_localFiles && isNew) {
        const Playlist created = m_localFiles->createPlaylist(
            name, description, coverJpeg, song.isLocalFile() ? QList<Song> {song} : QList<Song> {});
        ok = created.isValid();
        // opened, as a new spotify playlist is
        if (ok) {
            selectPlaylist(created);
        }
    } else if (m_localFiles) {
        const Playlist target = selectedPlaylist();
        ok = target.isLocal() && m_localFiles->updatePlaylist(target.id, name, description, coverJpeg);
    }
    emit playlistSaveFinished(ok);
}

QCoro::Task<> LibraryStore::runSave(bool isNew, QString name, QString description, bool isPublic, QByteArray coverJpeg)
{
    const auto finish = [this](bool ok) {
        setLoading(m_savingPlaylist, false);
        emit playlistSaveFinished(ok);
    };
    if (!m_session->hasFullLibraryAccess()) {
        m_errors->show(kReconnectToEdit);
        finish(false);
        co_return;
    }
    if (!m_session->hasLibraryToken()) {
        m_errors->show(isNew ? QStringLiteral("Connect your Spotify library to create playlists.") : QString());
        finish(false);
        co_return;
    }
    name = name.trimmed();
    description = description.trimmed();
    if (name.isEmpty()) {
        m_errors->show(QStringLiteral("Enter a playlist name."));
        finish(false);
        co_return;
    }

    setLoading(m_savingPlaylist, true);
    Playlist target;
    try {
        if (isNew) {
            target = co_await m_session->withLibraryAccessToken<Playlist>([this, name, description, isPublic](QString accessToken) {
                return m_session->webApi().createPlaylist(name, description, isPublic, accessToken);
            });
            QList<Playlist> playlists = m_playlistList;
            playlists.prepend(target);
            setPlaylistList(playlists);
            navigate({Page::Playlist, target.id});
            setSongs({});
        } else {
            target = selectedPlaylist();
            if (!target.isValid()) {
                finish(false);
                co_return;
            }
            co_await m_session->withLibraryAccessToken<void>([this, id = target.id, name, description, isPublic](QString accessToken) {
                return m_session->webApi().updatePlaylist(id, name, description, isPublic, accessToken);
            });
            QList<Playlist> playlists = m_playlistList;
            for (Playlist &playlist : playlists) {
                if (playlist.id == target.id) {
                    playlist.name = name;
                    playlist.description = description;
                    playlist.isPublic = isPublic;
                    target = playlist;
                }
            }
            setPlaylistList(playlists);
        }
    } catch (const Cancelled &) {
        finish(false);
        co_return;
    } catch (const SpotifyError &error) {
        reportError(isNew ? QStringLiteral("Creating playlist") : QStringLiteral("Updating playlist"), error.message());
        finish(false);
        co_return;
    }

    // the macos app only uploaded a chosen cover when editing; a new playlist
    // gets its cover too.
    bool ok = true;
    if (!coverJpeg.isEmpty()) {
        ok = co_await uploadCover(target, coverJpeg);
    }
    finish(ok);
}

QCoro::Task<bool> LibraryStore::uploadCover(Playlist playlist, QByteArray jpeg)
{
    if (!playlist.canModify(m_profile.id) || !m_session->hasLibraryToken()) {
        co_return false;
    }
    try {
        co_await m_session->withLibraryAccessToken<void>([this, id = playlist.id, jpeg](QString accessToken) {
            return m_session->webApi().setPlaylistImage(id, jpeg, accessToken);
        });
    } catch (const Cancelled &) {
        co_return false;
    } catch (const SpotifyError &error) {
        reportError(QStringLiteral("Setting the cover for \"%1\"").arg(playlist.name), error.message());
        co_return false;
    }
    detach(refreshCover(playlist));
    co_return true;
}

QCoro::Task<> LibraryStore::refreshCover(Playlist playlist)
{
    // spotify mints a new image url for an uploaded cover once it has processed
    // it, and the old url keeps serving the old picture until then.
    const QUrl staleUrl = playlist.artworkUrl;
    co_await loadPlaylists();
    const auto currentUrl = [&] {
        for (const Playlist &item : m_playlistList) {
            if (item.id == playlist.id) {
                return item.artworkUrl;
            }
        }
        return QUrl();
    };
    if (currentUrl() != staleUrl) {
        co_return;
    }
    co_await QCoro::sleepFor(std::chrono::seconds(4));
    co_await loadPlaylists();
}

// --- logout ------------------------------------------------------------------------------

void LibraryStore::reset()
{
    m_pendingSavedUris.clear();
    ++m_savedStatusRevision;
    m_songLoad.cancel();
    m_artistLoad.cancel();
    m_search.cancel();
    m_hasLoadedHome = false;
    m_hasLoadedSavedSongs = false;
    m_savedSongTotal = -1;
    emit savedSongTotalChanged();

    setProfile({});
    setPlaylistList({});
    m_songs.clear();
    m_playlistSongs->clear();
    m_displayedPlaylistSongs->clear();
    m_openedPlaylists.clear();
    m_songListCache.clear();
    m_openedAlbums.clear();
    m_openedArtists.clear();
    m_savedSongs->clear();
    m_recentlyPlayed->clear();
    m_recommendedAlbums->setItems({});
    m_recommendedArtists->setItems({});
    m_albumSongs->clear();
    m_artistAlbums->setItems({});
    m_artistTopSongs->clear();
    m_searchSongs->clear();
    m_searchAlbums->setItems({});
    m_searchArtists->setItems({});
    m_searchPlaylists->setItems({});
    m_savedUris.clear();
    ++m_savedRevision;

    m_history.clear();
    m_restoreTarget.reset();
    m_destination = {Page::Home};
    m_searchQuery.clear();
    m_playlistFilterQuery.clear();
    m_sortOrder = SortOrder::Playlist;
    m_sortReversed = false;

    m_loadingPlaylists = m_loadingSongs = m_loadingHome = m_loadingSavedSongs = false;
    m_searching = m_loadingArtist = m_savingPlaylist = false;
    emit loadingChanged();
    emit destinationChanged();
    emit selectedPlaylistChanged();
    emit searchChanged();
    emit savedChanged();
    emit playlistViewChanged();
}

} // namespace swiftify
