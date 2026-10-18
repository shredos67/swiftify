#include "library/LocalLibrary.h"

#include "app/Paths.h"
#include "app/Settings.h"

#include <swiftify_core.h>

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSet>
#include <QThreadPool>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace swiftify {

Q_LOGGING_CATEGORY(lcLocal, "swiftify.local")

namespace {

LocalLibrary *s_instance = nullptr;

const QString kSourcesKey = QStringLiteral("localMusicSources");
constexpr int kCacheVersion = 1;
constexpr int kPlaylistsVersion = 1;
const QString kLocalPrefix = QStringLiteral("local:");

/// what the core's decoder plays (spotify-core's local::AUDIO_EXTENSIONS)
const QSet<QString> kMusicExtensions {
    QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("oga"),
    QStringLiteral("m4a"), QStringLiteral("mp4"), QStringLiteral("aac"), QStringLiteral("alac"),
    QStringLiteral("wav"), QStringLiteral("aif"), QStringLiteral("aiff"),
};

QJsonObject readTags(const QString &path, const QString &artworkDirectory)
{
    char *error = nullptr;
    char *json = swiftify_read_local_tags(path.toUtf8().constData(), artworkDirectory.toUtf8().constData(), &error);
    if (!json) {
        if (error) {
            qCDebug(lcLocal).noquote() << "skipping" << path << ':' << QString::fromUtf8(error);
            swiftify_string_free(error);
        }
        return {};
    }
    const QByteArray bytes(json);
    swiftify_string_free(json);
    return QJsonDocument::fromJson(bytes).object();
}

QString text(const QJsonObject &tags, const char *key)
{
    return tags.value(QLatin1StringView(key)).toString().trimmed();
}

/// a folder's own path, not a file or folder inside one already added
bool isWithin(const QString &path, const QString &folder)
{
    return path == folder || path.startsWith(folder.endsWith(u'/') ? folder : folder + u'/');
}

} // namespace

LocalLibrary::LocalLibrary(Settings *settings, QObject *parent, const QString &storageDirectory)
    : QObject(parent)
    , m_settings(settings)
    , m_songs(new SongListModel(this))
{
    if (!s_instance) {
        s_instance = this;
    }
    const QString storage = storageDirectory.isEmpty() ? paths::dataDirectory() : storageDirectory;
    QDir().mkpath(storage);
    m_cachePath = storage + QStringLiteral("/local-files.json");
    // a playlist's cover is the person's own, and cannot be taken out again
    m_playlistsPath = storage + QStringLiteral("/local-playlists.json");
    m_coverDirectory = storage + QStringLiteral("/local-playlist-covers");
    // the playlists' own copies of their tracks: the person's, kept as data
    m_trackStore = storage + QStringLiteral("/local-playlist-tracks");
    m_playlists = new PlaylistListModel(this);
    // covers taken out of files can always be taken out again
    m_artworkDirectory = (storageDirectory.isEmpty() ? paths::cacheDirectory() : storageDirectory)
        + QStringLiteral("/local-artwork");

    m_sources = m_settings->value(kSourcesKey).toStringList();
    m_playlistsReadable = loadPlaylists();
    loadCache();
    if (m_playlistsReadable) {
        sweepStore();
    } else {
        // what the copies belong to is in there: nothing of the store goes,
        // and the list is left as it is, kept aside once more besides
        const QString aside = m_playlistsPath + QStringLiteral(".unreadable");
        if (!QFileInfo::exists(aside)) {
            QFile::copy(m_playlistsPath, aside);
        }
    }
    publishPlaylists();
    adoptOutsideTracks();
    if (!m_sources.isEmpty() || QFileInfo::exists(m_trackStore)) {
        rescan();
    }
}

LocalLibrary::~LocalLibrary()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

LocalLibrary *LocalLibrary::instance()
{
    return s_instance;
}

LocalLibrary *LocalLibrary::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

bool LocalLibrary::isMusicFile(const QString &path)
{
    return kMusicExtensions.contains(QFileInfo(path).suffix().toLower());
}

int LocalLibrary::addSources(const QList<QUrl> &urls)
{
    QStringList sources = m_sources;
    int added = 0;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        const QString path = info.canonicalFilePath();
        if (path.isEmpty() || (info.isFile() && !isMusicFile(path)) || (!info.isFile() && !info.isDir())) {
            continue;
        }
        // already searched as part of a folder added before
        if (std::any_of(sources.begin(), sources.end(), [&](const QString &source) { return isWithin(path, source); })) {
            continue;
        }
        // a folder takes in what was added from inside it
        if (info.isDir()) {
            sources.removeIf([&](const QString &source) { return isWithin(source, path); });
        }
        sources.append(path);
        ++added;
    }
    if (added > 0) {
        setSources(sources);
        rescan();
    }
    return added;
}

void LocalLibrary::removeSource(const QString &path)
{
    QStringList sources = m_sources;
    if (sources.removeAll(path) == 0) {
        return;
    }
    setSources(sources);
    // gone at once, rather than when the search catches up
    QList<Song> kept;
    for (const Song &song : m_songs->songs()) {
        if (!isWithin(song.localPath(), path)) {
            kept.append(song);
        }
    }
    m_songs->setSongs(kept);
    publishPlaylists();
    rescan();
}

void LocalLibrary::setSources(const QStringList &sources)
{
    if (m_sources == sources) {
        return;
    }
    m_sources = sources;
    m_settings->setValue(kSourcesKey, m_sources);
    emit sourcesChanged();
}

void LocalLibrary::setScanning(bool scanning)
{
    if (m_scanning != scanning) {
        m_scanning = scanning;
        emit scanningChanged();
    }
}

void LocalLibrary::rescan()
{
    if (m_scanning) {
        // one search at a time; this one goes again once it is done
        m_rescanQueued = true;
        return;
    }
    const quint64 generation = ++m_scanGeneration;
    setScanning(true);
    QtConcurrent::run(QThreadPool::globalInstance(), &LocalLibrary::scan, scanSources(), m_entries, m_artworkDirectory,
                      m_cachePath)
        .then(this, [this, generation](const ScanResult &result) {
            if (generation != m_scanGeneration) {
                return;
            }
            m_entries = result.entries;
            // a song that is just as it was stays the same song: the one
            // playing stays marked, and the list keeps its rows
            QHash<QString, Song> before;
            for (const Song &song : m_songs->songs()) {
                before.insert(song.uri, song);
            }
            for (auto kept = m_storeSongs.cbegin(); kept != m_storeSongs.cend(); ++kept) {
                before.insert(kept.key(), *kept);
            }
            // the playlists' copies are songs of their own, not the folders'
            QList<Song> songs;
            QHash<QString, Song> keptSongs;
            for (Song song : result.songs) {
                const auto old = before.constFind(song.uri);
                if (old != before.cend() && old->name == song.name && old->artists == song.artists
                    && old->albumName == song.albumName && old->artworkUrl == song.artworkUrl
                    && old->durationMs == song.durationMs) {
                    song.instanceId = old->instanceId;
                }
                if (isKeptCopy(song.localPath())) {
                    keptSongs.insert(song.uri, song);
                } else {
                    songs.append(song);
                }
            }
            m_storeSongs = keptSongs;
            m_songs->setSongs(songs);
            publishPlaylists();
            setScanning(false);
            emit scanned(m_songs->count());
            if (std::exchange(m_rescanQueued, false)) {
                rescan();
            }
        });
}

void LocalLibrary::loadCache()
{
    QFile file(m_cachePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != kCacheVersion) {
        return;
    }
    const QJsonObject files = root.value(QStringLiteral("files")).toObject();
    QList<std::pair<QString, QJsonObject>> known;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        m_entries.insert(it.key(), {entry.value(QStringLiteral("size")).toInteger(),
                                    entry.value(QStringLiteral("modified")).toInteger(),
                                    entry.value(QStringLiteral("tags")).toObject()});
    }
    // what was there last time shows at once; the search that follows
    // brings it up to date
    QHash<QString, Entry> shown;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (isKeptCopy(it.key())) {
            if (!it->tags.isEmpty()) {
                const Song song = songFor(it.key(), it->tags);
                m_storeSongs.insert(song.uri, song);
            }
        } else if (std::any_of(m_sources.begin(), m_sources.end(),
                               [&](const QString &source) { return isWithin(it.key(), source); })) {
            shown.insert(it.key(), *it);
        }
    }
    m_songs->setSongs(songsFrom(shown));
}

QStringList LocalLibrary::scanSources() const
{
    QStringList sources = m_sources;
    if (QFileInfo(m_trackStore).isDir()) {
        sources.append(m_trackStore);
    }
    return sources;
}

bool LocalLibrary::isKeptCopy(const QString &path) const
{
    return !path.isEmpty() && isWithin(path, m_trackStore) && path != m_trackStore;
}

QList<Song> LocalLibrary::allSongs() const
{
    QList<Song> songs = m_songs->songs();
    songs.append(m_storeSongs.values());
    return songs;
}

QList<Song> LocalLibrary::songsFrom(const QHash<QString, Entry> &entries)
{
    struct Found {
        Song song;
        QString albumArtist;
        int disc = 0;
        int track = 0;
    };
    QList<Found> found;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (it->tags.isEmpty()) {
            continue;
        }
        Found item {songFor(it.key(), it->tags), text(it->tags, "albumArtist"),
                    it->tags.value(QStringLiteral("discNumber")).toInt(),
                    it->tags.value(QStringLiteral("trackNumber")).toInt()};
        if (item.albumArtist.isEmpty()) {
            item.albumArtist = item.song.artists;
        }
        found.append(item);
    }

    // by artist and album, then in the album's own order
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(found.begin(), found.end(), [&](const Found &a, const Found &b) {
        if (const int artist = collator.compare(a.albumArtist, b.albumArtist); artist != 0) {
            return artist < 0;
        }
        if (const int album = collator.compare(a.song.albumName, b.song.albumName); album != 0) {
            return album < 0;
        }
        if (a.disc != b.disc) {
            return a.disc < b.disc;
        }
        if (a.track != b.track) {
            return a.track < b.track;
        }
        return collator.compare(a.song.localPath(), b.song.localPath()) < 0;
    });

    QList<Song> songs;
    songs.reserve(found.size());
    for (const Found &item : found) {
        songs.append(item.song);
    }
    return songs;
}

LocalLibrary::ScanResult LocalLibrary::scan(const QStringList &sources, QHash<QString, Entry> known,
                                            const QString &artworkDirectory, const QString &cachePath)
{
    QStringList files;
    QSet<QString> seen;
    const auto take = [&](const QFileInfo &info) {
        const QString path = info.absoluteFilePath();
        if (isMusicFile(path) && !seen.contains(path)) {
            seen.insert(path);
            files.append(path);
        }
    };
    for (const QString &source : sources) {
        const QFileInfo info(source);
        if (info.isFile()) {
            take(info);
            continue;
        }
        // hidden files are left out, and with them the "._" shadows macos
        // leaves on other disks
        QDirIterator walk(source, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            walk.next();
            take(walk.fileInfo());
        }
    }

    QHash<QString, Entry> entries;
    for (const QString &path : files) {
        const QFileInfo info(path);
        Entry entry {info.size(), info.lastModified().toMSecsSinceEpoch(), {}};
        const auto cached = known.constFind(path);
        if (cached != known.cend() && cached->size == entry.size && cached->modified == entry.modified) {
            entry.tags = cached->tags;
        } else {
            entry.tags = readTags(path, artworkDirectory);
            // an unreadable file is remembered as such, and not tried again
            // until it changes
        }
        entries.insert(path, entry);
    }

    ScanResult result;
    result.songs = songsFrom(entries);
    result.entries = entries;

    // kept for next time, written aside and moved into place
    QJsonObject stored;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        stored.insert(it.key(), QJsonObject {{QStringLiteral("size"), it->size},
                                             {QStringLiteral("modified"), it->modified},
                                             {QStringLiteral("tags"), it->tags}});
    }
    QSaveFile file(cachePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject {{QStringLiteral("version"), kCacheVersion},
                                              {QStringLiteral("files"), stored}})
                       .toJson(QJsonDocument::Compact));
        file.commit();
    }
    return result;
}

Song LocalLibrary::songFor(const QString &path, const QJsonObject &tags)
{
    const QFileInfo info(path);
    Song song;
    song.uri = QUrl::fromLocalFile(info.absoluteFilePath()).toString();
    song.name = text(tags, "title");
    if (song.name.isEmpty()) {
        song.name = info.completeBaseName();
    }
    song.artists = text(tags, "artist");
    if (song.artists.isEmpty()) {
        song.artists = text(tags, "albumArtist");
    }
    if (song.artists.isEmpty()) {
        song.artists = QStringLiteral("Unknown Artist");
    }
    song.albumName = text(tags, "album");
    if (song.albumName.isEmpty()) {
        song.albumName = info.dir().dirName();
    }
    song.durationMs = tags.value(QStringLiteral("durationMs")).toInt();
    const QString artwork = text(tags, "artworkPath");
    if (!artwork.isEmpty()) {
        song.artworkUrl = QUrl::fromLocalFile(artwork);
    }
    return song;
}

// --- tags ------------------------------------------------------------------------------

namespace {

/// the keys an edit may change, as LocalTags and TagEdits name them
const QStringList kEditableTags {
    QStringLiteral("title"),  QStringLiteral("artist"), QStringLiteral("album"),
    QStringLiteral("albumArtist"), QStringLiteral("genre"), QStringLiteral("date"),
    QStringLiteral("trackNumber"), QStringLiteral("discNumber"),
};

} // namespace

QVariantMap LocalLibrary::tagsOf(const QString &uri) const
{
    const auto entry = m_entries.constFind(QUrl(uri).toLocalFile());
    if (entry == m_entries.cend()) {
        return {};
    }
    QVariantMap tags;
    for (const QString &key : kEditableTags) {
        const QJsonValue value = entry->tags.value(key);
        if (value.isString() && !value.toString().isEmpty()) {
            tags.insert(key, value.toString());
        } else if (value.isDouble()) {
            tags.insert(key, QString::number(value.toInteger()));
        }
    }
    const QString artwork = entry->tags.value(QStringLiteral("artworkPath")).toString();
    if (!artwork.isEmpty()) {
        tags.insert(QStringLiteral("artworkUrl"), QUrl::fromLocalFile(artwork));
    }
    return tags;
}

void LocalLibrary::editTags(const QString &uri, const QVariantMap &fields, const QByteArray &artworkJpeg,
                            bool removeArtwork)
{
    const QString path = QUrl(uri).toLocalFile();
    if (path.isEmpty() || !m_entries.contains(path)) {
        emit tagsSaved(uri, false, QStringLiteral("Only music files in Local Files can be edited."));
        return;
    }
    QJsonObject edits;
    for (const QString &key : kEditableTags) {
        if (fields.contains(key)) {
            edits.insert(key, fields.value(key).toString());
        }
    }
    edits.insert(QStringLiteral("removeArtwork"), removeArtwork);
    const QByteArray json = QJsonDocument(edits).toJson(QJsonDocument::Compact);
    // written away from the window: a large file is copied whole
    QtConcurrent::run(QThreadPool::globalInstance(), [path, json, artworkJpeg]() -> QString {
        char *error = swiftify_write_local_tags(path.toUtf8().constData(), json.constData(),
                                                reinterpret_cast<const uint8_t *>(artworkJpeg.constData()),
                                                size_t(artworkJpeg.size()));
        if (!error) {
            return {};
        }
        const QString message = QString::fromUtf8(error);
        swiftify_string_free(error);
        return message;
    }).then(this, [this, uri, path](const QString &error) {
        if (!error.isEmpty()) {
            qCWarning(lcLocal).noquote() << "editing" << path << ':' << error;
            emit tagsSaved(uri, false, error);
            return;
        }
        // read again however little its size and time moved
        m_entries.remove(path);
        rescan();
        emit tagsSaved(uri, true, {});
    });
}

// --- playlists ---------------------------------------------------------------------

bool LocalLibrary::loadPlaylists()
{
    QFile file(m_playlistsPath);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcLocal).noquote() << "could not read the playlists in" << m_playlistsPath << ':' << file.errorString();
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(lcLocal).noquote() << "could not read the playlists in" << m_playlistsPath << ':' << error.errorString();
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kPlaylistsVersion) {
        qCWarning(lcLocal) << "playlists kept in a version this app does not read:" << m_playlistsPath;
        return false;
    }
    for (const QJsonValue &value : root.value(QStringLiteral("playlists")).toArray()) {
        const QJsonObject object = value.toObject();
        StoredPlaylist stored {object.value(QStringLiteral("id")).toString(),
                               object.value(QStringLiteral("name")).toString(),
                               object.value(QStringLiteral("description")).toString(),
                               object.value(QStringLiteral("cover")).toString(),
                               {}};
        if (!stored.id.startsWith(kLocalPrefix) || findPlaylist(stored.id)) {
            continue;
        }
        for (const QJsonValue &uri : object.value(QStringLiteral("songs")).toArray()) {
            stored.uris.append(uri.toString());
        }
        m_storedPlaylists.append(stored);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("copies")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString copy = object.value(QStringLiteral("copy")).toString();
        if (isKeptCopy(copy)) {
            m_copies.insert(copy, {object.value(QStringLiteral("source")).toString(),
                                   object.value(QStringLiteral("size")).toInteger(),
                                   object.value(QStringLiteral("modified")).toInteger()});
        }
    }
    return true;
}

void LocalLibrary::savePlaylists() const
{
    if (!m_playlistsReadable) {
        // writing over a list this app could not read would lose what it holds
        qCWarning(lcLocal) << "playlists not kept: the list there could not be read:" << m_playlistsPath;
        return;
    }
    QJsonArray playlists;
    for (const StoredPlaylist &stored : m_storedPlaylists) {
        playlists.append(QJsonObject {{QStringLiteral("id"), stored.id},
                                      {QStringLiteral("name"), stored.name},
                                      {QStringLiteral("description"), stored.description},
                                      {QStringLiteral("cover"), stored.cover},
                                      {QStringLiteral("songs"), QJsonArray::fromStringList(stored.uris)}});
    }
    QSaveFile file(m_playlistsPath);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcLocal).noquote() << "could not keep the playlists in" << m_playlistsPath << ':' << file.errorString();
        return;
    }
    QJsonArray copies;
    for (auto it = m_copies.cbegin(); it != m_copies.cend(); ++it) {
        copies.append(QJsonObject {{QStringLiteral("copy"), it.key()},
                                   {QStringLiteral("source"), it->source},
                                   {QStringLiteral("size"), it->size},
                                   {QStringLiteral("modified"), it->modified}});
    }
    file.write(QJsonDocument(QJsonObject {{QStringLiteral("version"), kPlaylistsVersion},
                                          {QStringLiteral("playlists"), playlists},
                                          {QStringLiteral("copies"), copies}})
                   .toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        qCWarning(lcLocal).noquote() << "could not keep the playlists in" << m_playlistsPath << ':' << file.errorString();
    }
}

void LocalLibrary::publishPlaylists()
{
    const SongIndex index = indexSongs();
    QList<Playlist> playlists;
    playlists.reserve(m_storedPlaylists.size());
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        playlists.append(describe(stored, index));
    }
    m_playlists->setItems(playlists);
    emit playlistsChanged();
}

LocalLibrary::StoredPlaylist *LocalLibrary::findPlaylist(const QString &id)
{
    const auto found = std::find_if(m_storedPlaylists.begin(), m_storedPlaylists.end(),
                                    [&](const StoredPlaylist &stored) { return stored.id == id; });
    return found == m_storedPlaylists.end() ? nullptr : &*found;
}

const LocalLibrary::StoredPlaylist *LocalLibrary::findPlaylist(const QString &id) const
{
    return const_cast<LocalLibrary *>(this)->findPlaylist(id);
}

LocalLibrary::SongIndex LocalLibrary::indexSongs() const
{
    SongIndex index;
    const QList<Song> &library = m_songs->songs();
    index.reserve(library.size() + m_storeSongs.size());
    for (const Song &song : library) {
        index.insert(song.uri, &song);
    }
    for (auto kept = m_storeSongs.cbegin(); kept != m_storeSongs.cend(); ++kept) {
        index.insert(kept.key(), &kept.value());
    }
    return index;
}

QList<Song> LocalLibrary::resolve(const StoredPlaylist &stored, const SongIndex &index) const
{
    QList<Song> songs;
    songs.reserve(stored.uris.size());
    for (const QString &uri : stored.uris) {
        if (const Song *song = index.value(uri)) {
            songs.append(song->freshInstance());
        }
    }
    return songs;
}

Playlist LocalLibrary::describe(const StoredPlaylist &stored, const SongIndex &index) const
{
    Playlist playlist;
    playlist.id = stored.id;
    playlist.uri = stored.id;
    playlist.name = stored.name;
    playlist.description = stored.description;
    playlist.ownerName = QStringLiteral("Local Files");
    const QList<Song> songs = resolve(stored, index);
    playlist.songCount = int(songs.size());
    if (!stored.cover.isEmpty()) {
        playlist.artworkUrl = QUrl::fromLocalFile(m_coverDirectory + u'/' + stored.cover);
    } else {
        // the first cover among its songs, as a playlist without one of its
        // own shows in spotify
        for (const Song &song : songs) {
            if (!song.artworkUrl.isEmpty()) {
                playlist.artworkUrl = song.artworkUrl;
                break;
            }
        }
    }
    return playlist;
}

Playlist LocalLibrary::playlist(const QString &id) const
{
    // as last published, which every change is
    for (const Playlist &playlist : m_playlists->items()) {
        if (playlist.id == id) {
            return playlist;
        }
    }
    return {};
}

QVariantList LocalLibrary::playlistChoices() const
{
    QVariantList choices;
    for (const Playlist &playlist : m_playlists->items()) {
        choices.append(QVariant::fromValue(playlist));
    }
    return choices;
}

QList<Song> LocalLibrary::songsOf(const QString &id) const
{
    const StoredPlaylist *stored = findPlaylist(id);
    return stored ? resolve(*stored, indexSongs()) : QList<Song> {};
}

QString LocalLibrary::writeCover(const StoredPlaylist &stored, const QByteArray &jpeg) const
{
    QDir().mkpath(m_coverDirectory);
    const QString name = stored.id.mid(kLocalPrefix.size()) + u'-'
        + QUuid::createUuid().toString(QUuid::Id128).left(8) + QStringLiteral(".jpg");
    QSaveFile file(m_coverDirectory + u'/' + name);
    if (!file.open(QIODevice::WriteOnly) || file.write(jpeg) != jpeg.size() || !file.commit()) {
        qCWarning(lcLocal).noquote() << "could not keep the cover for" << stored.name << ':' << file.errorString();
        return stored.cover;
    }
    if (!stored.cover.isEmpty()) {
        QFile::remove(m_coverDirectory + u'/' + stored.cover);
    }
    return name;
}

Playlist LocalLibrary::createPlaylist(const QString &name, const QString &description, const QByteArray &coverJpeg,
                                      const QList<Song> &songs)
{
    StoredPlaylist stored {kLocalPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces), name.trimmed(),
                           description.trimmed(), {}, {}};
    if (stored.name.isEmpty()) {
        return {};
    }
    if (!coverJpeg.isEmpty()) {
        stored.cover = writeCover(stored, coverJpeg);
    }
    // newest first, as spotify lists the person's own
    m_storedPlaylists.prepend(stored);
    savePlaylists();
    publishPlaylists();
    // the songs it starts with go in once copied
    addSongs(stored.id, songs);
    return playlist(stored.id);
}

bool LocalLibrary::updatePlaylist(const QString &id, const QString &name, const QString &description,
                                  const QByteArray &coverJpeg)
{
    StoredPlaylist *stored = findPlaylist(id);
    if (!stored || name.trimmed().isEmpty()) {
        return false;
    }
    stored->name = name.trimmed();
    stored->description = description.trimmed();
    if (!coverJpeg.isEmpty()) {
        stored->cover = writeCover(*stored, coverJpeg);
    }
    savePlaylists();
    publishPlaylists();
    return true;
}

bool LocalLibrary::deletePlaylist(const QString &id)
{
    const StoredPlaylist *stored = findPlaylist(id);
    if (!stored) {
        return false;
    }
    if (!stored->cover.isEmpty()) {
        QFile::remove(m_coverDirectory + u'/' + stored->cover);
    }
    m_storedPlaylists.removeIf([&](const StoredPlaylist &item) { return item.id == id; });
    collectCopies();
    savePlaylists();
    publishPlaylists();
    return true;
}

int LocalLibrary::addSongs(const QString &id, const QList<Song> &songs)
{
    QStringList paths;
    for (const Song &song : songs) {
        if (song.isLocalFile()) {
            paths.append(song.localPath());
        }
    }
    if (!findPlaylist(id) || paths.isEmpty()) {
        return 0;
    }
    keepTracks(id, paths);
    return int(paths.size());
}

void LocalLibrary::addFiles(const QString &playlistId, const QList<QUrl> &urls)
{
    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (!findPlaylist(playlistId) || paths.isEmpty()) {
        emit tracksAdded(playlistId, 0, QStringLiteral("There was nothing there to add."));
        return;
    }
    keepTracks(playlistId, paths);
}

int LocalLibrary::removeSong(const QString &id, const QString &uri)
{
    StoredPlaylist *stored = findPlaylist(id);
    const int removed = stored ? int(stored->uris.removeAll(uri)) : 0;
    if (removed > 0) {
        collectCopies();
        savePlaylists();
        publishPlaylists();
    }
    return removed;
}

// --- the playlists' own copies ---------------------------------------------------------

namespace {

/// the cover kept beside an album's files, by the names read_tags looks for
const QStringList kFolderCoverNames {QStringLiteral("cover"), QStringLiteral("folder"), QStringLiteral("front"),
                                     QStringLiteral("album"), QStringLiteral("albumart"), QStringLiteral("artwork")};
const QStringList kCoverExtensions {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                                    QStringLiteral("webp")};

/// a track's lyrics (a .lrc of its name) and its folder's cover go with it
void copyCompanions(const QFileInfo &track, const QString &into)
{
    const QFileInfoList beside = track.dir().entryInfoList(QDir::Files | QDir::Readable);
    for (const QFileInfo &file : beside) {
        const QString suffix = file.suffix().toLower();
        const bool lyrics = suffix == QLatin1StringView("lrc") && file.completeBaseName() == track.completeBaseName();
        const bool cover = kCoverExtensions.contains(suffix) && kFolderCoverNames.contains(file.completeBaseName().toLower());
        if (lyrics || cover) {
            QFile::copy(file.absoluteFilePath(), into + u'/' + file.fileName());
        }
    }
}

} // namespace

LocalLibrary::KeepResult LocalLibrary::keep(const QStringList &inputs, const QString &store,
                                            const QString &artworkDirectory, const QHash<QString, Origin> &origins)
{
    KeepResult result;

    // a folder gives its music files, in the order of their names
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    QStringList paths;
    for (const QString &input : inputs) {
        const QFileInfo info(input);
        if (info.isDir()) {
            QStringList found;
            QDirIterator walk(input, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (walk.hasNext()) {
                walk.next();
                if (isMusicFile(walk.filePath())) {
                    found.append(walk.fileInfo().absoluteFilePath());
                }
            }
            std::sort(found.begin(), found.end(),
                      [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
            paths.append(found);
        } else if (info.isFile() && isMusicFile(input)) {
            paths.append(info.absoluteFilePath());
        } else {
            result.failed.append(info.fileName());
        }
    }

    // a file copied before, unchanged since, is not copied again
    QHash<QString, Origin> known = origins;
    QHash<QString, QString> bySource;
    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        bySource.insert(it->source, it.key());
    }

    for (const QString &path : paths) {
        Kept kept;
        kept.source = path;
        if (isWithin(path, store)) {
            kept.uri = QUrl::fromLocalFile(path).toString();
            result.kept.append(kept);
            continue;
        }
        const QFileInfo info(path);
        const QString source = info.canonicalFilePath();
        const qint64 size = info.size();
        const qint64 modified = info.lastModified().toMSecsSinceEpoch();
        if (const auto copy = bySource.constFind(source); copy != bySource.cend()) {
            const Origin origin = known.value(*copy);
            if (origin.size == size && origin.modified == modified && QFileInfo::exists(*copy)) {
                kept.uri = QUrl::fromLocalFile(*copy).toString();
                result.kept.append(kept);
                continue;
            }
        }

        // a folder of its own, holding the file under its own name in a
        // folder of its old one's name: a file with no tags is still named
        // after itself, and its album after its folder
        const QString folder = store + u'/' + QUuid::createUuid().toString(QUuid::Id128).left(12);
        const QString parent = info.dir().dirName().isEmpty() ? QStringLiteral("Music") : info.dir().dirName();
        const QString home = folder + u'/' + parent;
        const QString copy = home + u'/' + info.fileName();
        const QString partial = copy + QStringLiteral(".partial");
        const auto fail = [&] {
            QDir(folder).removeRecursively();
            result.failed.append(info.fileName());
        };
        // written aside and moved into place: a copy cut short is never taken
        // for the song
        if (!QDir().mkpath(home) || !QFile::copy(path, partial) || !QFile::rename(partial, copy)) {
            fail();
            continue;
        }
        copyCompanions(info, home);
        const QFileInfo copied(copy);
        Entry entry {copied.size(), copied.lastModified().toMSecsSinceEpoch(), readTags(copy, artworkDirectory)};
        if (entry.tags.isEmpty()) {
            fail();
            continue;
        }
        kept.uri = QUrl::fromLocalFile(copy).toString();
        kept.copied = true;
        kept.origin = {source, size, modified};
        kept.entry = entry;
        known.insert(copy, kept.origin);
        bySource.insert(source, copy);
        result.kept.append(kept);
    }
    return result;
}

void LocalLibrary::adoptKept(const KeepResult &result)
{
    for (const Kept &kept : result.kept) {
        if (!kept.copied) {
            continue;
        }
        const QString path = QUrl(kept.uri).toLocalFile();
        m_entries.insert(path, kept.entry);
        m_copies.insert(path, kept.origin);
        m_storeSongs.insert(kept.uri, songFor(path, kept.entry.tags));
    }
}

void LocalLibrary::setCopying(int delta)
{
    const bool was = m_copying > 0;
    m_copying = std::max(0, m_copying + delta);
    if (was != (m_copying > 0)) {
        emit copyingChanged();
    }
}

void LocalLibrary::keepTracks(const QString &id, const QStringList &paths)
{
    setCopying(1);
    m_keepQueue.append({id, paths});
    keepNext();
}

void LocalLibrary::keepNext()
{
    if (m_keeping || m_keepQueue.isEmpty()) {
        return;
    }
    m_keeping = true;
    const auto [id, paths] = m_keepQueue.takeFirst();
    // copied away from the window: a long album is a lot of bytes. what was
    // copied before this began is known to it, so nothing is copied twice
    QtConcurrent::run(QThreadPool::globalInstance(), &LocalLibrary::keep, paths, m_trackStore, m_artworkDirectory,
                      m_copies)
        .then(this, [this, id](const KeepResult &result) {
            m_keeping = false;
            finishKeeping(id, result);
            setCopying(-1);
            keepNext();
        });
}

void LocalLibrary::finishKeeping(const QString &id, const KeepResult &result)
{
    adoptKept(result);
    if (id.isEmpty()) {
        // each in its place
        QHash<QString, QString> copies;
        for (const Kept &kept : result.kept) {
            copies.insert(kept.source, kept.uri);
        }
        for (StoredPlaylist &stored : m_storedPlaylists) {
            for (QString &uri : stored.uris) {
                if (const auto copy = copies.constFind(QUrl(uri).toLocalFile()); copy != copies.cend()) {
                    uri = *copy;
                }
            }
        }
        collectCopies();
        savePlaylists();
        publishPlaylists();
        return;
    }

    StoredPlaylist *stored = findPlaylist(id);
    if (stored) {
        for (const Kept &kept : result.kept) {
            stored->uris.append(kept.uri);
        }
    }
    // copied for a playlist deleted meanwhile: let go again
    collectCopies();
    savePlaylists();
    publishPlaylists();
    QString problem;
    if (result.failed.size() == 1) {
        problem = QStringLiteral("“%1” could not be added.").arg(result.failed.first());
    } else if (!result.failed.isEmpty()) {
        problem = QStringLiteral("%1 files could not be added.").arg(result.failed.size());
    }
    emit tracksAdded(id, stored ? int(result.kept.size()) : 0, problem);
}

void LocalLibrary::adoptOutsideTracks()
{
    QStringList outside;
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        for (const QString &uri : stored.uris) {
            const QString path = QUrl(uri).toLocalFile();
            if (!path.isEmpty() && !isKeptCopy(path) && QFileInfo(path).isFile() && !outside.contains(path)) {
                outside.append(path);
            }
        }
    }
    if (!outside.isEmpty()) {
        keepTracks({}, outside);
    }
}

void LocalLibrary::collectCopies()
{
    QSet<QString> held;
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        for (const QString &uri : stored.uris) {
            held.insert(QUrl(uri).toLocalFile());
        }
    }
    const QDir store(m_trackStore);
    for (auto it = m_copies.begin(); it != m_copies.end();) {
        if (held.contains(it.key())) {
            ++it;
            continue;
        }
        // the folder it was given goes whole, its lyrics and cover with it
        const QString folder = store.relativeFilePath(it.key()).section(u'/', 0, 0);
        if (!folder.isEmpty() && folder != QLatin1StringView("..")) {
            QDir(store.filePath(folder)).removeRecursively();
        }
        m_entries.remove(it.key());
        m_storeSongs.remove(QUrl::fromLocalFile(it.key()).toString());
        it = m_copies.erase(it);
    }
}

void LocalLibrary::sweepStore()
{
    // a folder a copy was being written into when the app stopped, or one
    // whose playlist went while it was away
    const QDir store(m_trackStore);
    QSet<QString> known;
    const auto folderOf = [&](const QString &path) {
        const QString relative = store.relativeFilePath(path);
        return relative.startsWith(QLatin1StringView("..")) ? QString() : relative.section(u'/', 0, 0);
    };
    for (auto it = m_copies.cbegin(); it != m_copies.cend(); ++it) {
        known.insert(folderOf(it.key()));
    }
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        for (const QString &uri : stored.uris) {
            known.insert(folderOf(QUrl(uri).toLocalFile()));
        }
    }
    for (const QString &folder : store.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!known.contains(folder)) {
            QDir(store.filePath(folder)).removeRecursively();
        }
    }
}

void LocalLibrary::reorder(const QString &id, const QStringList &uris)
{
    StoredPlaylist *stored = findPlaylist(id);
    if (!stored) {
        return;
    }
    // only what it holds, as often as it holds it; what the library no
    // longer finds keeps its place after the rest
    QHash<QString, int> held;
    for (const QString &uri : std::as_const(stored->uris)) {
        ++held[uri];
    }
    QStringList ordered;
    ordered.reserve(stored->uris.size());
    for (const QString &uri : uris) {
        if (int &left = held[uri]; left > 0) {
            --left;
            ordered.append(uri);
        }
    }
    for (const QString &uri : std::as_const(stored->uris)) {
        if (int &left = held[uri]; left > 0) {
            --left;
            ordered.append(uri);
        }
    }
    if (ordered == stored->uris) {
        return;
    }
    stored->uris = ordered;
    savePlaylists();
    publishPlaylists();
}

} // namespace swiftify
