#include "FakeCore.h"

#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "library/LibraryStore.h"
#include "library/LocalLibrary.h"
#include "playback/Lyrics.h"
#include "playback/PlaybackController.h"
#include "spotify/CredentialStore.h"
#include "spotify/SpotifySession.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <utility>

using namespace swiftify;

namespace {

/// half a second of silence as a 16-bit wav, its tags in a LIST INFO chunk
/// ahead of the data, where readers look for them
void writeTaggedWav(const QString &path, const QList<std::pair<QByteArray, QByteArray>> &tags)
{
    const auto le32 = [](quint32 value) {
        QByteArray bytes(4, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    const auto le16 = [](quint16 value) {
        QByteArray bytes(2, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    QByteArray info = "INFO";
    for (const auto &[id, text] : tags) {
        QByteArray value = text + '\0';
        if (value.size() % 2) {
            value += '\0';
        }
        info += id + le32(quint32(value.size())) + value;
    }
    const quint32 frames = 22'050;
    const QByteArray format = "fmt " + le32(16) + le16(1) + le16(2) + le32(44'100) + le32(44'100 * 4) + le16(4) + le16(16);
    const QByteArray list = "LIST" + le32(quint32(info.size())) + info;
    const QByteArray data = "data" + le32(frames * 4) + QByteArray(frames * 4, '\0');
    const QByteArray body = "WAVE" + format + list + data;
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("RIFF" + le32(quint32(body.size())) + body);
}

/// three tagged songs in a folder of their own, searched by `library`
QStringList addThreeSongs(LocalLibrary &library, const QTemporaryDir &music)
{
    QStringList uris;
    const char *names[] = {"Avalon", "Bluebird", "Comet"};
    for (int index = 0; index < 3; ++index) {
        const QString path = music.filePath(QStringLiteral("%1.wav").arg(index + 1));
        writeTaggedWav(path, {{"INAM", names[index]}, {"IART", "Someone"}, {"IPRD", "Album"},
                              {"IPRT", QByteArray::number(index + 1)}});
        uris.append(QUrl::fromLocalFile(QFileInfo(path).canonicalFilePath()).toString());
    }
    QSignalSpy scanned(&library, &LocalLibrary::scanned);
    library.addSources({QUrl::fromLocalFile(music.path())});
    if (!scanned.wait(5000)) {
        return {};
    }
    return uris;
}

Song songWithUri(const LocalLibrary &library, const QString &uri)
{
    for (const Song &song : library.songs()->songs()) {
        if (song.uri == uri) {
            return song;
        }
    }
    return {};
}

QStringList namesOf(const QList<Song> &songs)
{
    QStringList names;
    for (const Song &song : songs) {
        names.append(song.name);
    }
    return names;
}

} // namespace

/// music files of one's own: found in the folders added, named by their own
/// tags (or their file when they have none), played from disk, with lyrics
/// from a .lrc beside them
class LocalTest : public QObject {
    Q_OBJECT

private slots:
    void aFolderIsSearchedAndEachSongTakesItsOwnTags()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        QDir(music.path()).mkpath(QStringLiteral("Diamond Life"));
        writeTaggedWav(music.filePath(QStringLiteral("Diamond Life/02 b.wav")),
                       {{"INAM", "Your Love Is King"}, {"IART", "Sade"}, {"IPRD", "Diamond Life"}, {"IPRT", "2"}});
        writeTaggedWav(music.filePath(QStringLiteral("Diamond Life/01 a.wav")),
                       {{"INAM", "Smooth Operator"}, {"IART", "Sade"}, {"IPRD", "Diamond Life"}, {"IPRT", "1"}});
        // no tags at all: named after its file, its album after its folder
        writeTaggedWav(music.filePath(QStringLiteral("Diamond Life/hidden track.wav")), {});
        // not music, and not looked at
        QFile notes(music.filePath(QStringLiteral("Diamond Life/notes.txt")));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write("liner notes");
        notes.close();

        Settings settings(storage.filePath(QStringLiteral("swiftify.conf")));
        LocalLibrary library(&settings, nullptr, storage.path());
        QSignalSpy scanned(&library, &LocalLibrary::scanned);
        QCOMPARE(library.addSources({QUrl::fromLocalFile(music.path())}), 1);
        QVERIFY(scanned.wait(5000));
        QCOMPARE(library.songs()->count(), 3);

        // by artist and album, in the album's own order
        const QList<Song> songs = library.songs()->songs();
        const auto byName = [&](const QString &name) {
            return *std::find_if(songs.begin(), songs.end(), [&](const Song &song) { return song.name == name; });
        };
        QCOMPARE(songs.at(0).name, QStringLiteral("Smooth Operator"));
        QCOMPARE(songs.at(1).name, QStringLiteral("Your Love Is King"));
        QCOMPARE(songs.at(0).artists, QStringLiteral("Sade"));
        QCOMPARE(songs.at(0).albumName, QStringLiteral("Diamond Life"));
        QVERIFY(songs.at(0).isLocalFile());
        QVERIFY(std::abs(songs.at(0).durationMs - 500) <= 10);
        QCOMPARE(songs.at(0).localPath(), QFileInfo(music.filePath(QStringLiteral("Diamond Life/01 a.wav"))).canonicalFilePath());
        const Song untagged = byName(QStringLiteral("hidden track"));
        QCOMPARE(untagged.artists, QStringLiteral("Unknown Artist"));
        QCOMPARE(untagged.albumName, QStringLiteral("Diamond Life"));

        // the same folder again is not added twice, nor a file inside it
        QCOMPARE(library.addSources({QUrl::fromLocalFile(music.path())}), 0);
        QCOMPARE(library.addSources({QUrl::fromLocalFile(music.filePath(QStringLiteral("Diamond Life/01 a.wav")))}), 0);

        // what the tags said is kept: a second library shows it at once
        LocalLibrary again(&settings, nullptr, storage.path());
        QCOMPARE(again.songs()->count(), 3);
        QVERIFY(again.isScanning());
        QSignalSpy againScanned(&again, &LocalLibrary::scanned);
        QVERIFY(againScanned.wait(5000));
        QCOMPARE(again.songs()->count(), 3);

        // and removing the folder takes its songs away
        library.removeSource(library.sources().first());
        QCOMPARE(library.songs()->count(), 0);
        QVERIFY(library.sources().isEmpty());
    }

    void playlistsHoldCopiesOfTheirOwn()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        QTemporaryDir elsewhere;
        Settings settings(storage.filePath(QStringLiteral("swiftify.conf")));
        LocalLibrary library(&settings, nullptr, storage.path());
        const QStringList uris = addThreeSongs(library, music);
        QCOMPARE(uris.size(), 3);
        QSignalSpy changed(&library, &LocalLibrary::playlistsChanged);
        QSignalSpy added(&library, &LocalLibrary::tracksAdded);

        // a spotify song cannot go in, only files, and those once copied
        Song fromSpotify;
        fromSpotify.uri = QStringLiteral("spotify:track:4uLU6hMCjMI75M1A2tKUQC");
        const Playlist made = library.createPlaylist(QStringLiteral("  Night Drive "), QStringLiteral("for the road"),
                                                     QByteArray("a cover"), {songWithUri(library, uris[0]), fromSpotify});
        QVERIFY(made.isValid());
        QVERIFY(made.isLocal());
        QCOMPARE(made.name, QStringLiteral("Night Drive"));
        QCOMPARE(made.ownerDisplayName(), QStringLiteral("Local Files"));
        QVERIFY(QFileInfo::exists(made.artworkUrl.toLocalFile()));
        QCOMPARE(library.playlists()->count(), 1);
        QCOMPARE(library.playlistChoices().size(), 1);
        QVERIFY(changed.count() > 0);
        QVERIFY(!library.createPlaylist(QStringLiteral("  "), {}, {}, {}).isValid());
        QVERIFY(added.wait(5000));
        QCOMPARE(library.playlist(made.id).songCount, 1);

        // a copy of its own, kept with the app, not the file it came from
        const Song first = library.songsOf(made.id).value(0);
        QCOMPARE(first.name, QStringLiteral("Avalon"));
        QVERIFY(first.uri != uris[0]);
        QVERIFY(first.localPath().startsWith(QDir(storage.path()).canonicalPath())
                || first.localPath().startsWith(storage.path()));
        QVERIFY(QFileInfo::exists(first.localPath()));

        // files from anywhere, and a folder's, in the order asked for; what
        // is not music is said so
        writeTaggedWav(elsewhere.filePath(QStringLiteral("Dawn.wav")), {{"INAM", "Dawn"}});
        QVERIFY(QDir(elsewhere.path()).mkpath(QStringLiteral("Album")));
        writeTaggedWav(elsewhere.filePath(QStringLiteral("Album/10.wav")), {{"INAM", "Foxtrot"}});
        writeTaggedWav(elsewhere.filePath(QStringLiteral("Album/2.wav")), {{"INAM", "Echo"}});
        QFile lyrics(elsewhere.filePath(QStringLiteral("Dawn.lrc")));
        QVERIFY(lyrics.open(QIODevice::WriteOnly));
        lyrics.write("[00:01.00]morning\n");
        lyrics.close();
        QFile notes(elsewhere.filePath(QStringLiteral("notes.txt")));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.close();
        added.clear();
        library.addFiles(made.id, {QUrl::fromLocalFile(elsewhere.filePath(QStringLiteral("Dawn.wav"))),
                                   QUrl::fromLocalFile(elsewhere.filePath(QStringLiteral("Album"))),
                                   QUrl::fromLocalFile(elsewhere.filePath(QStringLiteral("notes.txt")))});
        QVERIFY(added.wait(5000));
        QCOMPARE(added.first().at(1).toInt(), 3);
        QVERIFY(added.first().at(2).toString().contains(QStringLiteral("notes.txt")));
        QCOMPARE(namesOf(library.songsOf(made.id)), (QStringList {"Avalon", "Dawn", "Echo", "Foxtrot"}));
        // its lyrics went with it
        const Song dawn = library.songsOf(made.id).value(1);
        QVERIFY(QFileInfo::exists(QFileInfo(dawn.localPath()).dir().filePath(QStringLiteral("Dawn.lrc"))));

        // the same file again is not copied again, and two asked for at once
        // land in the order asked
        added.clear();
        QCOMPARE(library.addSongs(made.id, {songWithUri(library, uris[0])}), 1);
        QCOMPARE(library.addSongs(made.id, {songWithUri(library, uris[1])}), 1);
        QTRY_COMPARE(added.count(), 2);
        QList<Song> songs = library.songsOf(made.id);
        QCOMPARE(namesOf(songs), (QStringList {"Avalon", "Dawn", "Echo", "Foxtrot", "Avalon", "Bluebird"}));
        QCOMPARE(songs[0].uri, songs[4].uri);
        // each its own occurrence, as a spotify playlist's songs are
        QVERIFY(songs[0].instanceId != songs[4].instanceId);

        // the originals moved and deleted: the playlist is whole
        QVERIFY(QFile::remove(elsewhere.filePath(QStringLiteral("Dawn.wav"))));
        QVERIFY(QFile::rename(QUrl(uris[0]).toLocalFile(), elsewhere.filePath(QStringLiteral("moved.wav"))));
        QSignalSpy scanned(&library, &LocalLibrary::scanned);
        library.rescan();
        QVERIFY(scanned.wait(5000));
        QCOMPARE(namesOf(library.songsOf(made.id)),
                 (QStringList {"Avalon", "Dawn", "Echo", "Foxtrot", "Avalon", "Bluebird"}));

        QStringList held;
        for (const Song &song : library.songsOf(made.id)) {
            held.append(song.uri);
        }
        library.reorder(made.id, {held[3], held[0], held[1], held[2], held[4], held[5]});
        QCOMPARE(namesOf(library.songsOf(made.id)),
                 (QStringList {"Foxtrot", "Avalon", "Dawn", "Echo", "Avalon", "Bluebird"}));
        // both of a copy go, and the copy with them: nothing holds it now
        QCOMPARE(library.removeSong(made.id, held[0]), 2);
        QCOMPARE(namesOf(library.songsOf(made.id)), (QStringList {"Foxtrot", "Dawn", "Echo", "Bluebird"}));
        QVERIFY(!QFileInfo::exists(QUrl(held[0]).toLocalFile()));

        // kept: another library made from the same place has it at once
        {
            LocalLibrary again(&settings, nullptr, storage.path());
            QCOMPARE(again.playlists()->count(), 1);
            QCOMPARE(namesOf(again.songsOf(made.id)), (QStringList {"Foxtrot", "Dawn", "Echo", "Bluebird"}));
            QCOMPARE(again.playlist(made.id).description, QStringLiteral("for the road"));
            QSignalSpy againScanned(&again, &LocalLibrary::scanned);
            QVERIFY(againScanned.wait(5000));
            QCOMPARE(namesOf(again.songsOf(made.id)), (QStringList {"Foxtrot", "Dawn", "Echo", "Bluebird"}));
        }

        // a new cover takes the old one's place, under a new name
        const QString oldCover = library.playlist(made.id).artworkUrl.toLocalFile();
        QVERIFY(library.updatePlaylist(made.id, QStringLiteral("Late Drive"), {}, QByteArray("another cover")));
        const Playlist updated = library.playlist(made.id);
        QCOMPARE(updated.name, QStringLiteral("Late Drive"));
        QVERIFY(updated.description.isEmpty());
        QVERIFY(updated.artworkUrl.toLocalFile() != oldCover);
        QVERIFY(!QFileInfo::exists(oldCover));
        QVERIFY(QFileInfo::exists(updated.artworkUrl.toLocalFile()));
        // and no cover leaves it alone
        QVERIFY(library.updatePlaylist(made.id, QStringLiteral("Late Drive"), {}, {}));
        QCOMPARE(library.playlist(made.id).artworkUrl, updated.artworkUrl);

        // deleted, its copies go with it
        const QString copy = library.songsOf(made.id).value(0).localPath();
        QVERIFY(QFileInfo::exists(copy));
        QVERIFY(library.deletePlaylist(made.id));
        QCOMPARE(library.playlists()->count(), 0);
        QVERIFY(!QFileInfo::exists(updated.artworkUrl.toLocalFile()));
        QVERIFY(!QFileInfo::exists(copy));
        LocalLibrary afterwards(&settings, nullptr, storage.path());
        QCOMPARE(afterwards.playlists()->count(), 0);
        QSignalSpy afterwardsScanned(&afterwards, &LocalLibrary::scanned);
        QVERIFY(afterwardsScanned.wait(5000));
    }

    /// a playlist kept before playlists held copies takes copies of the
    /// files it names, each in its place
    void playlistsFromBeforeTakeCopiesOfTheirTracks()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swiftify.conf")));
        QStringList uris;
        {
            LocalLibrary library(&settings, nullptr, storage.path());
            uris = addThreeSongs(library, music);
        }
        QCOMPARE(uris.size(), 3);
        QFile kept(storage.filePath(QStringLiteral("local-playlists.json")));
        QVERIFY(kept.open(QIODevice::WriteOnly));
        kept.write(QJsonDocument(QJsonObject {
                                     {QStringLiteral("version"), 1},
                                     {QStringLiteral("playlists"),
                                      QJsonArray {QJsonObject {{QStringLiteral("id"), QStringLiteral("local:old")},
                                                               {QStringLiteral("name"), QStringLiteral("Old")},
                                                               {QStringLiteral("songs"),
                                                                QJsonArray {uris[2], uris[0]}}}}},
                                 })
                       .toJson());
        kept.close();

        LocalLibrary library(&settings, nullptr, storage.path());
        QTRY_VERIFY(!library.isCopying() && library.songsOf(QStringLiteral("local:old")).size() == 2
                    && !library.songsOf(QStringLiteral("local:old")).value(0).localPath().startsWith(music.path()));
        const QList<Song> songs = library.songsOf(QStringLiteral("local:old"));
        QCOMPARE(namesOf(songs), (QStringList {"Comet", "Avalon"}));
        for (const Song &song : songs) {
            QVERIFY(!uris.contains(song.uri));
        }
        // the search at launch is done before the folders go
        QTRY_VERIFY(!library.isScanning());
    }

    void aListThatCannotBeReadLosesNothing_data()
    {
        QTest::addColumn<QByteArray>("unreadable");
        QTest::newRow("corrupt") << QByteArray("{\"version\": 1, \"playlists\": [");
        QTest::newRow("newer") << QJsonDocument(QJsonObject {{QStringLiteral("version"), 2},
                                                             {QStringLiteral("playlists"), QJsonArray {}}})
                                      .toJson();
    }

    /// a list broken, or kept by a newer app, is left as it is, and the
    /// copies it names with it, until it can be read again
    void aListThatCannotBeReadLosesNothing()
    {
        QFETCH(QByteArray, unreadable);
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swiftify.conf")));
        const QString listPath = storage.filePath(QStringLiteral("local-playlists.json"));
        QString id;
        QString copy;
        {
            LocalLibrary library(&settings, nullptr, storage.path());
            const QStringList uris = addThreeSongs(library, music);
            QCOMPARE(uris.size(), 3);
            QSignalSpy added(&library, &LocalLibrary::tracksAdded);
            id = library.createPlaylist(QStringLiteral("Kept"), {}, {}, {songWithUri(library, uris[0])}).id;
            QVERIFY(added.wait(5000));
            copy = library.songsOf(id).value(0).localPath();
            QTRY_VERIFY(!library.isCopying() && !library.isScanning());
        }
        QVERIFY(QFileInfo::exists(copy));
        QFile list(listPath);
        QVERIFY(list.open(QIODevice::ReadOnly));
        const QByteArray readable = list.readAll();
        list.close();
        QVERIFY(list.open(QIODevice::WriteOnly | QIODevice::Truncate));
        list.write(unreadable);
        list.close();
        const auto listNow = [&] {
            QFile file(listPath);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };

        {
            LocalLibrary library(&settings, nullptr, storage.path());
            QCOMPARE(library.playlists()->count(), 0);
            QTRY_VERIFY(!library.isScanning());
            // the copy stays, and the list is not written over, not even by
            // a playlist made now
            QVERIFY(QFileInfo::exists(copy));
            QVERIFY(library.createPlaylist(QStringLiteral("Meanwhile"), {}, {}, {}).isValid());
            QCOMPARE(listNow(), unreadable);
        }
        QVERIFY(QFileInfo::exists(copy));
        QCOMPARE(listNow(), unreadable);
        // and kept aside besides
        QFile aside(listPath + QStringLiteral(".unreadable"));
        QVERIFY(aside.open(QIODevice::ReadOnly));
        QCOMPARE(aside.readAll(), unreadable);

        // readable again, the playlist is whole
        QVERIFY(list.open(QIODevice::WriteOnly | QIODevice::Truncate));
        list.write(readable);
        list.close();
        LocalLibrary library(&settings, nullptr, storage.path());
        QCOMPARE(library.playlists()->count(), 1);
        QTRY_COMPARE(library.songsOf(id).value(0).localPath(), copy);
        QTRY_VERIFY(!library.isScanning());
        QVERIFY(QFileInfo::exists(copy));
    }

    void aPlaylistMadeHereOpensAndChangesWithoutSpotify()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swiftify.conf")));
        ErrorCenter errors {nullptr};
        MemoryCredentialStore credentials;
        QNetworkAccessManager network;
        testing::FakeCore core;
        SpotifySession session(&core, &credentials, &network, &errors);
        LibraryStore store(&session, &settings, &errors);
        LocalLibrary library(&settings, nullptr, storage.path());
        store.attachLocalFiles(&library);
        const QStringList uris = addThreeSongs(library, music);
        QCOMPARE(uris.size(), 3);

        // made from a song's menu: it opens, holding that song
        QSignalSpy saved(&store, &LibraryStore::playlistSaveFinished);
        store.saveLocalPlaylist(true, QStringLiteral("Mine"), {}, {}, QVariant::fromValue(songWithUri(library, uris[0])));
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.first().first().toBool(), true);
        QCOMPARE(store.page(), LibraryStore::Page::Playlist);
        const Playlist mine = store.selectedPlaylist();
        QVERIFY(mine.isLocal());
        QCOMPARE(mine.name, QStringLiteral("Mine"));
        QVERIFY(store.isSelectedPlaylistEditable());
        QVERIFY(store.canReorderPlaylist());
        QVERIFY(!store.isLoadingSongs());
        // once copied in
        QTRY_COMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Avalon"}));

        // with nobody signed in to spotify
        store.addToPlaylist(songWithUri(library, uris[1]), mine);
        store.addToPlaylist(songWithUri(library, uris[2]), mine);
        QTRY_COMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Avalon", "Bluebird", "Comet"}));
        QCOMPARE(store.selectedPlaylist().songCount, 3);
        Song fromSpotify;
        fromSpotify.uri = QStringLiteral("spotify:track:4uLU6hMCjMI75M1A2tKUQC");
        store.addToPlaylist(fromSpotify, mine);
        QVERIFY(errors.message().contains(QStringLiteral("Only music files")));
        QCOMPARE(store.playlistSongs()->count(), 3);

        // a drag moves the rows where they are, and the order is kept
        const quint64 avalon = store.playlistSongs()->songs().at(0).instanceId;
        store.moveSongs(0, 1, 3);
        QCOMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Bluebird", "Comet", "Avalon"}));
        QCOMPARE(store.playlistSongs()->songs().at(2).instanceId, avalon);
        QCOMPARE(namesOf(library.songsOf(mine.id)), (QStringList {"Bluebird", "Comet", "Avalon"}));

        // the row's own song: the playlist's copy
        store.removeFromSelectedPlaylist(store.playlistSongs()->songs().at(0));
        QCOMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Comet", "Avalon"}));
        // the occurrences left are the ones that were shown
        QCOMPARE(store.playlistSongs()->songs().at(1).instanceId, avalon);

        store.saveLocalPlaylist(false, QStringLiteral("Still Mine"), QStringLiteral("two left"), {}, QVariant());
        QCOMPARE(saved.count(), 2);
        QCOMPARE(store.selectedPlaylist().name, QStringLiteral("Still Mine"));

        // pinned, it shows with the pinned spotify playlists
        store.togglePinned(store.selectedPlaylist());
        QCOMPARE(store.pinnedPlaylists()->count(), 1);

        // away and back again
        store.showLocalFiles();
        store.showLocalTracks();
        QCOMPARE(store.page(), LibraryStore::Page::LocalTracks);
        store.goBack();
        QCOMPARE(store.page(), LibraryStore::Page::LocalFiles);
        store.goBack();
        QCOMPARE(store.selectedPlaylistId(), mine.id);
        store.showLocalFiles();
        store.selectPlaylistById(mine.id);
        QCOMPARE(store.selectedPlaylistId(), mine.id);
        QCOMPARE(store.playlistSongs()->count(), 2);

        QSignalSpy deleted(&store, &LibraryStore::playlistDeleted);
        store.deletePlaylist(store.selectedPlaylist());
        QCOMPARE(deleted.count(), 1);
        QCOMPARE(store.page(), LibraryStore::Page::LocalFiles);
        QCOMPARE(store.pinnedPlaylists()->count(), 0);
        QCOMPARE(library.playlists()->count(), 0);
    }

    void aSongsTagsAreEditedInItsFile()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swiftify.conf")));
        LocalLibrary library(&settings, nullptr, storage.path());
        const QStringList uris = addThreeSongs(library, music);
        QCOMPARE(uris.size(), 3);
        QCOMPARE(library.tagsOf(uris[0]).value(QStringLiteral("title")).toString(), QStringLiteral("Avalon"));
        QCOMPARE(library.tagsOf(uris[0]).value(QStringLiteral("trackNumber")).toString(), QStringLiteral("1"));

        // queued to play, as it is now
        ErrorCenter errors {nullptr};
        MemoryCredentialStore credentials;
        QNetworkAccessManager network;
        testing::FakeCore core;
        SpotifySession session(&core, &credentials, &network, &errors);
        PlaybackController player(&core, &session, &settings, &errors);
        const Song queued = songWithUri(library, uris[0]);
        player.play(queued, {queued});
        for (int pass = 0; pass < 5; ++pass) {
            QCoreApplication::processEvents();
        }

        QSignalSpy saved(&library, &LocalLibrary::tagsSaved);
        QSignalSpy scanned(&library, &LocalLibrary::scanned);
        const QByteArray cover("\xff\xd8\xff\xe0 a cover", 14);
        library.editTags(uris[0],
                         {{QStringLiteral("title"), QStringLiteral("Avalon (Live)")},
                          {QStringLiteral("artist"), QStringLiteral("Roxy Music")},
                          {QStringLiteral("trackNumber"), QStringLiteral("9")}},
                         cover, false);
        QVERIFY(saved.wait(5000));
        QCOMPARE(saved.first().at(1).toBool(), true);
        QVERIFY(scanned.wait(5000));
        const Song edited = songWithUri(library, uris[0]);
        QCOMPARE(edited.name, QStringLiteral("Avalon (Live)"));
        QCOMPARE(edited.artists, QStringLiteral("Roxy Music"));
        QVERIFY(!edited.artworkUrl.isEmpty());
        QCOMPARE(library.tagsOf(uris[0]).value(QStringLiteral("trackNumber")).toString(), QStringLiteral("9"));
        // the album was left as it was
        QCOMPARE(edited.albumName, QStringLiteral("Album"));

        // the copy playing takes it on, still the same occurrence
        player.refreshSongDetails(library.songs()->songs());
        QCOMPARE(player.currentSong().name, QStringLiteral("Avalon (Live)"));
        QCOMPARE(player.currentSong().instanceId, queued.instanceId);

        // and the cover can go again
        saved.clear();
        library.editTags(uris[0], {}, {}, true);
        QVERIFY(saved.wait(5000));
        QVERIFY(scanned.wait(5000));
        QVERIFY(songWithUri(library, uris[0]).artworkUrl.isEmpty());

        // a file the library does not hold is not touched
        saved.clear();
        library.editTags(QStringLiteral("file:///nowhere/at/all.mp3"), {{QStringLiteral("title"), QStringLiteral("x")}}, {},
                         false);
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.first().at(1).toBool(), false);
    }

    void lrcLinesAreTimedAndSorted()
    {
        const auto lyrics = Lyrics::fromLrc(QStringLiteral("[ar:Sade]\n[ti:Smooth Operator]\n"
                                                           "[00:21.50]Diamond life\n"
                                                           "[00:10.00][00:30.25]Lover boy\n"
                                                           "[00:25]Ooh\n"));
        QVERIFY(lyrics);
        QVERIFY(lyrics->synchronized);
        QCOMPARE(lyrics->lines.size(), 4);
        QCOMPARE(lyrics->lines.at(0).startTimeMs, 10'000u);
        QCOMPARE(lyrics->lines.at(0).words, QStringLiteral("Lover boy"));
        QCOMPARE(lyrics->lines.at(1).startTimeMs, 21'500u);
        QCOMPARE(lyrics->lines.at(1).endTimeMs, 25'000u);
        QCOMPARE(lyrics->lines.at(3).startTimeMs, 30'250u);
        QCOMPARE(lyrics->lineIndex(22'000), 1);

        // spotify's lines carry no end, so each runs to the next one's start
        const auto spotify = Lyrics::fromJson(R"({"syncType":"lineSynced","lines":[
            {"startTimeMs":1000,"endTimeMs":0,"words":"one"},
            {"startTimeMs":4000,"endTimeMs":0,"words":"two"}]})");
        QVERIFY(spotify);
        QCOMPARE(spotify->lines.at(0).endTimeMs, 4'000u);
        QCOMPARE(spotify->lines.at(1).endTimeMs, 9'000u);

        // without times, the lines are there to read along
        const auto plain = Lyrics::fromLrc(QStringLiteral("first line\nsecond line\n"));
        QVERIFY(plain);
        QVERIFY(!plain->synchronized);
        QCOMPARE(plain->lines.size(), 2);
        QVERIFY(!Lyrics::fromLrc(QStringLiteral("[ar:nobody]\n")));
    }

    void aFileOfOnesOwnPlaysFromDiskWithItsLyrics()
    {
        QTemporaryDir directory;
        Settings settings(directory.filePath(QStringLiteral("swiftify.conf")));
        ErrorCenter errors {nullptr};
        MemoryCredentialStore credentials;
        QNetworkAccessManager network;
        testing::FakeCore core;
        SpotifySession session(&core, &credentials, &network, &errors);
        PlaybackController player(&core, &session, &settings, &errors);

        // brackets in a name, as "[HQ]" so often is, are no pattern
        const QString path = directory.filePath(QStringLiteral("Injury [HQ].mp3"));
        QFile lrc(directory.filePath(QStringLiteral("Injury [HQ].LRC")));
        QVERIFY(lrc.open(QIODevice::WriteOnly));
        lrc.write("[00:01.00]one\n[00:02.00]two\n");
        lrc.close();

        Song song;
        song.name = QStringLiteral("song");
        song.uri = QUrl::fromLocalFile(path).toString();
        player.play(song, {song});
        for (int pass = 0; pass < 5; ++pass) {
            QCoreApplication::processEvents();
        }
        // straight from disk: no stream, no spotify session asked for
        const auto call = core.take(QStringLiteral("playFile"));
        QVERIFY(call);
        QCOMPARE(call->argument, path);
        QVERIFY(!core.take(QStringLiteral("playTrack")));
        call->succeed();
        for (int pass = 0; pass < 5; ++pass) {
            QCoreApplication::processEvents();
        }
        QVERIFY(player.isPlaying());
        QVERIFY(player.hasLyrics());
        QVERIFY(player.lyricsSynchronized());
        QCOMPARE(player.lyrics()->count(), 2);
    }
};

QTEST_MAIN(LocalTest)
#include "tst_local.moc"
