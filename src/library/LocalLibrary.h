#pragma once

#include "library/CollectionModels.h"
#include "library/SongListModel.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <utility>

class QQmlEngine;
class QJSEngine;

namespace swiftify {

class Settings;

/// music files of the person's own (LocalFiles in qml): the folders and
/// single files they added, searched for music, each song made from what its
/// own tags say: title, artists, album, track and disc, cover. tags are read
/// once and kept, by the file's size and time, so a library is read again
/// only where it changed. a file whose tags say nothing still gets a name
/// from its file and an album from its folder.
///
/// it also keeps playlists made on this computer: each a name, a
/// description, a cover and its tracks in order. a playlist holds copies of
/// its tracks of its own, taken from anywhere on disk and kept with the app,
/// so an original moved or deleted leaves it whole. a copy is kept once for
/// every playlist that holds it, and goes when none does.
class LocalLibrary : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(LocalFiles)
    QML_SINGLETON

    /// what was added: folders, searched all the way down, and single files
    Q_PROPERTY(QStringList sources READ sources NOTIFY sourcesChanged FINAL)
    Q_PROPERTY(swiftify::SongListModel *songs READ songs CONSTANT FINAL)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged FINAL)
    /// the playlists made here, newest first
    Q_PROPERTY(swiftify::PlaylistListModel *playlists READ playlists CONSTANT FINAL)
    /// tracks are being copied into a playlist
    Q_PROPERTY(bool copying READ isCopying NOTIFY copyingChanged FINAL)

public:
    /// `storageDirectory` holds what the tags said and the covers taken out
    /// of files; the app's own data folder by default
    explicit LocalLibrary(Settings *settings, QObject *parent = nullptr, const QString &storageDirectory = {});
    ~LocalLibrary() override;

    static LocalLibrary *instance();
    static LocalLibrary *create(QQmlEngine *, QJSEngine *);

    /// a file the player can play, going by its extension
    static bool isMusicFile(const QString &path);

    QStringList sources() const { return m_sources; }
    SongListModel *songs() const { return m_songs; }
    bool isScanning() const { return m_scanning; }
    PlaylistListModel *playlists() const { return m_playlists; }
    bool isCopying() const { return m_copying > 0; }
    /// every song of one's own: the folders added, and the playlists' copies
    QList<Song> allSongs() const;

    /// a playlist made here; invalid if there is none by that id
    Playlist playlist(const QString &id) const;
    /// the playlists made here, for a menu of where a song can go
    Q_INVOKABLE QVariantList playlistChoices() const;
    /// a playlist's songs as the library has them now, each its own
    /// occurrence
    QList<Song> songsOf(const QString &id) const;
    /// a new playlist, to hold copies of the files among `songs`
    Playlist createPlaylist(const QString &name, const QString &description, const QByteArray &coverJpeg,
                            const QList<Song> &songs);
    /// a new name and description, and a new cover if one is given
    bool updatePlaylist(const QString &id, const QString &name, const QString &description, const QByteArray &coverJpeg);
    bool deletePlaylist(const QString &id);
    /// copies the files among `songs` in and adds them at the end, in the
    /// background; how many were asked for. reports through tracksAdded
    int addSongs(const QString &id, const QList<Song> &songs);
    /// copies music files, or every one in a folder, from anywhere into the
    /// playlist's keeping and adds them at its end. reports through tracksAdded
    Q_INVOKABLE void addFiles(const QString &playlistId, const QList<QUrl> &urls);
    /// takes a track out wherever it is in the playlist; how many times
    int removeSong(const QString &id, const QString &uri);
    /// puts the files it shows in this order, and any it no longer finds
    /// after them
    void reorder(const QString &id, const QStringList &uris);

    /// what a local song's tags say, as written in the file: title, artist,
    /// album, albumArtist, genre, date, trackNumber, discNumber and
    /// artworkUrl, each missing when the file does not say it
    Q_INVOKABLE QVariantMap tagsOf(const QString &uri) const;
    /// writes `fields` (any of those above, as text; an empty one clears it)
    /// into a local song's file, with `artworkJpeg` as its cover when given
    /// or no cover at all with `removeArtwork`. the file is read again once
    /// written. reports through tagsSaved
    Q_INVOKABLE void editTags(const QString &uri, const QVariantMap &fields, const QByteArray &artworkJpeg,
                              bool removeArtwork);

    /// adds folders and music files, dropped or picked, and searches them.
    /// returns how many were new
    Q_INVOKABLE int addSources(const QList<QUrl> &urls);
    Q_INVOKABLE void removeSource(const QString &path);
    /// searches every source again, for files added, changed or gone
    Q_INVOKABLE void rescan();

signals:
    void sourcesChanged();
    void scanningChanged();
    /// a search finished, with how many songs there are now
    void scanned(int songCount);
    /// a playlist was made, changed or deleted, or the files it holds were
    void playlistsChanged();
    void copyingChanged();
    /// tracks asked for were copied into a playlist: how many went in, and
    /// what could not, if anything
    void tracksAdded(const QString &playlistId, int count, const QString &problem);
    /// an edit of a song's tags was written, or failed with `error`
    void tagsSaved(const QString &uri, bool ok, const QString &error);

private:
    struct Entry {
        qint64 size = 0;
        qint64 modified = 0;
        QJsonObject tags;
    };
    struct ScanResult {
        QList<Song> songs;
        QHash<QString, Entry> entries;
    };

    static ScanResult scan(const QStringList &sources, QHash<QString, Entry> known, const QString &artworkDirectory,
                           const QString &cachePath);
    static Song songFor(const QString &path, const QJsonObject &tags);
    /// the songs among `entries`, by artist and album, then track
    static QList<Song> songsFrom(const QHash<QString, Entry> &entries);
    struct StoredPlaylist {
        QString id;
        QString name;
        QString description;
        /// the cover's file in the covers folder; empty for none
        QString cover;
        QStringList uris;
    };
    /// the file a copy was taken from, as it was then: the same file added
    /// again is not copied again
    struct Origin {
        QString source;
        qint64 size = 0;
        qint64 modified = 0;
    };
    /// one track asked for, as it is kept
    struct Kept {
        /// the path it was asked for by
        QString source;
        QString uri;
        /// copied now, rather than one kept already
        bool copied = false;
        Origin origin;
        Entry entry;
    };
    struct KeepResult {
        QList<Kept> kept;
        QStringList failed;
    };

    /// copies `inputs` (files, and folders' music files) into `store`,
    /// reading each copy's tags; one kept already is used again. pure, for
    /// any thread
    static KeepResult keep(const QStringList &inputs, const QString &store, const QString &artworkDirectory,
                           const QHash<QString, Origin> &origins);
    /// copies tracks in and adds them to the playlist `id`, after whatever
    /// is being copied already, so they land in the order they were asked
    /// for. with no id, the tracks of every playlist still outside are
    /// copied in, each in its place
    void keepTracks(const QString &id, const QStringList &paths);
    void keepNext();
    void finishKeeping(const QString &id, const KeepResult &result);
    /// takes what `keep` copied into the library's own reckoning
    void adoptKept(const KeepResult &result);
    /// tracks of playlists that are still files elsewhere (from before
    /// playlists held copies) are copied in, each in its place
    void adoptOutsideTracks();
    /// lets go of copies no playlist holds
    void collectCopies();
    /// at launch, before anything is copied: folders in the store nobody knows
    void sweepStore();
    void setCopying(int delta);
    bool isKeptCopy(const QString &path) const;
    QStringList scanSources() const;

    void loadCache();
    void setSources(const QStringList &sources);
    void setScanning(bool scanning);
    /// false when the list is there but could not be read: then nothing of
    /// it or the store is written over or let go
    bool loadPlaylists();
    void savePlaylists() const;
    /// the playlists as they show now, after a change to them or their files
    void publishPlaylists();
    using SongIndex = QHash<QString, const Song *>;
    /// the library's songs by uri, to look a playlist's files up in
    SongIndex indexSongs() const;
    QList<Song> resolve(const StoredPlaylist &stored, const SongIndex &index) const;
    Playlist describe(const StoredPlaylist &stored, const SongIndex &index) const;
    StoredPlaylist *findPlaylist(const QString &id);
    const StoredPlaylist *findPlaylist(const QString &id) const;
    /// writes a cover under a name of its own, so nothing cached shows the
    /// old one, and lets the old one go
    QString writeCover(const StoredPlaylist &stored, const QByteArray &jpeg) const;

    Settings *m_settings;
    QString m_cachePath;
    QString m_artworkDirectory;
    QStringList m_sources;
    SongListModel *m_songs;
    QHash<QString, Entry> m_entries;
    bool m_scanning = false;
    QString m_playlistsPath;
    bool m_playlistsReadable = true;
    QString m_coverDirectory;
    /// where the playlists' copies live, each in a folder of its own
    QString m_trackStore;
    /// the copies, by their path
    QHash<QString, Origin> m_copies;
    /// the copies' songs, by uri
    QHash<QString, Song> m_storeSongs;
    int m_copying = 0;
    /// copies asked for and not yet begun, in order
    QList<std::pair<QString, QStringList>> m_keepQueue;
    bool m_keeping = false;
    QList<StoredPlaylist> m_storedPlaylists;
    PlaylistListModel *m_playlists;
    /// a search started after another one makes that one's result stale
    quint64 m_scanGeneration = 0;
    bool m_rescanQueued = false;
};

} // namespace swiftify
