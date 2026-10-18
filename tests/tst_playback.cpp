#include "FakeCore.h"

#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "playback/InteractionState.h"
#include "playback/PlaybackController.h"
#include "playback/PlaybackState.h"
#include "spotify/CredentialStore.h"
#include "spotify/SpotifySession.h"
#if defined(SWIFTIFY_HAS_DBUS) && defined(Q_OS_LINUX)
#include "system/dbus/Mpris.h"
#endif

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace swiftify;
using swiftify::testing::FakeCore;

namespace {

Song makeSong(const QString &name)
{
    Song song;
    song.name = name;
    song.artists = QStringLiteral("Artist");
    // a music file of one's own: it plays from disk, with no network, which
    // none of these rules depend on
    song.uri = QUrl::fromLocalFile(QStringLiteral("/music/") + name + QStringLiteral(".flac")).toString();
    song.durationMs = 200'000;
    return song;
}

QList<Song> makeQueue(int count)
{
    QList<Song> songs;
    for (int index = 0; index < count; ++index) {
        songs.append(makeSong(QStringLiteral("song%1").arg(index)));
    }
    return songs;
}

} // namespace

/// the player's rules, Appendix C of the plan: nothing but a person, or a queue
/// that is already playing, may start sound.
class PlaybackTest : public QObject {
    Q_OBJECT

private:
    struct Rig {
        QTemporaryDir directory;
        Settings settings {directory.filePath(QStringLiteral("swiftify.conf"))};
        ErrorCenter errors {nullptr};
        MemoryCredentialStore credentials;
        QNetworkAccessManager network;
        FakeCore core;
        SpotifySession session {&core, &credentials, &network, &errors};
        PlaybackController player {&core, &session, &settings, &errors};

        explicit Rig(const QString &path = {})
            : settings(path.isEmpty() ? directory.filePath(QStringLiteral("swiftify.conf")) : path)
        {
        }

        /// lets the song the player asked for finish loading
        void finishLoading()
        {
            settle();
            const auto call = core.take(QStringLiteral("playFile"));
            QVERIFY(call);
            call->succeed();
            settle();
        }

        /// qcoro resumes a coroutine waiting on a signal from the event loop
        static void settle()
        {
            for (int pass = 0; pass < 5; ++pass) {
                QCoreApplication::processEvents();
            }
        }
    };

private slots:
    void lastTrackQueueAndPositionSurviveRestartWithoutStartingSound()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("settings.ini"));
        auto songs = makeQueue(3);
        songs[1].uri = songs[0].uri; // same URI, different occurrence and name
        {
            Rig rig(path);
            rig.player.play(songs[1], songs);
            rig.finishLoading();
            rig.core.duration = 200000;
            rig.player.seek(42000);
            QTRY_VERIFY(rig.settings.contains(QStringLiteral("lastPlaybackState")));
            QTRY_COMPARE(playbackstate::decode(rig.settings.value(QStringLiteral("lastPlaybackState")).toByteArray())->positionMs, 42000u);
            QVERIFY(rig.player.isPlaying());
        }
        {
            Rig rig(path);
            QVERIFY(rig.player.hasSong());
            QVERIFY(!rig.player.isPlaying());
            QVERIFY(!rig.player.isLoading());
            QVERIFY(!rig.core.sounding);
            QVERIFY(rig.core.pending.isEmpty());
            QCOMPARE(rig.player.currentSong().name, songs[1].name);
            QCOMPARE(rig.player.positionMs(), 42000);
            QCOMPARE(rig.player.durationMs(), 200000);
            QCOMPARE(rig.player.currentQueueIndex(), 1);
            QCOMPARE(rig.player.activeQueue().size(), 3);
            QCOMPARE(rig.player.upNextSongs().first().uri, songs[2].uri);
            // Seeking a restored track changes the parked position, not the
            // empty audio core. The seek is applied only after explicit Play.
            rig.player.seek(60000);
            QCOMPARE(rig.player.positionMs(), 60000);
            QCOMPARE(rig.core.position, 0u);
            QVERIFY(rig.core.pending.isEmpty());
            rig.player.resume();
            rig.finishLoading();
            QVERIFY(rig.player.isPlaying());
            QCOMPARE(rig.core.position, 60000u);
            QVERIFY(rig.core.log.contains(QStringLiteral("seek 60000")));
        }
    }

    void restoredQueueRetainsShuffleOrderAndRepeat()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("settings.ini"));
        QStringList shuffled;
        QString currentName;
        {
            Rig rig(path);
            const auto songs = makeQueue(8);
            rig.player.play(songs[3], songs);
            rig.finishLoading();
            rig.player.toggleShuffle();
            rig.player.cycleRepeatMode();
            rig.player.skipForward();
            rig.finishLoading();
            currentName = rig.player.currentSong().name;
            for (const auto &song : rig.player.activeQueue()) shuffled.append(song.uri);
        }
        {
            Rig rig(path);
            QVERIFY(!rig.player.isPlaying());
            QVERIFY(rig.player.isShuffleEnabled());
            QCOMPARE(rig.player.repeatMode(), PlaybackController::RepeatMode::All);
            QStringList restored;
            for (const auto &song : rig.player.activeQueue()) restored.append(song.uri);
            QCOMPARE(restored, shuffled);
            QVERIFY(rig.player.precedingSong());
            QVERIFY(rig.player.activeQueue().contains(*rig.player.precedingSong()));
            rig.player.toggleShuffle();
            QCOMPARE(rig.player.activeQueue().at(0).name, QStringLiteral("song0"));
            QCOMPARE(rig.player.currentSong().name, currentName);
        }
    }

    void pauseStillWinsOverRestoredPlaybackAndLogoutForgetsIt()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("settings.ini"));
        {
            Rig rig(path);
            const auto songs = makeQueue(2);
            rig.player.play(songs[0], songs);
            rig.finishLoading();
            rig.player.seek(30000);
        }
        {
            Rig rig(path);
            rig.player.resume();
            rig.player.pause();
            rig.finishLoading();
            QVERIFY(!rig.player.isPlaying());
            QVERIFY(!rig.core.sounding);
            rig.player.resume();
            rig.finishLoading();
            QCOMPARE(rig.core.position, 30000u);
            QVERIFY(QMetaObject::invokeMethod(&rig.session, "loggedOut"));
            QVERIFY(!rig.player.hasSong());
            QVERIFY(!rig.settings.contains(QStringLiteral("lastPlaybackState")));
        }
        Rig next(path);
        QVERIFY(!next.player.hasSong());
        QVERIFY(next.core.pending.isEmpty());
    }

    void playbackSnapshotsRejectCorruptionAndKeepTheCurrentTrackInLargeQueues()
    {
        QVERIFY(!playbackstate::decode(QByteArrayLiteral("bad JSON")));
        QVERIFY(!playbackstate::decode(QByteArrayLiteral("{\"version\":99}")));
        const auto songs = makeQueue(2600);
        const auto bytes = playbackstate::encode(songs, {}, {}, songs[2400], 900000, 200000, false, 0);
        const auto snapshot = playbackstate::decode(bytes);
        QVERIFY(snapshot);
        QVERIFY(snapshot->queue.size() <= playbackstate::maximumQueue);
        QCOMPARE(snapshot->queue.at(snapshot->currentIndex).name, songs[2400].name);
        QCOMPARE(snapshot->positionMs, 200000u);
        auto object = QJsonDocument::fromJson(bytes).object();
        object.insert(QStringLiteral("currentIndex"), 9000);
        QVERIFY(!playbackstate::decode(QJsonDocument(object).toJson()));
        object.insert(QStringLiteral("currentIndex"), snapshot->currentIndex);
        object.insert(QStringLiteral("shuffledOrder"), QJsonArray {0, 0});
        QVERIFY(!playbackstate::decode(QJsonDocument(object).toJson()));
        Song debug;
        debug.uri = QString::fromLatin1(PlaybackController::kLoadingTestUri);
        QVERIFY(playbackstate::encode({debug}, {}, {}, debug, 0, 0, false, 0).isEmpty());
    }

    void playingASongLoadsItAndStartsIt()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(3);
        rig.player.play(queue[1], queue);
        QVERIFY(rig.player.isLoading());
        QVERIFY(!rig.player.isPlaying());
        rig.finishLoading();
        QVERIFY(rig.player.isPlaying());
        QVERIFY(!rig.player.isLoading());
        QVERIFY(rig.core.sounding);
        QCOMPARE(rig.session.status(), QStringLiteral("Playing"));
        QCOMPARE(rig.player.upNextSongs().size(), 1);
        QCOMPARE(rig.player.previouslyPlayedSongs().size(), 1);
    }

    void aSongStartedElsewhereIsShownAndSkipsGoThere()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(4);
        rig.player.play(queue[0], queue);
        rig.finishLoading();

        // picked in spotify on a phone, playing through the connect device
        Song picked;
        picked.name = QStringLiteral("Picked elsewhere");
        picked.uri = QStringLiteral("spotify:track:elsewhere");
        picked.durationMs = 180'000;
        rig.core.remote = true;
        emit rig.core.remoteTrackStarted(picked);
        QCOMPARE(rig.player.currentSong().uri, picked.uri);
        QCOMPARE(rig.player.durationMs(), 180'000);
        QVERIFY(rig.player.isPlaying());
        QVERIFY(!rig.player.isLoading());
        // what follows is up to whoever picked it
        QCOMPARE(rig.player.upNext()->count(), 0);
        QVERIFY(rig.player.canSkipForward());

        rig.player.skipForward();
        rig.player.skipToPreviousSong();
        QCOMPARE(rig.core.log.mid(rig.core.log.size() - 2),
                 (QStringList{QStringLiteral("skip next"), QStringLiteral("skip previous")}));
        QCOMPARE(rig.player.currentSong().uri, picked.uri);

        emit rig.core.remotePlayingChanged(false);
        QVERIFY(!rig.player.isPlaying());
        QCOMPARE(rig.session.status(), QStringLiteral("Paused"));
        emit rig.core.remotePlayingChanged(true);
        QVERIFY(rig.player.isPlaying());

        // playing something here takes over again
        rig.player.play(queue[1], queue);
        rig.finishLoading();
        const qsizetype logged = rig.core.log.size();
        rig.player.skipForward();
        rig.finishLoading();
        QCOMPARE(rig.player.currentSong().uri, queue[2].uri);
        QVERIFY(!rig.core.log.mid(logged).contains(QStringLiteral("skip next")));
    }

    void theQueueStepsOnWithoutAReset()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(5);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        QCOMPARE(rig.player.upNext()->count(), 4);

        QSignalSpy resets(rig.player.upNext(), &QAbstractItemModel::modelReset);
        QSignalSpy removals(rig.player.upNext(), &QAbstractItemModel::rowsRemoved);
        rig.player.skipForward();
        rig.finishLoading();
        QCOMPARE(resets.count(), 0);
        QCOMPARE(removals.count(), 1);
        QCOMPARE(removals.first().at(1).toInt(), 0);
        QCOMPARE(rig.player.upNext()->count(), 3);
        QCOMPARE(rig.player.upNext()->at(0).uri, queue[2].uri);
    }

    void pauseWinsOverALoadStillInFlight()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[0], queue);
        rig.player.pause();
        // the core finishes the load it was given and starts the track anyway
        rig.finishLoading();
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.core.sounding);
        // play starts it properly from there
        rig.player.resume();
        rig.finishLoading();
        QVERIFY(rig.player.isPlaying());
    }

    void aFailedLoadLeavesTheCorePaused()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.core.take(QStringLiteral("playFile"))->fail(QStringLiteral("no such track"));
        rig.settle();
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(rig.core.log.contains(QStringLiteral("pause")));
        QCOMPARE(rig.errors.message(), QStringLiteral("no such track"));
    }

    void aReplacedLoadIsAskedForAgainWhileItsSongIsWanted()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[0], queue);
        // something other than a song, a reconnect for a download, replaced
        // the request in the core before it made any sound
        rig.settle();
        rig.core.take(QStringLiteral("playFile"))->fail(QString::fromLatin1(SWIFTIFY_CORE_SUPERSEDED));
        rig.settle();
        QVERIFY(rig.player.isLoading());
        QVERIFY(rig.errors.message().isEmpty());
        // the song is still the one wanted, so it is asked for again
        rig.finishLoading();
        QVERIFY(rig.player.isPlaying());
        QCOMPARE(rig.player.currentSong().uri, queue[0].uri);
    }

    void seekingNeverStartsSound()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        // ignored while the song is still loading
        rig.player.seek(10'000);
        QVERIFY(!rig.core.log.contains(QStringLiteral("seek 10000")));
        rig.finishLoading();
        rig.player.pause();
        rig.core.duration = 200'000;
        rig.player.seek(30'000);
        QVERIFY(rig.core.log.contains(QStringLiteral("seek 30000")));
        QVERIFY(!rig.core.sounding);
        QVERIFY(!rig.player.isPlaying());
    }

    void theEndOfTheQueueStopsAndPlayStartsOver()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 199'500;
        rig.core.endOfTrack = true;
        QTRY_COMPARE_WITH_TIMEOUT(rig.session.status(), QStringLiteral("Finished"), 2000);
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.core.sounding);

        // scrubbing a finished song stays silent
        rig.player.seek(50'000);
        QVERIFY(!rig.core.sounding);
        // play loads it again from the start
        rig.player.resume();
        QVERIFY(rig.core.take(QStringLiteral("playFile")));
    }

    void aStaleEndOfTrackDoesNotSkipMidSong()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 60'000;
        rig.core.endOfTrack = true;
        QTest::qWait(400);
        QVERIFY(rig.player.currentSong().isSameInstance(queue[0]));
        QVERIFY(rig.player.isPlaying());
    }

    void aLostDeviceStopsAndRebuildsOnlyOnPlay()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 42'000;
        rig.core.stalled = true;
        QTRY_VERIFY_WITH_TIMEOUT(!rig.player.isPlaying(), 2000);
        QVERIFY(!rig.core.sounding);
        QVERIFY(rig.core.pending.isEmpty() || !rig.core.take(QStringLiteral("restartAudioOutput")));

        rig.player.resume();
        rig.core.take(QStringLiteral("restartAudioOutput"))->succeed();
        rig.settle();
        rig.finishLoading();
        QVERIFY(rig.core.log.contains(QStringLiteral("seek 42000")));
        QVERIFY(rig.player.isPlaying());
    }

    void sleepPausesAPlayingSong()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.player.prepareForSleep();
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.core.sounding);
    }

    void aSongOutsideTheQueuePlaysAlone()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(3);
        const Song stranger = makeSong(QStringLiteral("song4"));
        rig.player.play(stranger, queue);
        QCOMPARE(rig.player.activeQueue().size(), 1);
    }

    void shufflePutsTheCurrentSongFirstAndClearsHistory()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(5);
        rig.player.play(queue[2], queue);
        rig.finishLoading();
        rig.player.skipForward();
        rig.finishLoading();
        QCOMPARE(rig.player.previouslyPlayedSongs().size(), 3);

        rig.player.toggleShuffle();
        QVERIFY(rig.player.activeQueue().first().isSameInstance(queue[3]));
        QCOMPARE(rig.player.activeQueue().size(), 5);
        QVERIFY(rig.player.previouslyPlayedSongs().isEmpty());
    }

    void playingFromHistoryTrimsIt()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(4);
        rig.player.toggleShuffle();
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.player.skipForward();
        rig.finishLoading();
        rig.player.skipForward();
        rig.finishLoading();
        const QList<Song> history = rig.player.previouslyPlayedSongs();
        QCOMPARE(history.size(), 2);
        rig.player.playFromQueue(history.first());
        QVERIFY(rig.player.previouslyPlayedSongs().isEmpty() || !rig.player.previouslyPlayedSongs().first().isSameInstance(history.first()));
    }

    void playNextAndPlayLastDoNotDuplicate()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(3);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        const Song again = queue[2].freshInstance();
        rig.player.playNext(again);
        QList<Song> upNext = rig.player.upNextSongs();
        QCOMPARE(upNext.size(), 2);
        QVERIFY(upNext.first().isSameInstance(again));
        rig.player.playLast(queue[1].freshInstance());
        upNext = rig.player.upNextSongs();
        QCOMPARE(upNext.size(), 2);
        QCOMPARE(upNext.last().uri, queue[1].uri);
    }

    void backRestartsAfterThreeSeconds()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[1], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 10'000;
        QTRY_COMPARE_WITH_TIMEOUT(rig.player.livePositionMs(), 10'000u, 2000);
        rig.player.skipBackward();
        QVERIFY(rig.core.log.contains(QStringLiteral("seek 0")));
        QVERIFY(rig.player.currentSong().isSameInstance(queue[1]));

        rig.core.position = 1'000;
        rig.player.seek(1'000);
        rig.player.skipBackward();
        QVERIFY(rig.player.currentSong().isSameInstance(queue[0]));
    }

    void muteRemembersTheVolume()
    {
        Rig rig;
        rig.player.setVolume(0.4);
        rig.player.toggleMute();
        QCOMPARE(rig.player.volume(), 0.0);
        QCOMPARE(rig.core.volumeValue, 0.0f);
        rig.player.toggleMute();
        QCOMPARE(rig.player.volume(), 0.4);
        QCOMPARE(rig.settings.value(QStringLiteral("playerVolume")).toDouble(), 0.4);
    }

    void theSleepTimerPausesAndRestoresTheVolume()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.setVolume(0.8);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.player.startSleepTimer(1, QStringLiteral("entire"));
        QVERIFY(rig.player.isSleepTimerRunning());
        QVERIFY(rig.player.isVolumeManagedBySleepTimer());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.player.isSleepTimerRunning(), 3000);
        QVERIFY(!rig.player.isPlaying());
        QCOMPARE(rig.player.volume(), 0.8);
    }

    void cancellingTheSleepTimerRestoresTheVolume()
    {
        Rig rig;
        rig.player.setVolume(0.6);
        rig.player.startSleepTimer(60, QStringLiteral("entire"));
        QTest::qWait(300);
        QVERIFY(rig.player.volume() < 0.6);
        rig.player.cancelSleepTimer();
        QCOMPARE(rig.player.volume(), 0.6);
    }

    void sleepFadeLevels()
    {
        QCOMPARE(sleepFadeLevel(SleepFade::None, 1, 60), 1.0);
        QCOMPARE(sleepFadeLevel(SleepFade::FinalSeconds, 20, 60), 1.0);
        QCOMPARE(sleepFadeLevel(SleepFade::FinalSeconds, 5, 60), 0.5);
        QCOMPARE(sleepFadeLevel(SleepFade::FinalSeconds, 2, 4), 0.5);
        QCOMPARE(sleepFadeLevel(SleepFade::Entire, 30, 60), 0.5);
        QCOMPARE(sleepFadeLevel(SleepFade::Entire, 0, 60), 0.0);
    }

    void lyricsFollowThePosition()
    {
        Rig rig;
        // a file with a .lrc beside it
        Song song = makeSong(QStringLiteral("sung"));
        song.uri = QUrl::fromLocalFile(rig.directory.filePath(QStringLiteral("sung.flac"))).toString();
        QFile lyrics(rig.directory.filePath(QStringLiteral("sung.lrc")));
        QVERIFY(lyrics.open(QIODevice::WriteOnly));
        lyrics.write("[00:01.00]one\n[00:05.00]two\n");
        lyrics.close();
        rig.player.play(song, {song});
        rig.settle();
        QVERIFY(rig.player.hasLyrics());
        QCOMPARE(rig.player.activeLyricIndex(), -1);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 5'500;
        QTRY_COMPARE_WITH_TIMEOUT(rig.player.activeLyricIndex(), 1, 2000);
    }

    void missingLyricsShowThePhrase()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.settle();
        QVERIFY(!rig.player.hasLyrics());
        QCOMPARE(rig.player.lyricsMessage(), QStringLiteral("i mean i guess bro"));
    }

    void aScrubOnlyCommitsForTheSameSong()
    {
        ScrubSession session;
        session.begin(7);
        QVERIFY(session.acceptsChanges(7));
        QCOMPARE(session.finish(7, 0.5), 0.5);
        session.begin(7);
        QCOMPARE(session.finish(8, 0.5), -1.0);
        session.begin(7);
        session.cancel();
        QCOMPARE(session.finish(7, 0.5), -1.0);
        QVERIFY(!session.isEditing());
    }

    /// debugging: a song that loads for ever, for the visualizers' loading
    /// animation, asking the core and spotify for nothing
    void theLoadingTestLoadsUntilWithdrawn()
    {
        Rig rig;
        rig.player.playLoadingTest();
        QTest::qWait(700);
        QVERIFY(rig.player.hasSong());
        QCOMPARE(rig.player.currentSong().name, QStringLiteral("Loading Test"));
        QVERIFY(rig.player.isLoading());
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.player.isLoadingLyrics() || !rig.core.take(QStringLiteral("lyrics")));
        for (const QString &call : {QStringLiteral("playTrack"), QStringLiteral("playFile"),
                                    QStringLiteral("connect"), QStringLiteral("lyrics")}) {
            QVERIFY2(!rig.core.take(call), qPrintable(call));
        }

        // paused, it stops loading; played again, it loads again
        rig.player.pause();
        QVERIFY(!rig.player.isLoading());
        rig.player.togglePlayback();
        Rig::settle();
        QVERIFY(rig.player.isLoading());

        // a real song takes its place
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        QVERIFY(!rig.player.isLoading());
        QVERIFY(rig.player.isPlaying());
        QCOMPARE(rig.player.currentSong().name, queue[0].name);
    }

    void logOutForgetsTheQueue()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        QSignalSpy loggedOut(&rig.session, &SpotifySession::loggedOut);
        rig.session.logOut();
        QTRY_COMPARE(loggedOut.count(), 1);
        QVERIFY(!rig.player.hasSong());
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(rig.player.activeQueue().isEmpty());
    }

#if defined(SWIFTIFY_HAS_DBUS) && defined(Q_OS_LINUX)
    void mprisReflectsPlaybackStateAndControls()
    {
        Rig rig;
        Mpris mpris(&rig.player);
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Stopped"));
        QVERIFY(!mpris.hasSong());

        const QList<Song> queue = makeQueue(3);
        rig.player.play(queue[0], queue);
        rig.finishLoading();

        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Playing"));
        QVERIFY(mpris.hasSong());
        QVERIFY(mpris.canGoNext());
        QVERIFY(mpris.canSeek());
        const QVariantMap metadata = mpris.metadata();
        QCOMPARE(metadata.value(QStringLiteral("xesam:title")).toString(), queue[0].name);
        QCOMPARE(metadata.value(QStringLiteral("xesam:artist")).toStringList(), QStringList {queue[0].artists});

        rig.player.pause();
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Paused"));

        rig.player.resume();
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Playing"));
    }
#endif
};

QTEST_GUILESS_MAIN(PlaybackTest)
#include "tst_playback.moc"
