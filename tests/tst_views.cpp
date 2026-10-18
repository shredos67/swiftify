#include "app/Translations.h"
#include "FakeCore.h"
#include "ViewRig.h"

#include "app/AppController.h"
#include "app/Fonts.h"
#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "artwork/ArtworkCache.h"
#include "audio/AudioShaping.h"
#include "library/CollectionModels.h"
#include "library/LibraryStore.h"
#include "library/LocalLibrary.h"
#include "playback/PlaybackController.h"
#include "render/SpectrumItem.h"
#include "render/SpectrumSource.h"
#include "spotify/CredentialStore.h"
#include "spotify/OAuthFlow.h"
#include "spotify/SpotifySession.h"
#include "theme/PlayerStyle.h"
#include "theme/Theme.h"

#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QFontDatabase>
#include <QJSValue>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlListReference>
#include <QQmlProperty>
#include <QPointer>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <QtMath>
#include <rhi/qrhi.h>

#include <utility>

using namespace swiftify;
using swiftify::testing::FakeCore;

namespace {

/// covers without the network: every one is a small red square
class RedArt : public QQuickImageProvider {
public:
    RedArt() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &, QSize *size, const QSize &) override
    {
        QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        if (size) {
            *size = image.size();
        }
        return image;
    }
};

/// spotify's token endpoint, as far as a login goes: every request is
/// answered with a fresh token
class TokenServer : public QObject {
    Q_OBJECT

public:
    TokenServer()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    const QByteArray request = socket->peek(socket->bytesAvailable());
                    const qsizetype body = request.indexOf("\r\n\r\n");
                    const qsizetype length = request.mid(request.toLower().indexOf("content-length:") + 15).trimmed().split('\r').value(0).toInt();
                    if (body < 0 || request.size() < body + 4 + length) {
                        return;
                    }
                    socket->readAll();
                    const QByteArray json = R"({"access_token":"access","refresh_token":"refresh","expires_in":3600})";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/api/token").arg(server.serverPort())); }

    QTcpServer server;
};

/// the browser a login page opens in, which the test plays the part of
class Browser : public QObject {
    Q_OBJECT

public:
    QList<QUrl> opened;

    /// spotify, approving, sends it back to the app
    void approve()
    {
        const QString state = QUrlQuery(opened.last()).queryItemValue(QStringLiteral("state"));
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, OAuthFlow::kRedirectPort);
        if (!socket.waitForConnected(2000)) {
            return;
        }
        socket.write(QStringLiteral("GET /login?code=approved&state=%1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
                         .arg(state)
                         .toUtf8());
        socket.waitForBytesWritten(2000);
        socket.waitForReadyRead(2000);
    }

public slots:
    void open(const QUrl &url) { opened.append(url); }
};

} // namespace

/// the views build, lay out and draw without a single warning, with a song
/// playing so every panel has something to show. a view that only breaks
/// when it is first created (a binding to a read-only property, a missing
/// shader) fails here rather than in front of someone.
///
/// frames are drawn on the gpu into a texture through QQuickRenderControl,
/// since the offscreen platform would otherwise fall back to the software
/// renderer, which skips every shader. without a gpu the views are still
/// built and drawn in software, and the checks on pixels are skipped.
class ViewsTest : public QObject {
    Q_OBJECT

private:
    using Rig = swiftify::testing::ViewRig;

private slots:
    void languageSelectionRetranslatesTheSettings()
    {
        Rig rig;
        rig.settings.setLanguage(QStringLiteral("en"));
        Translations translations(&rig.settings);
        connect(&translations, &Translations::languageChanged, &rig.engine, &QQmlEngine::retranslate);
        const auto pages = rig.show(QStringLiteral("SettingsPages"));
        QVERIFY(pages);
        pages->setProperty("page", 4);
        auto *selection = pages->findChild<QObject *>(QStringLiteral("languageSelection"));
        QVERIFY(selection);
        QCOMPARE(selection->property("text").toString(), QStringLiteral("Interface language"));
        QVERIFY(QMetaObject::invokeMethod(selection, "activated", Q_ARG(int, 2)));
        QCOMPARE(rig.settings.language(), QStringLiteral("ru"));
        QCOMPARE(selection->property("text").toString(), QStringLiteral("Язык интерфейса"));
        for (int page = 0; page < 8; ++page) {
            pages->setProperty("page", page);
            Rig::settle();
            QVERIFY(!rig.render().isNull());
        }
        QVERIFY(QMetaObject::invokeMethod(selection, "activated", Q_ARG(int, 1)));
        QCOMPARE(rig.settings.language(), QStringLiteral("en"));
        QCOMPARE(selection->property("text").toString(), QStringLiteral("Interface language"));
    }

    void initTestCase()
    {
        // the face the app draws in, as main() loads it
        swiftify::fonts::loadInterfaceFont();
        // as main() sets it, before anything draws text
        qputenv("QT_MAX_CACHED_GLYPH_SIZE", "512");
#ifdef Q_OS_MACOS
        // macos's offscreen platform offers no gpu api of its own, so it
        // would draw in software; ask for metal, which needs no window
        qputenv("QT_QUICK_BACKEND", "rhi");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    }

    void init()
    {
        // macos's offscreen platform has no "Sans Serif"; nothing on linux says this
        QTest::failOnWarning(QRegularExpression(QStringLiteral("^(?!(Populating font family aliases|.*QWaylandTextInputv3)).*")));
    }

    void immersivePanels_data()
    {
        QTest::addColumn<QString>("panel");
        QTest::newRow("artwork") << QStringLiteral("artwork");
        QTest::newRow("queue") << QStringLiteral("queue");
        QTest::newRow("sleep") << QStringLiteral("sleep");
        QTest::newRow("audio") << QStringLiteral("audio");
    }

    void immersivePanels()
    {
        QFETCH(QString, panel);
        Rig rig;
        rig.playSomething();
        rig.settings.setImmersivePlayerPanel(panel);
        const auto player = rig.show(QStringLiteral("ImmersivePlayer"));
        QVERIFY(player);
        QVERIFY(!rig.frame.isNull());
    }

    void immersivePlayerFitsShortWindows_data()
    {
        QTest::addColumn<int>("windowControlsTopInset");
        QTest::newRow("native-controls") << 0;
        QTest::newRow("in-app-controls") << 36;
    }

    void immersivePlayerFitsShortWindows()
    {
        QFETCH(int, windowControlsTopInset);
        Rig rig;
        rig.playSomething();
        rig.settings.setCircularVisualizer(true);
        rig.settings.setImmersivePlayerPanel(QStringLiteral("artwork"));
        const auto player = rig.show(QStringLiteral("ImmersivePlayer"));
        QVERIFY(player);
        player->setProperty("windowControlsTopInset", windowControlsTopInset);
        auto *close = player->findChild<QQuickItem *>(QStringLiteral("immersiveCloseButton"));
        auto *nowPlaying = player->findChild<QQuickItem *>(QStringLiteral("immersiveNowPlaying"));
        auto *artwork = player->findChild<QQuickItem *>(QStringLiteral("immersiveArtwork"));
        auto *details = player->findChild<QQuickItem *>(QStringLiteral("immersiveDetails"));
        auto *seek = player->findChild<QQuickItem *>(QStringLiteral("immersiveSeek"));
        auto *transport = player->findChild<QQuickItem *>(QStringLiteral("immersiveTransport"));
        QVERIFY(close && nowPlaying && artwork && details && seek && transport);

        for (const int width : {724, 526}) {
            player->setSize(QSize(width, 410));
            Rig::settle();
            rig.render();
            QCOMPARE(close->x(), 24.0);
            QCOMPARE(close->y(), qMax(18.0, player->property("titleBarInset").toReal() + 6));
            QVERIFY(close->y() + close->height() <= close->parentItem()->height());
            QVERIFY(artwork->width() >= 160);
            QVERIFY(seek->width() >= 150);
            QVERIFY(transport->width() <= details->width());
            QVERIFY(artwork->mapToItem(player.get(), artwork->width(), 0).x()
                    < details->mapToItem(player.get(), 0, 0).x());
            QVERIFY(nowPlaying->mapToItem(player.get(), nowPlaying->width(), 0).x() <= width);
        }

        rig.settings.setCircularVisualizer(false);
        Rig::settle();
        rig.render();
        QVERIFY(nowPlaying->implicitHeight() <= 234);
        QVERIFY(artwork->width() >= 160);
        QVERIFY(seek->width() >= 150);

        rig.settings.setImmersivePlayerPanel(QStringLiteral("queue"));
        QMetaObject::invokeMethod(player.get(), "swapPanel", QStringLiteral("queue"), true);
        Rig::settle();
        QVERIFY(player->property("panelOnly").toBool());
        auto *panel = player->findChild<QQuickItem *>(QStringLiteral("immersivePanel"));
        QVERIFY(panel);
        QCOMPARE(panel->width(), player->property("rowSpace").toDouble());
        QVERIFY(panel->x() >= 0);
    }

    /// lyrics scrolled up past the panel are faded out of sight, and the
    /// buttons over them take the click, not the lines
    void scrolledLyricsLeaveTheTopBarClickable()
    {
        Rig rig;
        rig.settings.setPerformanceMode(true);
        QFile lyrics(rig.directory.filePath(QStringLiteral("sung.lrc")));
        QVERIFY(lyrics.open(QIODevice::WriteOnly));
        for (int line = 0; line < 40; ++line) {
            lyrics.write(QStringLiteral("[%1:%2.00]line %3\n")
                             .arg(line * 5 / 60, 2, 10, QLatin1Char('0'))
                             .arg(line * 5 % 60, 2, 10, QLatin1Char('0'))
                             .arg(line)
                             .toUtf8());
        }
        lyrics.close();
        Song song;
        song.uri = QUrl::fromLocalFile(rig.directory.filePath(QStringLiteral("sung.flac"))).toString();
        song.durationMs = 200'000;
        rig.player.play(song, {song});
        Rig::settle();
        QVERIFY(rig.player.lyricsSynchronized());
        rig.settings.setImmersivePlayerPanel(QStringLiteral("lyrics"));
        const auto player = rig.show(QStringLiteral("ImmersivePlayer"));
        QVERIFY(player);
        // the smallest window shows the lyrics alone, right under the top bar
        player->setSize(QSize(526, 410));
        Rig::settle();
        QVERIFY(player->property("panelOnly").toBool());
        auto *list = player->findChild<QQuickItem *>(QStringLiteral("immersiveLyricsList"));
        auto *close = player->findChild<QQuickItem *>(QStringLiteral("immersiveCloseButton"));
        auto *switcher = player->findChild<QQuickItem *>(QStringLiteral("immersiveSwitcher"));
        QVERIFY(list && close && switcher);
        list->setProperty("contentY", 600.0);
        Rig::settle();
        rig.render();

        const auto click = [&](QPointF point) {
            QTest::mouseClick(rig.window.get(), Qt::LeftButton, Qt::NoModifier, point.toPoint());
            Rig::settle();
        };
        QSignalSpy closing(player.get(), SIGNAL(closeRequested()));
        click(close->mapToScene(QPointF(close->width() / 2, close->height() / 2)));
        QCOMPARE(closing.count(), 1);
        // the switcher's last segment, Sound, on the right of the bar
        click(switcher->mapToScene(QPointF(switcher->width() - 14, switcher->height() / 2)));
        QTRY_COMPARE(player->property("panel").toString(), QStringLiteral("audio"));
    }

    /// in a short window the settings rail scrolls to its last destinations,
    /// and shows its scroll bar only then
    void theSettingsRailScrollsInShortWindows()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        // a plain window's overlay has no size, so the dialog sits in a host
        // the size of the window it would cover
        component.setData("import QtQuick\nimport Swiftify\nItem { id: host; width: 1280; height: 780\n"
                          "    property alias dialog: settings\n"
                          "    SettingsDialog { id: settings; parent: host } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        auto *dialog = object->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *rail = object->findChild<QQuickItem *>(QStringLiteral("settingsRail"));
        QVERIFY(rail);
        auto *bar = QQmlProperty(rail, QStringLiteral("ScrollBar.vertical"), qmlContext(rail)).read().value<QQuickItem *>();
        QVERIFY(bar);

        // everything fits in the usual window: nothing scrolls or shows
        Rig::settle();
        rig.render();
        QVERIFY(!rail->property("interactive").toBool());
        QVERIFY(!bar->isVisible());

        qobject_cast<QQuickItem *>(object.get())->setSize(QSizeF(526, 410));
        Rig::settle();
        rig.render();
        QVERIFY(rail->property("interactive").toBool());
        QVERIFY(bar->isVisible());
        const qreal bottom = rail->property("contentHeight").toReal() - rail->height();
        rail->setProperty("contentY", bottom);
        Rig::settle();
        rig.render();
        // About, the last destination, sits just above the rail's bottom edge
        QTest::mouseClick(rig.window.get(), Qt::LeftButton, Qt::NoModifier,
                          rail->mapToScene(QPointF(rail->width() / 2, rail->height() - 48)).toPoint());
        QTRY_COMPARE(dialog->property("page").toInt(), 7);
    }

    void longSongTitlesScrollInImmersivePlayer()
    {
        Rig rig;
        QList<Song> queue;
        Song longSong;
        longSong.name = QStringLiteral("This Is An Incredibly Long Song Title That Far Exceeds Normal Metadata Width whoever reads this pls save yourself this file is way too long");
        longSong.artists = QStringLiteral("Artist");
        longSong.albumName = QStringLiteral("Album");
        longSong.uri = QUrl::fromLocalFile(QStringLiteral("/music/longsong.flac")).toString();
        longSong.durationMs = 200'000;
        queue.append(longSong);

        rig.player.play(queue[0], queue);
        Rig::settle();
        const auto call = rig.core.take(QStringLiteral("playFile"));
        QVERIFY(call);
        call->succeed();
        Rig::settle();

        const auto player = rig.show(QStringLiteral("ImmersivePlayer"));
        QVERIFY(player);
        auto *titleItem = player->findChild<QQuickItem *>(QStringLiteral("immersiveTitle"));
        QVERIFY(titleItem);
        QCOMPARE(titleItem->property("text").toString(), longSong.name);
        QVERIFY(titleItem->property("overflows").toBool());
        QVERIFY(titleItem->property("shouldScroll").toBool());
        // when standing still at the start, the left side has no fade so the first character is not cut off or faded
        QCOMPARE(titleItem->property("leftEdge").toReal(), 0.0);
        QVERIFY(titleItem->property("rightEdge").toReal() > 0.0);

        Song shortSong;
        shortSong.name = QStringLiteral("Hi");
        shortSong.artists = QStringLiteral("Artist");
        shortSong.albumName = QStringLiteral("Album");
        shortSong.uri = QUrl::fromLocalFile(QStringLiteral("/music/shortsong.flac")).toString();
        shortSong.durationMs = 200'000;
        queue[0] = shortSong;
        rig.player.play(queue[0], queue);
        Rig::settle();
        const auto call2 = rig.core.take(QStringLiteral("playFile"));
        QVERIFY(call2);
        call2->succeed();
        Rig::settle();

        QCOMPARE(titleItem->property("text").toString(), shortSong.name);
        QVERIFY(!titleItem->property("overflows").toBool());
        QVERIFY(!titleItem->property("shouldScroll").toBool());
    }

    void multilineLyricsStayInsideTheirHighlight_data()
    {
        QTest::addColumn<QString>("words");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("minimumLines");
        QTest::addColumn<bool>("noShapes");
        for (bool noShapes : {false, true}) {
            const QByteArray suffix = noShapes ? "-plain" : "-wavy";
            QTest::newRow("single" + suffix) << QStringLiteral("jumping past") << 512 << 1 << noShapes;
            QTest::newRow("two" + suffix)
                << QStringLiteral("I'm so stuck in the past and I just can't get it back") << 512 << 2 << noShapes;
            QTest::newRow("three" + suffix)
                << QStringLiteral("'Cause I ain't been healthy (healthy), I'm so stuck on the past") << 512 << 3 << noShapes;
            QTest::newRow("narrow" + suffix)
                << QStringLiteral("'Cause I ain't been healthy (healthy), I'm so stuck on the past") << 280 << 4 << noShapes;
            QTest::newRow("explicit-breaks" + suffix)
                << QStringLiteral("jumping\npast\nyesterday") << 512 << 3 << noShapes;
        }
    }

    void multilineLyricsStayInsideTheirHighlight()
    {
        QFETCH(QString, words);
        QFETCH(int, width);
        QFETCH(int, minimumLines);
        QFETCH(bool, noShapes);
        Rig rig;
        rig.settings.setPerformanceMode(true);
        rig.settings.setNoShapes(noShapes);
        QFile lyrics(rig.directory.filePath(QStringLiteral("sung.lrc")));
        QVERIFY(lyrics.open(QIODevice::WriteOnly));
        lyrics.write("[00:00.00]placeholder\n");
        lyrics.close();
        Song song;
        song.uri = QUrl::fromLocalFile(rig.directory.filePath(QStringLiteral("sung.flac"))).toString();
        song.durationMs = 200'000;
        rig.player.play(song, {song});
        Rig::settle();
        QVERIFY(rig.player.hasLyrics());
        QVERIFY(rig.player.lyricsSynchronized());
        QCOMPARE(rig.player.activeLyricIndex(), 0);
        rig.player.lyrics()->setLines({{0, 200'000, words}});
        const auto panel = rig.show(QStringLiteral("ImmersiveLyrics"));
        QVERIFY(panel);
        panel->setWidth(width);
        auto *list = panel->findChild<QQuickItem *>(QStringLiteral("immersiveLyricsList"));
        QVERIFY(list);
        list->setProperty("contentY", -120.0);
        Rig::settle();
        rig.render();
        QQuickItem *line = nullptr;
        QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, line), Q_ARG(int, 0)));
        QVERIFY(line);
        auto *label = line->findChild<QQuickItem *>(QStringLiteral("lyricText"));
        auto *highlight = line->findChild<QQuickItem *>(QStringLiteral("lyricHighlight"));
        QVERIFY(label && highlight);
        QVERIFY(label->property("lineCount").toInt() >= minimumLines);
        QVERIFY(line->property("active").toBool());
        if (!rig.gpu) return;

        const qreal ratio = rig.window->devicePixelRatio();
        const QPointF origin = line->mapToScene(QPointF());
        const QRect pixels(qRound(origin.x() * ratio), qRound(origin.y() * ratio),
                           qRound(line->width() * ratio), qRound(line->height() * ratio));
        const QString preview = qEnvironmentVariable("SWIFTIFY_LYRICS_PREVIEW");
        if (!preview.isEmpty()) {
            rig.window->setColor(QColor(QStringLiteral("#381c23")));
            rig.render();
            QVERIFY(rig.render().copy(pixels).save(preview + QLatin1Char('.')
                + QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png")));
        }

        rig.window->setColor(Qt::black);
        QVERIFY(QQmlProperty(list, QStringLiteral("layer.enabled")).write(false));
        label->setVisible(false);
        highlight->setProperty("color", QColor(Qt::white));
        Rig::settle();
        rig.render();
        const QImage background = rig.render().copy(pixels);
        highlight->setVisible(false);
        line->setProperty("glowStrength", 0.0);
        label->setVisible(true);
        Rig::settle();
        rig.render();
        const QImage text = rig.render().copy(pixels);
        const int clearance = qRound(8 * ratio);
        int glyphPixels = 0;
        for (int row = 0; row < text.height(); ++row) {
            for (int column = 0; column < text.width(); ++column) {
                if (text.pixelColor(column, row).lightness() < 128) continue;
                ++glyphPixels;
                for (int offsetY : {-clearance, 0, clearance}) {
                    for (int offsetX : {-clearance, 0, clearance}) {
                        const QPoint sample(column + offsetX, row + offsetY);
                        QVERIFY2(background.rect().contains(sample)
                                     && background.pixelColor(sample).lightness() > 240,
                                 qPrintable(QStringLiteral("Glyph at %1,%2 lacks 8px of highlight padding (sample %3,%4: %5)")
                                                .arg(column).arg(row).arg(sample.x()).arg(sample.y())
                                                .arg(background.rect().contains(sample) ? background.pixelColor(sample).lightness() : -1)));
                    }
                }
            }
        }
        QVERIFY(glyphPixels > 100);
    }

    void collectionHeroKeepsPillButtonsWhileTheyFit()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "CollectionHero { width: 480; kind: 'PLAYLIST'; title: 'Ddd'; facts: '10 songs'\n"
                          "  actions: [TextButton { objectName: 'heroPlay'; text: 'Play'; symbol: 'play_arrow'; wavy: true; iconOnly: parent ? parent.compactActions : false },\n"
                          "            TextButton { objectName: 'heroShuffle'; text: 'Shuffle'; symbol: 'shuffle'; wavy: true; iconOnly: parent ? parent.compactActions : false }] }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *hero = qobject_cast<QQuickItem *>(object.get());
        hero->setParentItem(rig.window->contentItem());
        auto *artwork = hero->findChild<QQuickItem *>(QStringLiteral("heroArtwork"));
        auto *details = hero->findChild<QQuickItem *>(QStringLiteral("heroDetails"));
        auto *play = hero->findChild<QQuickItem *>(QStringLiteral("heroPlay"));
        auto *shuffle = hero->findChild<QQuickItem *>(QStringLiteral("heroShuffle"));
        QVERIFY(artwork && details && play && shuffle);
        Rig::settle();
        rig.render();
        QVERIFY(!hero->property("stacked").toBool());
        QVERIFY(artwork->x() + artwork->width() < details->x());
        QVERIFY(!play->property("iconOnly").toBool());
        QVERIFY(!shuffle->property("iconOnly").toBool());

        hero->setWidth(430);
        Rig::settle();
        rig.render();
        QVERIFY(hero->property("stacked").toBool());
        QVERIFY(!play->property("iconOnly").toBool());
    }

    void theSeekBarMovesBetweenPlayerUpdates()
    {
        Rig rig;
        rig.playSomething();
        rig.core.duration = 200'000;
        QTRY_COMPARE(rig.player.durationMs(), 200'000u);
        const auto bar = rig.show(QStringLiteral("SeekBar"));
        QVERIFY(bar);

        const double initial = bar->property("displayPositionMs").toDouble();
        QTest::qWait(110);
        const double moving = bar->property("displayPositionMs").toDouble();
        QVERIFY2(moving > initial + 50, qPrintable(QStringLiteral("the bar moved only %1 ms").arg(moving - initial)));

        rig.player.seek(50'000);
        QCOMPARE(bar->property("displayPositionMs").toDouble(), 50'000.0);
        rig.player.togglePlayback();
        const double paused = bar->property("displayPositionMs").toDouble();
        QTest::qWait(110);
        QCOMPARE(bar->property("displayPositionMs").toDouble(), paused);
    }

    void theSingletonsAreTheAppsOwn()
    {
        Rig rig;
        const QList<std::pair<const char *, QObject *>> singletons {
            {"Settings", &rig.settings}, {"Theme", &rig.theme}, {"Errors", &rig.errors},
            {"Session", &rig.session}, {"Player", &rig.player}, {"Library", &rig.library},
            {"Sound", &rig.sound}, {"App", &rig.app},
            {"PlayerStyle", &rig.style}, {"Spectrum", &rig.spectrum},
        };
        for (const auto &[name, expected] : singletons) {
            QQmlComponent probe(&rig.engine);
            probe.setData(QByteArrayLiteral("import QtQuick\nimport Swiftify\nQtObject { property QtObject seen: ") + name
                              + QByteArrayLiteral(" }"),
                          QUrl());
            std::unique_ptr<QObject> object(probe.create());
            QVERIFY2(object, qPrintable(probe.errorString()));
            QVERIFY2(object->property("seen").value<QObject *>() == expected, name);
        }
    }

    void aChosenPanelIsRememberedAndAnAutomaticOneIsNot()
    {
        Rig rig;
        rig.playSomething();
        rig.settings.setImmersivePlayerPanel(QStringLiteral("queue"));
        const auto player = rig.show(QStringLiteral("ImmersivePlayer"));
        QVERIFY(player);
        QCOMPARE(player->property("panel").toString(), QStringLiteral("queue"));

        QMetaObject::invokeMethod(player.get(), "swapPanel", QStringLiteral("audio"), true);
        QTRY_COMPARE(player->property("panel").toString(), QStringLiteral("audio"));
        QCOMPARE(rig.settings.immersivePlayerPanel(), QStringLiteral("audio"));

        QMetaObject::invokeMethod(player.get(), "swapPanel", QString(), false);
        QTRY_COMPARE(player->property("panel").toString(), QString());
        QCOMPARE(rig.settings.immersivePlayerPanel(), QStringLiteral("audio"));
        // the panel slides away fading, then the area empties
        QTRY_COMPARE(player->property("shownPanel").toString(), QString());
        QCOMPARE(player->property("panelOpacity").toReal(), 0.0);
        QCOMPARE(player->property("panelShift").toReal(), 1.0);

        // and one opening slides back in to its place
        QMetaObject::invokeMethod(player.get(), "swapPanel", QStringLiteral("queue"), true);
        QCOMPARE(player->property("shownPanel").toString(), QStringLiteral("queue"));
        QTRY_COMPARE(player->property("panelOpacity").toReal(), 1.0);
        QTRY_COMPARE(player->property("panelShift").toReal(), 0.0);
    }

    void theBackdropPoursTheCoverColours()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.playSomething();
        const auto backdrop = rig.show(QStringLiteral("LiquidBackdrop"));
        QVERIFY(backdrop);
        // the field is the accent green while nothing has artwork: whatever
        // the blur and the shade do, the middle of the window is not black
        // and leans green
        const QColor middle = rig.frame.pixelColor(rig.frame.width() / 2, rig.frame.height() / 2);
        QVERIFY2(middle.green() > 24, qPrintable(middle.name()));
        QVERIFY2(middle.green() > middle.red(), qPrintable(middle.name()));
    }

    void thePlaylistEditorOpensAndDrawsItsCovers()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "    property alias editor: editor\n"
                          "    PlaylistEditor { id: editor }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(object.get());
        item->setParentItem(rig.window->contentItem());
        item->setSize(Rig::kSize);
        auto *editor = object->property("editor").value<QObject *>();
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "openForCreate");
        Rig::settle();
        rig.render();
        // let the opening transition run, then draw what it settled on
        QTRY_VERIFY(editor->property("opened").toBool());
        const QImage frame = rig.render();
        QVERIFY(!frame.isNull());
        QCOMPARE(editor->property("headline").toString(), QStringLiteral("New Playlist"));
    }

    void tabKeyNavigatesPlaylistEditorFieldsAndButtons()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "    property alias editor: editor\n"
                          "    PlaylistEditor { id: editor }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(object.get());
        item->setParentItem(rig.window->contentItem());
        item->setSize(Rig::kSize);
        auto *editor = object->property("editor").value<QObject *>();
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "openForCreate");
        Rig::settle();
        rig.render();
        QTRY_VERIFY(editor->property("opened").toBool());

        // Focus starts on name field input
        QQuickItem *focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(focusItem->parentItem());
        QCOMPARE(focusItem->parentItem()->property("placeholder").toString(), QStringLiteral("Name"));

        // Pressing Tab moves to description field without inserting spaces into name
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(focusItem->parentItem());
        QCOMPARE(focusItem->parentItem()->property("placeholder").toString(), QStringLiteral("Description"));
        QCOMPARE(focusItem->property("text").toString(), QStringLiteral(""));

        // Pressing Shift+Tab moves back to name field
        QTest::keyClick(rig.window.get(), Qt::Key_Backtab, Qt::ShiftModifier);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(focusItem->parentItem());
        QCOMPARE(focusItem->parentItem()->property("placeholder").toString(), QStringLiteral("Name"));
        QCOMPARE(focusItem->property("text").toString(), QStringLiteral(""));

        // Enter name text so Save button becomes enabled
        focusItem->setProperty("text", QStringLiteral("Roadtrip Tunes"));
        Rig::settle();
        QCOMPARE(focusItem->property("text").toString(), QStringLiteral("Roadtrip Tunes"));

        // Tab to description field
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(focusItem->parentItem());
        QCOMPARE(focusItem->parentItem()->property("placeholder").toString(), QStringLiteral("Description"));

        // Tab from description to Public switch
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(QString::fromLatin1(focusItem->metaObject()->className()).startsWith(QLatin1StringView("StyledSwitch")));
        const bool initialChecked = focusItem->property("checked").toBool();
        QTest::keyClick(rig.window.get(), Qt::Key_Space);
        Rig::settle();
        QCOMPARE(focusItem->property("checked").toBool(), !initialChecked);

        // Tab from Public switch to Save button
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QCOMPARE(focusItem->property("text").toString(), QStringLiteral("Save"));

        // Tab from Save to Cancel button
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QCOMPARE(focusItem->property("text").toString(), QStringLiteral("Cancel"));

        // Tab from Cancel wraps back to Name field
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(focusItem->parentItem());
        QCOMPARE(focusItem->parentItem()->property("placeholder").toString(), QStringLiteral("Name"));

        // For local playlists, the public switch is hidden so Tab skips it
        QMetaObject::invokeMethod(editor, "close");
        Rig::settle();
        QMetaObject::invokeMethod(editor, "openForCreateLocal", Q_ARG(QVariant, QVariant()));
        Rig::settle();
        QTRY_VERIFY(editor->property("opened").toBool());

        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(focusItem->parentItem());
        QCOMPARE(focusItem->parentItem()->property("placeholder").toString(), QStringLiteral("Name"));
        focusItem->setProperty("text", QStringLiteral("Local Gems"));
        Rig::settle();

        // Tab to description
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QVERIFY(focusItem->parentItem());
        QCOMPARE(focusItem->parentItem()->property("placeholder").toString(), QStringLiteral("Description"));

        // Tab from description skips hidden switch directly to Save button
        QTest::keyClick(rig.window.get(), Qt::Key_Tab);
        Rig::settle();
        focusItem = rig.window->activeFocusItem();
        QVERIFY(focusItem);
        QCOMPARE(focusItem->property("text").toString(), QStringLiteral("Save"));
    }

    void everySettingsPageDraws()
    {
        Rig rig;
        const auto pages = rig.show(QStringLiteral("SettingsPages"));
        QVERIFY(pages);
        // visualizer to performance, account, about
        for (int page = 0; page < 8; ++page) {
            pages->setProperty("page", page);
            Rig::settle();
            rig.render();
            QVERIFY2(pages->implicitHeight() > 100, qPrintable(QString::number(page)));
        }

        // the account's page holds what the sidebar's card did
        pages->setProperty("page", 6);
        Rig::settle();
        const std::function<QQuickItem *(QQuickItem *, const QString &)> shown = [&](QQuickItem *item,
                                                                                   const QString &text) -> QQuickItem * {
            if (item->isVisible() && item->property("text").toString() == text) {
                return item;
            }
            for (QQuickItem *child : item->childItems()) {
                if (QQuickItem *found = shown(child, text)) {
                    return found;
                }
            }
            return nullptr;
        };
        for (const QString &action : {QStringLiteral("Connect Library"), QStringLiteral("Connect Playback"),
                                      QStringLiteral("Log Out")}) {
            QVERIFY2(shown(pages.get(), action), qPrintable(action));
        }
        // with nobody signed in, there is nobody to log out
        QVERIFY(!shown(pages.get(), QStringLiteral("Log Out"))->isEnabled());
    }

    /// the foot of the sidebar opens the settings, where the account now is
    void theSidebarsFootOpensTheSettings()
    {
        Rig rig;
        const auto sidebar = rig.show(QStringLiteral("Sidebar"));
        QVERIFY(sidebar);
        QQuickItem *button = nullptr;
        const std::function<void(QQuickItem *)> find = [&](QQuickItem *item) {
            if (!button && item->property("text").toString() == QStringLiteral("Settings")
                && QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1StringView("NavigationItem"))) {
                button = item;
            }
            for (QQuickItem *child : item->childItems()) {
                find(child);
            }
        };
        find(sidebar.get());
        QVERIFY(button);
        QVERIFY(button->isVisible());
        auto *ui = rig.engine.singletonInstance<QObject *>("Swiftify", "UiState");
        QVERIFY(ui);
        QSignalSpy asked(ui, SIGNAL(settingsRequested()));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(asked.count(), 1);
    }

    void filledIconsHaveNoHoles_data()
    {
        QTest::addColumn<QString>("symbol");
        for (const char *symbol : {"account_circle", "library_music", "home", "favorite"}) {
            QTest::newRow(symbol) << QString::fromLatin1(symbol);
        }
    }

    // a filled glyph is drawn from overlapping outlines; a renderer that
    // punches holes where they overlap leaves icons half filled
    void filledIconsHaveNoHoles()
    {
        QFETCH(QString, symbol);
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw with");
        }
        swiftify::fonts::loadIconFont();
        rig.window->setColor(Qt::white);
        const auto draw = [&](const QString &item) {
            QQmlComponent component(&rig.engine);
            component.setData(("import QtQuick\nimport Swiftify\nItem { width: 200; height: 200\n" + item + " }").toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const QImage icon = draw(QStringLiteral("MaterialSymbol { x: 20; y: 20; text: \"%1\"; iconSize: 160; fill: 1; color: \"black\" }").arg(symbol));
        // the same glyph through the platform's own rasteriser
        const QImage reference = draw(QStringLiteral(
            "Text { x: 20; y: 20; width: 160; height: 160; text: \"%1\"; color: \"black\"; renderType: Text.NativeRendering\n"
            "horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter\n"
            "font.family: \"Material Symbols Rounded\"; font.pixelSize: 160\n"
            "font.variableAxes: ({ \"FILL\": 1, \"wght\": 400, \"GRAD\": 0, \"opsz\": 48 }) }").arg(symbol));
        const auto dark = [](const QImage &image, int x, int y) { return qGray(image.pixel(x, y)) < 128; };
        int holes = 0;
        for (int y = 2; y < 198; ++y) {
            for (int x = 2; x < 198; ++x) {
                bool interior = true;
                for (int dy = -2; dy <= 2 && interior; dy += 2) {
                    for (int dx = -2; dx <= 2 && interior; dx += 2) {
                        interior = dark(reference, x + dx, y + dy);
                    }
                }
                if (interior && !dark(icon, x, y)) {
                    ++holes;
                }
            }
        }
        QVERIFY2(holes < 10, qPrintable(QStringLiteral("%1 pixels missing inside the glyph").arg(holes)));
    }

    // a divider draws as the seek line's shader drew it, the wave the whole
    // way across, now from its own vertices
    void wavyLinesDrawAsTheShaderDid()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw with");
        }
        rig.window->setColor(Qt::white);
        const auto draw = [&](const QString &item) {
            QQmlComponent component(&rig.engine);
            component.setData(("import QtQuick\nimport Swiftify\nItem { width: 300; height: 60\n" + item + " }").toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            if (!object) {
                qWarning() << component.errors();
                return QImage();
            }
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const QString box = QStringLiteral("x: 13.5; y: 20; width: 231; height: 10");
        const QImage line = draw(QStringLiteral("WavyLine { %1; color: \"black\"; amplitude: 1.6; wavelength: 14; phase: 0.7 }").arg(box));
        const QImage reference = draw(QStringLiteral(
            "ShaderEffect { %1\n"
            "property size itemSize: Qt.size(width, height); property real progress: 1\n"
            "property real amplitude: 1.6; property real wavelength: 14; property real phase: 0.7\n"
            "property real thickness: 1.5; property real handleWidth: 0; property real handleHeight: 0\n"
            "property real gap: 0; property color activeColor: \"black\"\n"
            "property color trackColor: \"transparent\"; property color handleColor: \"transparent\"\n"
            "fragmentShader: \"qrc:/shaders/wavy_progress.frag.qsb\" }").arg(box));
        QVERIFY(!line.isNull() && !reference.isNull());
        double difference = 0;
        double ink = 0;
        for (int y = 0; y < line.height(); ++y) {
            for (int x = 0; x < line.width(); ++x) {
                difference += std::abs(qGray(line.pixel(x, y)) - qGray(reference.pixel(x, y))) / 255.0;
                ink += (255 - qGray(reference.pixel(x, y))) / 255.0;
            }
        }
        QVERIFY2(ink > 100, "the shader drew nothing");
        QVERIFY2(difference < ink * 0.02, qPrintable(QStringLiteral("%1 apart of %2").arg(difference).arg(ink)));
    }

    void iconsLandWhereTextPutThem_data()
    {
        QTest::addColumn<QString>("symbol");
        QTest::addColumn<int>("size");
        QTest::addColumn<int>("fill");
        for (const char *symbol : {"play_arrow", "skip_next", "favorite", "search"}) {
            for (const int size : {18, 24, 40}) {
                for (const int fill : {0, 1}) {
                    QTest::addRow("%s %d %d", symbol, size, fill) << QString::fromLatin1(symbol) << size << fill;
                }
            }
        }
    }

    // an icon is filled from the font's outline into the atlas rather than
    // drawn as text, and must come out as the platform draws the glyph: in
    // the same place, as heavy, at the sizes the app shows them
    void iconsLandWhereTextPutThem()
    {
        QFETCH(QString, symbol);
        QFETCH(int, size);
        QFETCH(int, fill);
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw with");
        }
        swiftify::fonts::loadIconFont();
        rig.window->setColor(Qt::white);
        const auto draw = [&](const QString &item) {
            QQmlComponent component(&rig.engine);
            component.setData(("import QtQuick\nimport Swiftify\nItem { width: 100; height: 100\n" + item + " }").toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const QString box = QStringLiteral("x: 13; y: 17; width: %1; height: %1").arg(size + 6);
        const int opsz = size < 22 ? 20 : (size < 32 ? 24 : (size < 44 ? 40 : 48));
        const QImage icon = draw(QStringLiteral("MaterialSymbol { %1; text: \"%2\"; iconSize: %3; fill: %4; color: \"black\" }")
                                     .arg(box, symbol).arg(size).arg(fill));
        const QImage reference = draw(QStringLiteral(
            "Text { %1; text: \"%2\"; color: \"black\"; renderType: Text.NativeRendering\n"
            "horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter\n"
            "font.family: \"Material Symbols Rounded\"; font.pixelSize: %3\n"
            "font.variableAxes: ({ \"FILL\": %4, \"wght\": 400, \"GRAD\": 0, \"opsz\": %5 }) }")
                                          .arg(box, symbol).arg(size).arg(fill).arg(opsz));
        if (qEnvironmentVariableIsSet("VIEWS_DUMP_ICONS")) {
            const QString name = QStringLiteral("%1/%2-%3-%4").arg(qEnvironmentVariable("VIEWS_DUMP_ICONS"), symbol).arg(size).arg(fill);
            icon.save(name + QStringLiteral("-atlas.png"));
            reference.save(name + QStringLiteral("-text.png"));
        }
        // the ink: how dark each pixel is, and where its middle is
        const auto ink = [](const QImage &image) {
            double total = 0;
            double x = 0;
            double y = 0;
            for (int row = 0; row < image.height(); ++row) {
                for (int column = 0; column < image.width(); ++column) {
                    const double dark = (255 - qGray(image.pixel(column, row))) / 255.0;
                    total += dark;
                    x += dark * column;
                    y += dark * row;
                }
            }
            return std::array {total, total > 0 ? x / total : 0, total > 0 ? y / total : 0};
        };
        const auto drawn = ink(icon);
        const auto expected = ink(reference);
        QVERIFY2(expected[0] > 0, "the platform drew nothing");
        QVERIFY2(std::abs(drawn[0] - expected[0]) <= expected[0] * 0.08,
                 qPrintable(QStringLiteral("ink %1, the platform's %2").arg(drawn[0]).arg(expected[0])));
        // within half a pixel, as rounding to the pixel grid leaves it
        const double ratio = icon.devicePixelRatio();
        QVERIFY2(std::abs(drawn[1] - expected[1]) <= 0.75 * ratio && std::abs(drawn[2] - expected[2]) <= 0.75 * ratio,
                 qPrintable(QStringLiteral("middle at %1,%2, the platform's at %3,%4")
                                .arg(drawn[1]).arg(drawn[2]).arg(expected[1]).arg(expected[2])));
    }

    void aRoundCoverWearsASquiggle()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.window->setColor(Qt::transparent);
        // a round cover `size` across with its top left at (20, 20), drawn
        struct Drawn { QImage frame; qreal centre; qreal waves; qreal thickness; qreal amplitude; bool ready; };
        const auto draw = [&](bool squiggly, int size) {
            QQmlComponent component(&rig.engine);
            component.setData(QStringLiteral("import QtQuick\nimport Swiftify\n"
                                             "ArtworkImage { x: 20; y: 20; width: %2; height: %2; circular: true; "
                                             "squiggleColor: \"#00ff00\"; squiggly: %1 }")
                                  .arg(squiggly ? QStringLiteral("true") : QStringLiteral("false")).arg(size).toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            // Unlike the old solid rectangle, fallback sleeves are decoded
            // asynchronously through the artwork provider. Test the cut only
            // after its texture exists, not the transparent loading frame.
            const bool ready = QTest::qWaitFor([&] {
                for (QObject *child : object->findChildren<QObject *>()) {
                    if (child->property("source").toUrl().toString().contains(QStringLiteral("swiftify-cover"))
                        && child->property("status").toInt() == 1) return true; // Image.Ready
                }
                return false;
            }, 2000);
            Rig::settle();
            rig.render();
            return Drawn { rig.render(), 20 + size / 2.0, object->property("squiggleWaves").toReal(),
                           object->property("squiggleThickness").toReal(), object->property("squiggleAmplitude").toReal(), ready };
        };
        const auto at = [](const Drawn &drawn, qreal angle, qreal radius) {
            return drawn.frame.pixel(qFloor(drawn.centre + radius * std::sin(angle)),
                                     qFloor(drawn.centre - radius * std::cos(angle)));
        };
        const auto green = [](QRgb pixel) { return qGreen(pixel) > 220 && qRed(pixel) < 40 && qBlue(pixel) < 40; };
        // how much of a circle round the cover is drawn at all
        const auto covered = [&](const Drawn &drawn, qreal radius) {
            int count = 0;
            for (int i = 0; i < 720; ++i) {
                count += qAlpha(at(drawn, 2 * M_PI * i / 720, radius)) > 128 ? 1 : 0;
            }
            return count / 720.0;
        };

        const Drawn plain = draw(false, 120);
        const Drawn squiggly = draw(true, 120);
        QVERIFY(plain.ready && squiggly.ready);
        // just inside the plain cover's edge it is solid all the way round;
        // the squiggly one's edge comes and goes across that circle, as its
        // wave does, and nothing of it is outside the cover's own square
        QVERIFY2(covered(plain, 58) > 0.97, qPrintable(QString::number(covered(plain, 58))));
        const qreal wavering = covered(squiggly, 58);
        QVERIFY2(wavering > 0.25 && wavering < 0.85, qPrintable(QString::number(wavering)));
        QCOMPARE(covered(squiggly, 60.5), 0.0);
        QVERIFY2(covered(squiggly, 40) > 0.99, "the squiggle cuts too deep");
        // and the middle is all cover, with no speck of the line in it
        for (const QPoint point : {QPoint(80, 80), QPoint(79, 79), QPoint(80, 79), QPoint(79, 80)}) {
            const QRgb middle = squiggly.frame.pixel(point);
            QVERIFY2(qAlpha(middle) > 250 && !green(middle), qPrintable(QColor::fromRgba(middle).name(QColor::HexArgb)));
        }

        // at every size the line and the cover's edge are one sine, the
        // edge two line widths straight in from the line: going in at a
        // crest, a trough or anywhere between, the line in the squiggle's
        // colour, a gap that shows what is behind, then the cover
        for (const int size : {120, 400}) {
            const Drawn drawn = draw(true, size);
            QVERIFY(drawn.ready);
            const qreal t = drawn.thickness;
            const qreal middle = size / 2.0 - drawn.amplitude - t / 2 - 0.75;
            // the proportions hold, so a big cover's waves do not bunch up
            QVERIFY2(drawn.waves >= 12 && drawn.waves <= 24, qPrintable(QString::number(drawn.waves)));
            int lines = 0;
            int gaps = 0;
            int covers = 0;
            int samples = 0;
            for (int wave = 0; wave < int(drawn.waves); ++wave) {
                for (const qreal step : {0.0, 0.25, 0.5, 0.75}) {
                    const qreal angle = (2 * M_PI * (wave + step)) / drawn.waves;
                    const qreal line = middle + drawn.amplitude * std::sin(2 * M_PI * step);
                    const qreal edge = line - 2 * t;
                    ++samples;
                    lines += green(at(drawn, angle, line)) ? 1 : 0;
                    gaps += qAlpha(at(drawn, angle, (line - t / 2 + edge) / 2 - 0.25)) < 40 ? 1 : 0;
                    covers += qAlpha(at(drawn, angle, edge - 1.5)) > 220 ? 1 : 0;
                }
            }
            const QString counts = QStringLiteral("at %1 px: %2 lines, %3 gaps, %4 covers of %5")
                                       .arg(size).arg(lines).arg(gaps).arg(covers).arg(samples);
            QVERIFY2(lines >= samples - 2, qPrintable(counts));
            QVERIFY2(gaps >= samples - 2, qPrintable(counts));
            QVERIFY2(covers >= samples - 2, qPrintable(counts));
        }
    }

    void theMacMenuBarBuilds()
    {
        // native menus come from the platform; the offscreen one has none
        if (QGuiApplication::platformName() != QLatin1StringView("cocoa")) {
            QSKIP("no native menu bar on this platform");
        }
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.loadFromModule("Swiftify", "MacMenuBar");
        std::unique_ptr<QObject> menuBar(component.create());
        QVERIFY2(menuBar, qPrintable(component.errorString()));
        // file, edit, view, controls and window
        QCOMPARE(QQmlListReference(menuBar.get(), "menus").count(), 5);
    }

    void goingBackShowsThePageAsItWasLeft()
    {
        Rig rig;
        const auto host = rig.show(QStringLiteral("PageHost"));
        QVERIFY(host);
        const auto page = [&] { return host->property("currentPage").value<QQuickItem *>(); };
        QPointer<QQuickItem> first = page();
        QVERIFY(first);

        rig.library.showPlaylists();
        QPointer<QQuickItem> second = page();
        QVERIFY(second && second != first);
        // the page left behind is kept, hidden, not rebuilt
        QVERIFY(first);
        QVERIFY(!first->isVisible());

        rig.library.showSearch();
        QPointer<QQuickItem> third = page();
        QVERIFY(third && third != second);
        QCOMPARE(rig.library.historyDepth(), 2);

        // back goes to the very same pages, shown again
        rig.library.goBack();
        QCOMPARE(page(), second.data());
        QVERIFY(second->isVisible());
        // the page left is let go
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!third);
        // one page is kept behind the one showing; the first was let go when
        // the third was built, and back builds it afresh
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!first);
        rig.library.goBack();
        QVERIFY(page());
        QVERIFY(page()->isVisible());
        QCOMPARE(rig.library.historyDepth(), 0);

        // going on again builds a new page
        rig.library.showSearch();
        QVERIFY(page() != second.data());
        QVERIFY(page() != first.data());
    }

    void aRingIsCutAlongTheSquiggle()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.window->setColor(Qt::transparent);
        // a 400 px ring centred at (220, 220); its first bar points half a
        // step of its 72 clockwise of straight up
        const auto draw = [&](int waves) {
            QQmlComponent component(&rig.engine);
            component.setData(QStringLiteral("import QtQuick\nimport Swiftify\n"
                                             "SpectrumView { x: 20; y: 20; width: 400; height: 400; kind: SpectrumView.Ring; "
                                             "active: true; cutWaves: %1; cutRadius: 122; cutAmplitude: 0 }")
                                  .arg(waves).toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const QImage whole = draw(0);
        const QImage cut = draw(20);
        const double theta = -M_PI / 2 + M_PI / 72;
        const auto alongTheBar = [&](const QImage &frame, double radius) {
            const qreal ratio = qreal(frame.width()) / rig.window->width();
            const QPointF at(220 + radius * std::cos(theta), 220 + radius * std::sin(theta));
            return qAlpha(frame.pixel((at * ratio).toPoint()));
        };
        // the bars stand under where the cover would be, from a little inside
        // its circle; uncut they show there, cut they start at the line
        QVERIFY2(alongTheBar(whole, 112) > 100, "the bar's foot is not drawn");
        QCOMPARE(alongTheBar(cut, 112), 0);
        QVERIFY2(alongTheBar(cut, 140) > 100, "the bar is cut beyond the line");
    }

    void aResizedCoverKeepsShowingWhileItDecodesAgain()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.engine.addImageProvider(QStringLiteral("art"), new RedArt);
        rig.window->setColor(Qt::transparent);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "ArtworkImage { x: 10; y: 10; width: 60; height: 60; radius: 0; source: \"https://example.com/a.jpg\" }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *cover = qobject_cast<QQuickItem *>(object.get());
        cover->setParentItem(rig.window->contentItem());
        const auto red = [](QRgb pixel) { return qRed(pixel) > 200 && qGreen(pixel) < 40 && qBlue(pixel) < 40; };
        // loaded and faded in
        QTRY_VERIFY(object->property("ready").toBool());
        QTest::qWait(400);
        QVERIFY(red(rig.render().pixel(40, 40)));

        // a small resize stays within the size it decoded at
        const int decoded = object->property("pixelSize").toInt();
        cover->setSize(QSizeF(62, 62));
        QCOMPARE(object->property("pixelSize").toInt(), decoded);

        // a big one decodes again, and the cover stays up meanwhile
        cover->setSize(QSizeF(300, 300));
        QVERIFY(object->property("pixelSize").toInt() > decoded);
        QVERIFY(red(rig.render().pixel(160, 160)));
        QTRY_VERIFY(object->property("ready").toBool());
        QVERIFY(red(rig.render().pixel(160, 160)));
    }

    void theCornerCameoComesUpForItsSoundtracks()
    {
        Rig rig;
        const auto cameo = rig.show(QStringLiteral("CornerCameo"));
        QVERIFY(cameo);
        const auto matches = [&](const QString &album) {
            bool result = false;
            QMetaObject::invokeMethod(cameo.get(), "isItsSoundtrack", Q_RETURN_ARG(bool, result), Q_ARG(QString, album));
            return result;
        };
        QVERIFY(matches(QStringLiteral("TVアニメ「僕のヒーローアカデミア」オリジナル・サウンドトラック")));
        QVERIFY(matches(QStringLiteral("My Hero Academia: Season 2 (Original Soundtrack)")));
        QVERIFY(matches(QStringLiteral("Boku no Hero Academia OST")));
        QVERIFY(!matches(QStringLiteral("Haikyu!! Original Soundtrack")));

        // it rises, holds, and drops back out of sight
        QCOMPARE(cameo->property("lift").toReal(), 0.0);
        QMetaObject::invokeMethod(cameo.get(), "pop");
        QTRY_COMPARE_WITH_TIMEOUT(cameo->property("lift").toReal(), 1.0, 2000);
        QTest::qWait(2000);
        QCOMPARE(cameo->property("lift").toReal(), 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(cameo->property("lift").toReal(), 0.0, 6000);
    }

    /// a red pill on a black track: three empty 80 px segments from (24, 24)
    static std::unique_ptr<QObject> segmentedControl(Rig &rig, const QByteArray &extra)
    {
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { width: 400; height: 100\n"
                          "  property alias picker: picker\n"
                          "  SegmentedControl { id: picker; x: 20; y: 20; segmentWidth: 80; currentIndex: 1\n"
                          "    trackColor: \"black\"; indicatorColor: \"red\"\n"
                          "    segments: [Segment {}, Segment {}, Segment {}] }\n"
                          + extra + " }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        if (!object) {
            qWarning().noquote() << component.errorString();
            return {};
        }
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();
        return object;
    }

    static QPoint segmentCentre(int index) { return QPoint(24 + 80 * index + 40, 24 + 18); }

    /// the frame's pixel under a point of the window, at however many pixels a point it draws
    static QColor pixelAt(const Rig &rig, const QImage &frame, QPoint point)
    {
        const qreal ratio = qreal(frame.width()) / rig.window->width();
        return frame.pixelColor((QPointF(point) * ratio).toPoint());
    }

    void aSegmentSettledOnAsItIsBuiltIsWhereThePillStarts()
    {
        Rig rig;
        // as the immersive player does, choosing its panel once it is built,
        // before the row has laid the segments out
        const auto object = segmentedControl(rig, "Component.onCompleted: picker.currentIndex = 2");
        QVERIFY(object);
        rig.render();
        const QImage frame = rig.render();
        const auto red = [](QColor pixel) { return pixel.red() > 200 && pixel.green() < 40 && pixel.blue() < 40; };
        QVERIFY2(red(pixelAt(rig, frame, segmentCentre(2))), "the pill is not on its segment from the first frame");
        QVERIFY(!red(pixelAt(rig, frame, segmentCentre(1))));
        QVERIFY(!red(pixelAt(rig, frame, segmentCentre(0))));

        // a choice made once it shows slides there
        object->property("picker").value<QObject *>()->setProperty("currentIndex", 0);
        Rig::settle();
        QVERIFY2(!red(pixelAt(rig, rig.render(), segmentCentre(0))), "the pill jumped rather than slid");
        bool arrived = false;
        for (int frame = 0; frame < 120 && !arrived; ++frame) {
            QTest::qWait(16);
            arrived = red(pixelAt(rig, rig.render(), segmentCentre(0)));
        }
        QVERIFY2(arrived, "the pill never reached the new choice");
    }

    void aHoveredSegmentsTintLiesUnderThePill()
    {
        Rig rig;
        const auto object = segmentedControl(rig, "");
        QVERIFY(object);
        rig.render();
        const auto hover = [&](QPoint point) {
            QMouseEvent move(QEvent::MouseMove, point, point, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(rig.window.get(), &move);
            Rig::settle();
            // the tint's fade
            QTest::qWait(400);
            return pixelAt(rig, rig.render(), point);
        };
        // an unchosen segment shows its tint on the track
        QVERIFY(hover(segmentCentre(0)) != QColor(Qt::black));
        // the chosen one keeps its pill on top
        const QColor chosen = hover(segmentCentre(1));
        QVERIFY2(chosen.red() > 200 && chosen.green() < 40 && chosen.blue() < 40, qPrintable(chosen.name()));
    }

    void theSpotlightShowsTheCoverOnceHomesFirstLoadArrives()
    {
        Rig rig;
        rig.engine.addImageProvider(QStringLiteral("art"), new ArtworkImageProvider);
        // a cover red on the left and blue on the right
        QTemporaryDir dir;
        QImage cover(256, 256, QImage::Format_RGB32);
        for (int y = 0; y < 256; ++y) {
            for (int x = 0; x < 256; ++x) {
                cover.setPixel(x, y, x < 128 ? qRgb(230, 30, 30) : qRgb(30, 60, 230));
            }
        }
        const QString path = dir.filePath(QStringLiteral("cover.png"));
        QVERIFY(cover.save(path));

        // home is built before the recent songs come, as when the app starts
        const auto home = rig.show(QStringLiteral("HomeView"));
        QVERIFY(home);
        const std::function<QQuickItem *(QQuickItem *, QLatin1StringView)> find = [&](QQuickItem *item, QLatin1StringView type) {
            if (QString::fromLatin1(item->metaObject()->className()).startsWith(type)) {
                return item;
            }
            for (QQuickItem *child : item->childItems()) {
                if (QQuickItem *found = find(child, type)) {
                    return found;
                }
            }
            return static_cast<QQuickItem *>(nullptr);
        };
        QQuickItem *glow = find(home.get(), QLatin1StringView("ArtworkGlow"));
        QQuickItem *wash = find(home.get(), QLatin1StringView("QQuickMultiEffect"));
        QVERIFY(glow && wash);
        const QColor themed = glow->property("primary").value<QColor>();

        Song song;
        song.name = QStringLiteral("Spotlit");
        song.artists = QStringLiteral("Artist");
        song.uri = QStringLiteral("spotify:track:spotlit");
        song.artworkUrl = QUrl::fromLocalFile(path);
        rig.library.recentlyPlayed()->setSongs({song});
        Rig::settle();
        QCOMPARE(home->property("spotlightSong").value<Song>().name, song.name);
        // the glow takes the cover's colours
        QTRY_VERIFY(glow->property("primary").value<QColor>() != themed);

        // and the card's wash is the cover blurred: red to the left, blue to
        // the right. read along the card's top, clear of the cover, its glow
        // and the words
        const auto tint = [&](qreal across) {
            const QImage frame = rig.render();
            const qreal ratio = qreal(frame.width()) / rig.window->width();
            const QPointF at = wash->mapToScene(QPointF(wash->width() * across, 10));
            const QColor pixel = frame.pixelColor((at * ratio).toPoint());
            return pixel.blueF() - pixel.redF();
        };
        bool washed = false;
        for (int frame = 0; frame < 100 && !washed; ++frame) {
            QTest::qWait(20);
            washed = tint(0.9) > 0.08 && tint(0.3) < -0.08;
        }
        QVERIFY2(washed, qPrintable(QStringLiteral("left %1, right %2").arg(tint(0.3)).arg(tint(0.9))));
    }

    void draggingAFaderDoesNotScrollItsPanel()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Flickable { width: 300; height: 200; contentHeight: 800\n"
                          "  property alias fader: fader\n"
                          "  VerticalFader { id: fader; x: 100; y: 30; height: 132; onMoved: (next) => value = next } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();

        const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
            QMouseEvent event(type, at, at, at, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(rig.window.get(), &event);
        };
        // grab the cap in the middle and drag it well up
        QPointF at(122, 30 + 66);
        send(QEvent::MouseButtonPress, at, Qt::LeftButton);
        for (int step = 0; step < 8; ++step) {
            at.ry() -= 8;
            send(QEvent::MouseMove, at, Qt::LeftButton);
        }
        send(QEvent::MouseButtonRelease, at, Qt::NoButton);
        Rig::settle();

        QCOMPARE(object->property("contentY").toReal(), 0.0);
        QVERIFY(object->property("fader").value<QObject *>()->property("value").toReal() > 0);
    }

    void aClickedCoverFliesToTheHero()
    {
        Rig rig;
        rig.engine.addImageProvider(QStringLiteral("art"), new RedArt);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "  id: root; width: 800; height: 600\n"
                          "  property alias card: card\n"
                          "  property bool claimedWhileUnarmed\n"
                          "  MediaArtwork { id: card; x: 30; y: 300; width: 120; height: 120; source: \"https://example.com/a.jpg\" }\n"
                          "  HeroFlight { anchors.fill: parent }\n"
                          "  function launch() { UiState.launchHero(card.cover, card.source, false) }\n"
                          "  function flyHome() { UiState.launchReturn(Qt.rect(40, 40, 232, 232), card.cover, card.source, false) }\n"
                          "  function open(): Item {\n"
                          "    return Qt.createQmlObject('import Swiftify; CollectionHero { width: 800 }', root)\n"
                          "  }\n"
                          "  Component.onCompleted: claimedWhileUnarmed = UiState.claimHero(card)\n"
                          "}", QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        // nothing was clicked, so a hero has nothing to take
        QVERIFY(!object->property("claimedWhileUnarmed").toBool());

        QObject *state = rig.engine.singletonInstance<QObject *>("Swiftify", "UiState");
        QVERIFY(state);
        auto *cover = object->property("card").value<QObject *>()->property("cover").value<QQuickItem *>();
        QVERIFY(cover);
        QVERIFY(QMetaObject::invokeMethod(object.get(), "launch"));
        QVERIFY(state->property("heroArmed").toBool());
        QCOMPARE(state->property("heroFrom").toRectF(), QRectF(30, 300, 120, 120));

        // the page that opens takes the cover in flight, and hides its own
        // until it lands
        QQuickItem *hero = nullptr;
        QVERIFY(QMetaObject::invokeMethod(object.get(), "open", Q_RETURN_ARG(QQuickItem *, hero)));
        QVERIFY(hero);
        QVERIFY(!state->property("heroArmed").toBool());
        QVERIFY(state->property("heroTarget").value<QQuickItem *>());
        Rig::settle();
        rig.render();

        // it springs into place and hands over once the hero's cover is in
        QTRY_VERIFY_WITH_TIMEOUT(!state->property("heroTarget").value<QQuickItem *>(), 4000);

        // going back, it flies home to the card, which waits hidden for it
        QVERIFY(QMetaObject::invokeMethod(object.get(), "flyHome"));
        QCOMPARE(state->property("heroTarget").value<QQuickItem *>(), cover);
        QCOMPARE(cover->opacity(), 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(!state->property("heroTarget").value<QQuickItem *>(), 4000);
        QCOMPARE(cover->opacity(), 1.0);
    }

    void theSameListAgainKeepsItsRows()
    {
        AlbumListModel albums;
        Album first;
        first.id = QStringLiteral("a");
        Album second;
        second.id = QStringLiteral("b");
        albums.setItems({first, second});
        QSignalSpy resets(&albums, &QAbstractItemModel::modelReset);
        QSignalSpy refreshes(&albums, &QAbstractItemModel::dataChanged);
        // going back to a page loads its albums again: the rows stay built
        second.name = QStringLiteral("renamed");
        albums.setItems({first, second});
        QCOMPARE(resets.count(), 0);
        QCOMPARE(refreshes.count(), 1);
        QCOMPARE(albums.items().at(1).name, QStringLiteral("renamed"));
        // a different list is a new one
        albums.setItems({second});
        QCOMPARE(resets.count(), 1);
    }

    void revealedThingsMoveInAndOut()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "    property bool on: false\n"
                          "    property alias box: box\n"
                          "    Rectangle { id: box; width: 40; height: 40; Reveal.shown: parent.on }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *box = object->property("box").value<QQuickItem *>();
        QVERIFY(box);
        // the first value applies at once, so nothing animates as a view builds
        QVERIFY(!box->isVisible());

        object->setProperty("on", true);
        QVERIFY(box->isVisible());
        QVERIFY(box->opacity() < 0.5);
        QTRY_COMPARE(box->opacity(), 1.0);
        QTRY_COMPARE(box->scale(), 1.0);

        // leaving, it stays drawn until it has gone
        object->setProperty("on", false);
        QVERIFY(box->isVisible());
        QTRY_VERIFY(!box->isVisible());
        QCOMPARE(box->opacity(), 0.0);
    }

    void revealedThingsGrowWithReducedMotion_data()
    {
        QTest::addColumn<bool>("performance");
        QTest::newRow("reduced motion") << false;
        QTest::newRow("performance mode") << true;
    }

    /// reduced motion stills only what loops: a thing revealed still grows
    /// in as it fades. performance mode shows it at once
    void revealedThingsGrowWithReducedMotion()
    {
        QFETCH(bool, performance);
        Rig rig;
        rig.settings.setReduceMotion(!performance);
        rig.settings.setPerformanceMode(performance);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "    property bool on: false\n"
                          "    property alias box: box\n"
                          "    Rectangle { id: box; width: 40; height: 40; Reveal.shown: parent.on }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *box = object->property("box").value<QQuickItem *>();
        QVERIFY(box);

        object->setProperty("on", true);
        QVERIFY(box->isVisible());
        if (performance) {
            QCOMPARE(box->scale(), 1.0);
            QCOMPARE(box->opacity(), 1.0);
            return;
        }
        QVERIFY(box->opacity() < 0.5);
        QVERIFY(box->scale() < 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(box->opacity(), 1.0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(qFuzzyCompare(box->scale(), 1.0), 2000);
    }

    /// what turns with the music holds still with reduced motion
    void reducedMotionStillsTheSpin()
    {
        Rig rig;
        rig.settings.setReduceMotion(true);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "    readonly property real speed: UiState.spinSpeed\n"
                          "    Component.onCompleted: UiState.spinning = true\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        QTest::qWait(1200);
        QCOMPARE(object->property("speed").toReal(), 0.0);
        rig.settings.setReduceMotion(false);
        QTRY_VERIFY_WITH_TIMEOUT(object->property("speed").toReal() > 0.8, 2000);
    }

    /// with no shapes every shape is a plain circle, but one that keeps its
    /// shape, and no stickers are scattered behind the pages
    void noShapesDrawsCircles()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw with");
        }
        rig.window->setColor(Qt::white);
        const auto draw = [&](const QString &shape, bool keeps) {
            QQmlComponent component(&rig.engine);
            component.setData(QStringLiteral("import QtQuick\nimport Swiftify\nItem { width: 200; height: 200\n"
                                             "MaterialShape { x: 20; y: 20; width: 160; height: 160; color: \"black\"; "
                                             "shape: \"%1\"; keepsShape: %2 } }")
                                  .arg(shape, keeps ? QStringLiteral("true") : QStringLiteral("false"))
                                  .toUtf8(),
                              QUrl());
            std::unique_ptr<QObject> object(component.create());
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const QImage heart = draw(QStringLiteral("heart"), false);
        rig.settings.setNoShapes(true);
        const QImage plain = draw(QStringLiteral("heart"), false);
        const QImage circle = draw(QStringLiteral("circle"), false);
        const QImage kept = draw(QStringLiteral("heart"), true);
        QCOMPARE(plain, circle);
        QVERIFY(plain != heart);
        QCOMPARE(kept, heart);

        QQmlComponent stickers(&rig.engine);
        stickers.setData("import QtQuick\nimport Swiftify\nStickers { width: 400; height: 300; count: 4 }\n", QUrl());
        std::unique_ptr<QObject> scatter(stickers.create());
        QVERIFY2(scatter, qPrintable(stickers.errorString()));
        int shapes = 0;
        for (QQuickItem *child : qobject_cast<QQuickItem *>(scatter.get())->childItems()) {
            shapes += QString::fromLatin1(child->metaObject()->className()).contains(QLatin1StringView("MaterialShape"));
        }
        QCOMPARE(shapes, 0);
    }

    /// with no shapes the squiggles go straight: a wavy button is a plain
    /// pill, a wavy edge plain, a ring and a divider plain lines
    void noShapesStraightensTheSquiggles()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "    width: 400; height: 400\n"
                          "    property alias pill: pill\n"
                          "    property alias clip: clip\n"
                          "    property alias ring: ring\n"
                          "    property alias cover: cover\n"
                          "    WavyPill { id: pill; width: 120; height: 40; hovered: true }\n"
                          "    WavyClip { id: clip; y: 50; width: 100; height: 100; wavy: true }\n"
                          "    SquiggleRing { id: ring; y: 160; width: 80; height: 80; progress: 0.5 }\n"
                          "    ArtworkImage { id: cover; x: 200; width: 120; height: 120; circular: true }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        const auto part = [&](const char *name) { return object->property(name).value<QObject *>(); };
        QVERIFY(part("pill")->property("amplitude").toReal() > 0);
        QVERIFY(part("clip")->property("waviness").toReal() > 0);
        QVERIFY(part("ring")->property("waves").toReal() > 0);
        QVERIFY(part("cover")->property("showsSquiggle").toBool());

        rig.settings.setNoShapes(true);
        // the scallops spring flat
        QTRY_COMPARE_WITH_TIMEOUT(part("pill")->property("amplitude").toReal(), 0.0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(part("clip")->property("waviness").toReal(), 0.0, 3000);
        QCOMPARE(part("ring")->property("waves").toReal(), 0.0);
        QVERIFY(!part("cover")->property("showsSquiggle").toBool());

        // a divider draws as one with no wave at all
        if (!rig.gpu) {
            return;
        }
        rig.settings.setNoShapes(false);
        rig.window->setColor(Qt::white);
        const auto draw = [&](qreal amplitude) {
            QQmlComponent line(&rig.engine);
            line.setData(QStringLiteral("import QtQuick\nimport Swiftify\nWavyLine { x: 20; y: 20; width: 200; "
                                        "color: \"black\"; thickness: 2; amplitude: %1 }")
                             .arg(amplitude)
                             .toUtf8(),
                         QUrl());
            std::unique_ptr<QObject> drawn(line.create());
            auto *item = qobject_cast<QQuickItem *>(drawn.get());
            item->setParentItem(rig.window->contentItem());
            // the same height either way, so only the wave differs
            item->setHeight(12);
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const QImage straight = draw(0);
        const QImage wavy = draw(3);
        QVERIFY(wavy != straight);
        rig.settings.setNoShapes(true);
        QCOMPARE(draw(3), straight);
    }

    /// the pointer lying still lets what moves under it come to rest; moving
    /// it again wakes it
    void aRestingPointerStillsHoverMotion()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nDrift { active: true }\n", QUrl());
        std::unique_ptr<QObject> drift(component.create());
        QVERIFY2(drift, qPrintable(component.errorString()));

        QWindow window;
        const auto move = [&](QPointF where) {
            QMouseEvent event(QEvent::MouseMove, where, where, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &event);
        };
        move({10, 10});
        QVERIFY(!rig.app.isPointerResting());
        QTRY_VERIFY(drift->property("rate").toReal() > 0.9);

        // a frame's hover delivered again from the same place is not a move
        QTRY_VERIFY_WITH_TIMEOUT(rig.app.isPointerResting(), AppController::kPointerRestMs + 2000);
        move({10, 10});
        QVERIFY(rig.app.isPointerResting());
        QTRY_COMPARE(drift->property("running").toBool(), false);

        move({20, 14});
        QVERIFY(!rig.app.isPointerResting());
        QTRY_VERIFY(drift->property("running").toBool());
    }

    void theImmersiveRevealShowsTheMiddleFirst()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.window->setColor(Qt::transparent);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem { width: 300; height: 200\n"
                          "    Rectangle { anchors.fill: parent; color: \"red\"\n"
                          "        layer.enabled: true\n"
                          "        layer.effect: ShaderEffect {\n"
                          "            property variant source\n"
                          "            property size itemSize: Qt.size(width, height)\n"
                          "            property vector4d shape: Shapes.params(\"clover4\")\n"
                          "            property real reach: 60\n"
                          "            property real spin: 0\n"
                          "            fragmentShader: \"qrc:/shaders/shape_mask.frag.qsb\"\n"
                          "        }\n"
                          "    }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();
        const QImage frame = rig.render();
        // part open: the middle shows, the corners are still hidden
        QVERIFY(qAlpha(frame.pixel(150, 100)) > 200);
        QCOMPARE(qAlpha(frame.pixel(5, 5)), 0);
        QCOMPARE(qAlpha(frame.pixel(295, 195)), 0);
        // and the shape is a clover, not a disc: a leaf reaches further than
        // the gap between two leaves
        QVERIFY(qAlpha(frame.pixel(150 + 55, 100)) > 200);
        QCOMPARE(qAlpha(frame.pixel(150 + 39, 100 + 39)), 0);
    }

    void theShapeCatalogMatchesItsAtlas()
    {
        Rig rig;
        QObject *shapes = rig.engine.singletonInstance<QObject *>("Swiftify", "Shapes");
        QVERIFY(shapes);
        const QStringList names = shapes->property("names").toStringList();
        const QImage atlas(QStringLiteral(":/shapes/shape_atlas.png"));
        QVERIFY(!atlas.isNull());
        QCOMPARE(names.join(u','), atlas.text(QStringLiteral("names")));
        QCOMPARE(names.size(), 37);
    }

    void materialShapesDrawFromTheAtlas()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.window->setColor(Qt::transparent);
        // a 200 px shape with its top left at (10, 10); a point is given in
        // the shape's own unit square
        const auto draw = [&](const QString &shape, const QString &morphTo, qreal morph) {
            QQmlComponent component(&rig.engine);
            component.setData(QStringLiteral("import QtQuick\nimport Swiftify\n"
                                             "MaterialShape { x: 10; y: 10; width: 200; height: 200; color: \"white\"; "
                                             "shape: \"%1\"; morphTo: \"%2\"; morph: %3 }")
                                  .arg(shape, morphTo).arg(morph).toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const auto filled = [](const QImage &frame, qreal x, qreal y) {
            const qreal side = 200 - 1.5;
            return qAlpha(frame.pixel(qFloor(10 + 100 + (x - 0.5) * side), qFloor(10 + 100 + (y - 0.5) * side))) > 200;
        };

        // a heart: two lobes and a tip, and the notch between the lobes clear
        const QImage heart = draw(QStringLiteral("heart"), QStringLiteral("heart"), 0);
        QVERIFY(filled(heart, 0.25, 0.25));
        QVERIFY(filled(heart, 0.75, 0.25));
        QVERIFY(filled(heart, 0.5, 0.85));
        QVERIFY(!filled(heart, 0.5, 0.15));
        QVERIFY(!filled(heart, 0.1, 0.9));

        // the pixel circle's stepped corner reaches where a circle does not
        QVERIFY(filled(draw(QStringLiteral("pixelCircle"), QStringLiteral("pixelCircle"), 0), 0.915, 0.8));
        QVERIFY(!filled(draw(QStringLiteral("circle"), QStringLiteral("circle"), 0), 0.915, 0.8));

        // a cookie morphing into a heart fills the notch, then leaves it
        QVERIFY(filled(draw(QStringLiteral("cookie9"), QStringLiteral("heart"), 0), 0.5, 0.15));
        QVERIFY(!filled(draw(QStringLiteral("cookie9"), QStringLiteral("heart"), 1), 0.5, 0.15));
    }

    void aSoftShadowDrawsAsTheEffectDid_data()
    {
        QTest::addColumn<QString>("settings");
        QTest::newRow("card") << QStringLiteral("radius: 12; blur: 7; offset: Qt.vector2d(0, 3); color: Qt.rgba(0, 0, 0, 0.11)");
        QTest::newRow("hovered") << QStringLiteral("radius: 12; blur: 13; offset: Qt.vector2d(0, 6); color: Qt.rgba(0, 0, 0, 0.6)");
        QTest::newRow("glow") << QStringLiteral("radius: 30; blur: 24; offset: Qt.vector2d(-5.3, 2.9); color: Qt.rgba(0.4, 0.2, 0.9, 0.5)");
        QTest::newRow("round") << QStringLiteral("radius: 100; blur: 15; color: Qt.rgba(1, 0.5, 0, 0.8)");
    }

    /// SoftShadow stands in for qt quick effects' RectangularShadow, which
    /// was an effect and a draw of its own for every card: it should look the same
    void aSoftShadowDrawsAsTheEffectDid()
    {
        QFETCH(QString, settings);
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw with");
        }
        rig.window->setColor(Qt::transparent);
        const auto draw = [&](const QString &type) {
            QQmlComponent component(&rig.engine);
            component.setData(QStringLiteral("import QtQuick\nimport QtQuick.Effects\nimport Swiftify\n"
                                             "%1 { x: 60; y: 60; width: 200; height: 200; %2 }")
                                  .arg(type, settings).toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            if (!object) {
                qWarning().noquote() << component.errorString();
                return QImage();
            }
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            return rig.render();
        };
        const QImage effect = draw(QStringLiteral("RectangularShadow"));
        const QImage soft = draw(QStringLiteral("SoftShadow"));
        QVERIFY(!effect.isNull() && !soft.isNull());
        int worst = 0;
        qint64 total = 0;
        for (int y = 0; y < 320; ++y) {
            for (int x = 0; x < 320; ++x) {
                const int difference = std::abs(qAlpha(effect.pixel(x, y)) - qAlpha(soft.pixel(x, y)));
                worst = std::max(worst, difference);
                total += difference;
            }
        }
        QVERIFY2(worst <= 4, qPrintable(QStringLiteral("alpha differs by up to %1 (%2 in all)").arg(worst).arg(total)));
    }

    void aTurnedShapeKeepsItsCorners()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.window->setColor(Qt::transparent);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "MaterialShape { x: 100; y: 100; width: 200; height: 200; color: \"white\"; "
                          "shape: \"square\"; spinAngle: 45 }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();
        const QImage frame = rig.render();
        // turned an eighth, its corners point up, down and to the sides and
        // reach out past its item; a shader draws only in its item, so they
        // were cut off flat
        QVERIFY(qAlpha(frame.pixel(200, 200)) > 200);
        QVERIFY(qAlpha(frame.pixel(200, 90)) > 200);
        QVERIFY(qAlpha(frame.pixel(310, 200)) > 200);
        // while the item's own corners are now clear
        QCOMPARE(qAlpha(frame.pixel(108, 108)), 0);
    }

    void aHoverFadesInWithoutFlashingDark()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the button with");
        }
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Rectangle { width: 200; height: 200; color: \"white\"\n"
                          "    IconButton { x: 80; y: 80; colBackgroundHover: \"#e0e0e0\" } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();
        QCOMPARE(qRed(rig.render().pixel(86, 100)), 255);

        const QPointF inside(100, 100);
        QMouseEvent move(QEvent::MouseMove, inside, inside, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(rig.window.get(), &move);
        // all the way in, the state layer only ever darkens towards its
        // colour: easing from "transparent", black with no alpha, it went
        // through grey on the way and flashed dark
        int darkest = 255;
        for (int frame = 0; frame < 24; ++frame) {
            QTest::qWait(16);
            darkest = std::min(darkest, qRed(rig.render().pixel(86, 100)));
        }
        QVERIFY2(darkest >= 0xe0 - 2, qPrintable(QString::number(darkest, 16)));
        QVERIFY(std::abs(qRed(rig.render().pixel(86, 100)) - 0xe0) <= 2);
    }

    void aSongRowStaysOverItsListsHeader()
    {
        Rig rig;
        rig.playSomething();
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "ListView { width: 800; height: 400; model: Player.upNext\n"
                          "    header: Item { width: 800; height: 40 }\n"
                          "    delegate: SongRow { width: 800; songs: Player.upNext } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *list = qobject_cast<QQuickItem *>(object.get());
        list->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();
        QQuickItem *header = list->property("headerItem").value<QQuickItem *>();
        QVERIFY(header);
        QQuickItem *row = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, 0));
        QVERIFY(row);

        // hovered, the row grows over its neighbours; left again, it must
        // not sink under the header, whose stickers reach down behind it
        const auto moveTo = [&](QPointF point) {
            QMouseEvent move(QEvent::MouseMove, point, point, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(rig.window.get(), &move);
            Rig::settle();
        };
        moveTo(row->mapToScene(QPointF(400, row->height() / 2)));
        QTRY_VERIFY(row->property("hovered").toBool());
        moveTo(QPointF(1200, 700));
        QTRY_VERIFY(!row->property("hovered").toBool());
        // drawn after it: above it, or level with it and later among the
        // list's children, as a delegate made after the header is
        const QList<QQuickItem *> stack = row->parentItem()->childItems();
        QVERIFY2(row->z() > header->z() || (row->z() == header->z() && stack.indexOf(row) > stack.indexOf(header)),
                 qPrintable(QStringLiteral("row %1, header %2").arg(row->z()).arg(header->z())));
    }

    /// with no cover, the players wear the selected theme, whichever it is,
    /// never the pink of the swift app's accent
    void aSongWithoutACoverWearsTheTheme()
    {
        Rig rig;
        const auto expected = [&] {
            const MaterialScheme::Roles &roles = rig.theme.colors()->roles();
            const QColor primary = roles[std::size_t(MaterialScheme::Role::Primary)];
            return rig.theme.dark()
                ? primary
                : Theme::rolesFromSeed(primary, true, false)[std::size_t(MaterialScheme::Role::Primary)];
        };
        // a theme is picked a moment after it is asked for
        const auto pick = [&](const QString &theme) {
            const QColor before = expected();
            rig.settings.setThemeSource(theme);
            QTRY_VERIFY(expected() != before);
        };
        pick(QStringLiteral("gruvbox"));
        rig.playSomething();
        QVERIFY(rig.player.currentSong().artworkUrl.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(rig.style.primary(), expected(), 3000);
        QCOMPARE(rig.style.scheme()->primary(), rig.theme.colors()->primary());
        QVERIFY(rig.style.primary() != QColor::fromRgba(Theme::kAccent));

        // and follows another theme picked while it plays
        const QColor before = rig.style.primary();
        pick(QStringLiteral("rosepine"));
        QTRY_COMPARE_WITH_TIMEOUT(rig.style.primary(), expected(), 3000);
        QVERIFY(rig.style.primary() != before);
    }

    /// a menu is as wide as its longest entry, up to a point past which the
    /// entry ends in an ellipsis
    void aMenuIsAsWideAsItsLongestEntry()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { width: 1000; height: 700\n"
                          "    property alias menu: menu\n"
                          "    StyledMenu { id: menu\n"
                          "        StyledMenuItem { text: \"Short\" }\n"
                          "        StyledMenuItem { symbol: \"delete\"; text: \"Delete “A Playlist Name Long Enough To Be Cut Off”\" }\n"
                          "    } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        auto *menu = object->property("menu").value<QObject *>();
        QMetaObject::invokeMethod(menu, "popup");
        QTRY_VERIFY(menu->property("opened").toBool());
        QQuickItem *entry = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, entry), Q_ARG(int, 1));
        QVERIFY(entry);
        auto *content = entry->property("contentItem").value<QQuickItem *>();
        QVERIFY2(menu->property("width").toReal() > 240, qPrintable(QString::number(menu->property("width").toReal())));
        QTRY_VERIFY2(content->width() >= content->implicitWidth() - 0.5,
                     qPrintable(QStringLiteral("%1 of %2").arg(content->width()).arg(content->implicitWidth())));
    }

    /// the presets menu: what can be done with the sound first, the presets
    /// saved in a menu of their own, then the built-in ones after a divider,
    /// however often the list changes after the menu is built
    void thePresetMenuKeepsItsOrder()
    {
        Rig rig;
        const auto panel = rig.show(QStringLiteral("SoundPanel"));
        QVERIFY(panel);
        rig.sound.applyPreset(QStringLiteral("Bass Booster"));
        rig.sound.setGain(0, 7.5);
        rig.sound.savePreset(QStringLiteral("Mine"));
        Rig::settle();

        QObject *menu = nullptr;
        const auto texts = [](QObject *of) {
            QStringList texts;
            const int count = of->property("count").toInt();
            for (int index = 0; index < count; ++index) {
                QQuickItem *entry = nullptr;
                QMetaObject::invokeMethod(of, "itemAt", Q_RETURN_ARG(QQuickItem *, entry), Q_ARG(int, index));
                const QVariant text = entry ? entry->property("text") : QVariant();
                texts << (text.isValid() ? text.toString() : QStringLiteral("|"));
            }
            return texts;
        };
        for (QObject *candidate : panel->findChildren<QObject *>()) {
            if (QString::fromLatin1(candidate->metaObject()->className()).startsWith(QLatin1StringView("StyledMenu_QMLTYPE"))
                && texts(candidate).contains(QStringLiteral("Save Current…"))) {
                menu = candidate;
            }
        }
        QVERIFY(menu);
        const QStringList order = texts(menu);
        const qsizetype divider = order.indexOf(QStringLiteral("|"));
        QVERIFY2(divider > 0, qPrintable(order.join(u',')));
        for (const QString &top : {QStringLiteral("Save Current…"), QStringLiteral("Reset"), QStringLiteral("Custom Presets")}) {
            QVERIFY2(order.indexOf(top) >= 0 && order.indexOf(top) < divider, qPrintable(order.join(u',')));
        }
        QVERIFY2(!order.contains(QStringLiteral("Mine")), qPrintable(order.join(u',')));
        const QStringList builtIn = order.mid(divider + 1);
        QVERIFY2(builtIn.size() >= 5 && builtIn.first() == QStringLiteral("Flat"), qPrintable(order.join(u',')));

        QObject *saved = nullptr;
        for (QObject *candidate : panel->findChildren<QObject *>()) {
            if (candidate->property("title").toString() == QStringLiteral("Custom Presets")) {
                saved = candidate;
            }
        }
        QVERIFY(saved);
        QCOMPARE(texts(saved), QStringList {QStringLiteral("Mine")});
    }

    /// every way to a new playlist, a local one, a song's info or a deletion
    /// opens its dialog in the window itself
    void theWindowOpensItsEditors()
    {
        Rig rig;
        // the tray's menu is native, and the platform tests run on has none
        rig.settings.setTrayIcon(false);
        QQmlComponent component(&rig.engine, QUrl(QStringLiteral("qrc:/qt/qml/Swiftify/qml/Main.qml")));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        Rig::settle();
        QQmlComponent asker(&rig.engine);
        asker.setData("import QtQuick\nimport Swiftify\nQtObject {\n"
                      "    function newPlaylist() { UiState.beginCreatePlaylist() }\n"
                      "    function newLocalPlaylist() { UiState.createLocalPlaylistRequested(null) }\n"
                      "    function editTrack(target: song) { UiState.editLocalTrackRequested(target) }\n"
                      "    function settings() { UiState.settingsRequested() }\n"
                      "}",
                      QUrl());
        std::unique_ptr<QObject> ask(asker.create());
        QVERIFY2(ask, qPrintable(asker.errorString()));
        const auto opened = [&](const char *type) -> QObject * {
            for (QObject *candidate : window->findChildren<QObject *>()) {
                if (QString::fromLatin1(candidate->metaObject()->className()).startsWith(QLatin1StringView(type))
                    && candidate->property("opened").toBool()) {
                    return candidate;
                }
            }
            return nullptr;
        };

        QVERIFY(QMetaObject::invokeMethod(ask.get(), "newPlaylist"));
        QTRY_VERIFY(opened("PlaylistEditor_QMLTYPE"));
        QVERIFY(!opened("PlaylistEditor_QMLTYPE")->property("local").toBool());
        QMetaObject::invokeMethod(opened("PlaylistEditor_QMLTYPE"), "close");
        QTRY_VERIFY(!opened("PlaylistEditor_QMLTYPE"));

        QVERIFY(QMetaObject::invokeMethod(ask.get(), "newLocalPlaylist"));
        QTRY_VERIFY(opened("PlaylistEditor_QMLTYPE"));
        QVERIFY(opened("PlaylistEditor_QMLTYPE")->property("local").toBool());
        QMetaObject::invokeMethod(opened("PlaylistEditor_QMLTYPE"), "close");
        QTRY_VERIFY(!opened("PlaylistEditor_QMLTYPE"));

        Song song;
        song.name = QStringLiteral("Mine");
        song.uri = QStringLiteral("file:///music/mine.mp3");
        QVERIFY(QMetaObject::invokeMethod(ask.get(), "editTrack", Q_ARG(swiftify::Song, song)));
        QTRY_VERIFY(opened("LocalTrackEditor_QMLTYPE"));
        QMetaObject::invokeMethod(opened("LocalTrackEditor_QMLTYPE"), "close");
        QTRY_VERIFY(!opened("LocalTrackEditor_QMLTYPE"));

        // the settings open over the window, not in one of their own
        const qsizetype windows = QGuiApplication::topLevelWindows().size();
        QVERIFY(QMetaObject::invokeMethod(ask.get(), "settings"));
        QTRY_VERIFY(opened("SettingsDialog_QMLTYPE"));
        QCOMPARE(QGuiApplication::topLevelWindows().size(), windows);
        // asked for again, still the one
        QVERIFY(QMetaObject::invokeMethod(ask.get(), "settings"));
        Rig::settle();
        int open = 0;
        for (QObject *candidate : window->findChildren<QObject *>()) {
            open += QString::fromLatin1(candidate->metaObject()->className()).startsWith(QLatin1StringView("SettingsDialog_QMLTYPE"));
        }
        QCOMPARE(open, 1);
        QMetaObject::invokeMethod(opened("SettingsDialog_QMLTYPE"), "close");
        QTRY_VERIFY(!opened("SettingsDialog_QMLTYPE"));
    }

    /// the continue card's title keeps to one line, however long the name
    void theContinueCardKeepsItsTitleToOneLine()
    {
        Rig rig;
        Song song;
        song.name = QStringLiteral("A Song Whose Name Goes On And On Well Past The Width Of Any Card It Could Be Shown In");
        song.artists = QStringLiteral("Artist");
        song.uri = QUrl::fromLocalFile(QStringLiteral("/music/long.flac")).toString();
        rig.player.play(song, {song});
        Rig::settle();
        rig.core.take(QStringLiteral("playFile"))->succeed();
        Rig::settle();
        const auto page = rig.show(QStringLiteral("HomeView"));
        QVERIFY(page);
        const std::function<QQuickItem *(QQuickItem *)> title = [&](QQuickItem *item) -> QQuickItem * {
            if (item->property("text").toString() == song.name) {
                return item;
            }
            for (QQuickItem *child : item->childItems()) {
                if (QQuickItem *found = title(child)) {
                    return found;
                }
            }
            return nullptr;
        };
        QTRY_VERIFY(title(page.get()));
        QQuickItem *shown = title(page.get());
        QCOMPARE(shown->property("lineCount").toInt(), 1);
        QVERIFY(shown->property("truncated").toBool());
    }

    /// connecting for the first time: the setup stays through the library's
    /// login until playback's is done too, and only then gives way
    void theSetupWaitsForPlaybackToo()
    {
        Rig rig;
        TokenServer tokens;
        Browser browser;
        OAuthFlow::setTokenEndpoint(tokens.url());
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "open");
        const auto restore = qScopeGuard([] {
            OAuthFlow::setTokenEndpoint(QUrl(QStringLiteral("https://accounts.spotify.com/api/token")));
            QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        });
        // nothing to load from spotify itself here
        rig.session.setInitialLibraryLoader({});
        rig.session.restoreSavedSessions();
        QTRY_VERIFY(!rig.session.isEvaluatingStoredLogin());

        const auto host = rig.show(QStringLiteral("PageHost"));
        QVERIFY(host);
        const auto page = [&] {
            const QQuickItem *current = host->property("currentPage").value<QQuickItem *>();
            return current ? QString::fromLatin1(current->metaObject()->className()) : QString();
        };
        QTRY_VERIFY2(page().startsWith(QLatin1StringView("SetupView")), qPrintable(page()));

        rig.session.setLibraryClientId(QStringLiteral("client"));
        rig.session.beginLibraryLogin();
        QTRY_COMPARE(browser.opened.size(), 1);
        browser.approve();
        QTRY_VERIFY(rig.session.isLibraryConnected());
        // still setting up: playback is not
        QTest::qWait(200);
        QVERIFY2(page().startsWith(QLatin1StringView("SetupView")), qPrintable(page()));

        rig.session.beginPlaybackLogin();
        QTRY_COMPARE(browser.opened.size(), 2);
        browser.approve();
        std::shared_ptr<swiftify::testing::PendingCall> connect;
        QTRY_VERIFY(std::any_of(rig.core.pending.cbegin(), rig.core.pending.cend(),
                                [](const auto &call) { return call->name == QLatin1StringView("connect"); }));
        rig.core.take(QStringLiteral("connect"))->succeed(QStringLiteral("login"));
        QTRY_VERIFY(rig.session.isConnected());
        // both: the library at last
        QTRY_VERIFY2(!page().startsWith(QLatin1StringView("SetupView")), qPrintable(page()));
    }

    /// a launch that brings the library back while playback is still on its
    /// way, or not coming, opens on the library, not the setup
    void aLaunchWithTheLibraryShowsTheLibrary()
    {
        Rig rig;
        rig.session.setInitialLibraryLoader({});
        rig.credentials.items.insert(CredentialStore::Account::Library,
                                     {QStringLiteral("client"), QStringLiteral("refresh"),
                                      OAuthFlow::kLibraryAuthorizationVersion, {}, QStringLiteral("access"),
                                      QDateTime::currentDateTimeUtc().addSecs(1800)});
        rig.session.restoreSavedSessions();
        QTRY_VERIFY(rig.session.isLibraryConnected());
        QVERIFY(!rig.session.isConnected());
        const auto host = rig.show(QStringLiteral("PageHost"));
        QVERIFY(host);
        const QQuickItem *current = host->property("currentPage").value<QQuickItem *>();
        QVERIFY(current);
        QVERIFY2(!QString::fromLatin1(current->metaObject()->className()).startsWith(QLatin1StringView("SetupView")),
                 current->metaObject()->className());
    }

    /// a menu is built the first time it opens, and opens then
    void aMenuOpensTheFirstTimeItIsAsked()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { width: 400; height: 300\n"
                          "    property alias atAnchor: atAnchor\n"
                          "    property alias atPointer: atPointer\n"
                          "    MenuLoader { id: atAnchor; sourceComponent: StyledMenu { StyledMenuItem { text: \"One\" } } }\n"
                          "    MenuLoader { id: atPointer; sourceComponent: StyledMenu { StyledMenuItem { text: \"Two\" } } } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *root = qobject_cast<QQuickItem *>(object.get());
        root->setParentItem(rig.window->contentItem());
        auto *atAnchor = object->property("atAnchor").value<QObject *>();
        auto *atPointer = object->property("atPointer").value<QObject *>();
        QVERIFY(!atAnchor->property("item").value<QObject *>());

        QVERIFY(QMetaObject::invokeMethod(atAnchor, "popupAt", Q_ARG(QQuickItem *, root), Q_ARG(double, 10.0),
                                          Q_ARG(double, 20.0)));
        auto *menu = atAnchor->property("item").value<QObject *>();
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(atPointer, "popupHere"));
        QVERIFY(atPointer->property("item").value<QObject *>());
        QTRY_VERIFY(atPointer->property("item").value<QObject *>()->property("opened").toBool());
    }

    /// the playing row's mark and meter are built for it alone and let go of
    /// once they have shrunk away, without a binding loop on the way
    void aSongRowLetsGoOfItsMarkOnceItShrinks()
    {
        Rig rig;
        rig.playSomething();
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { width: 800; height: 60\n"
                          "    property alias row: row\n"
                          "    property song playing\n"
                          "    SongRow { id: row; width: 800; index: 0; song: parent.playing } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        object->setProperty("playing", QVariant::fromValue(rig.player.currentSong()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        auto *row = object->property("row").value<QQuickItem *>();
        const auto loaded = [&] {
            int count = 0;
            const std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
                for (QQuickItem *child : item->childItems()) {
                    // a loader with properties of its own is a type of its own
                    if (QString::fromLatin1(child->metaObject()->className()).startsWith(QLatin1StringView("QQuickLoader"))) {
                        count += child->property("active").toBool();
                    }
                    walk(child);
                }
            };
            walk(row);
            return count;
        };
        QVERIFY(row->property("isCurrent").toBool());
        rig.settings.setVisualizersEnabled(true);
        QTRY_COMPARE(loaded(), 2);

        // the next song: this row's mark and meter shrink away and go
        rig.player.skipForward();
        QTRY_VERIFY(std::any_of(rig.core.pending.cbegin(), rig.core.pending.cend(),
                                [](const auto &call) { return call->name == QLatin1StringView("playFile"); }));
        rig.core.take(QStringLiteral("playFile"))->succeed();
        QTRY_VERIFY(!row->property("isCurrent").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(loaded(), 0, 3000);
    }

    void theMetersDrawEveryFrameWhileMusicPlays()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("counting frames needs the offscreen gpu rig");
        }
        rig.playSomething();
        const auto meter = rig.show(QStringLiteral("CompactMeter"));
        QVERIFY(meter);
        meter->setProperty("active", true);
        const QList<float> steady(48, 0.5f);
        rig.spectrum.publish(steady.constData(), 48);
        int drawn = 0;
        QObject::connect(rig.control.get(), &QQuickRenderControl::sceneChanged, meter.get(), [&] { ++drawn; });
        const auto frames = [&](int count) {
            drawn = 0;
            for (int frame = 0; frame < count; ++frame) {
                QTest::qWait(16);
                rig.render();
            }
            return drawn;
        };
        // long enough for its bars to settle on levels that hold still
        frames(90);
        QVERIFY(!qobject_cast<SpectrumItem *>(meter.get())->isMoving());
        // and still drawn every frame, as long as music plays
        QVERIFY2(frames(10) >= 10, qPrintable(QString::number(drawn)));
        // paused, its bars fall, and once they rest nothing more is drawn
        meter->setProperty("active", false);
        frames(120);
        QCOMPARE(frames(10), 0);
    }

    void aRingStandsUprightAboutItsMirror()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "SpectrumView { kind: SpectrumView.Ring; active: true; width: 400; height: 400 }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();
        // loud bass, quiet above: the tallest bars stand either side of the top
        QList<float> levels(48, 0.1f);
        std::fill_n(levels.begin(), 6, 1.0f);
        rig.spectrum.publish(levels.constData(), int(levels.size()));
        for (int frame = 0; frame < 60; ++frame) {
            QTest::qWait(16);
            rig.render();
        }
        const QImage frame = rig.render();
        const int side = int(400 * qreal(frame.width()) / rig.window->width());
        const QColor background = frame.pixelColor(side + 4, 4);
        const auto ink = [&](QColor pixel) {
            return std::abs(pixel.redF() - background.redF()) + std::abs(pixel.greenF() - background.greenF())
                 + std::abs(pixel.blueF() - background.blueF());
        };
        // across the top half, where the tall bars stand, what is drawn
        // centres on the middle: a ring turned half a bar's step leans aside
        double weighted = 0;
        double total = 0;
        for (int y = 0; y < side / 2; ++y) {
            for (int x = 0; x < side; ++x) {
                const double amount = ink(frame.pixelColor(x, y));
                weighted += amount * (x + 0.5);
                total += amount;
            }
        }
        QVERIFY(total > 0);
        const double lean = weighted / total - side / 2.0;
        QVERIFY2(std::abs(lean) < 0.5, qPrintable(QStringLiteral("leans %1 px").arg(lean)));
    }

    void aQuitIsToldApartFromAClosedWindow()
    {
        Rig rig;
        // closing a window (cmd-w, its close button) is not quitting: the
        // main window hides on it and the app runs on
        QQuickWindow other;
        other.show();
        Rig::settle();
        other.close();
        Rig::settle();
        QVERIFY(!rig.app.isQuitting());
        // a quit, however it was asked for, comes to the app first; the main
        // window then lets itself be closed rather than hiding
        QSignalSpy quitting(&rig.app, &AppController::quittingChanged);
        QEvent quit(QEvent::Quit);
        QCoreApplication::sendEvent(qApp, &quit);
        QVERIFY(rig.app.isQuitting());
        QCOMPARE(quitting.count(), 1);
    }

    void shapesAreCutToTheRoundedCardTheyAreIn()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.window->setColor(Qt::transparent);
        // a red circle far bigger than the 100 px card it sits in, over its
        // top left corner, as a genre card's shape is over its top right
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { x: 40; y: 40; width: 100; height: 100\n"
                          "  MaterialShape { x: -80; y: -80; width: 240; height: 240; shape: \"circle\"; color: \"red\";"
                          " parentClipRadius: 24 } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();
        const QImage frame = rig.render();
        const qreal ratio = qreal(frame.width()) / rig.window->width();
        const auto alpha = [&](qreal x, qreal y) { return qAlpha(frame.pixel((QPointF(x, y) * ratio).toPoint())); };
        QVERIFY2(alpha(90, 90) > 200, "the shape is not drawn inside the card");
        QVERIFY2(alpha(90, 41) > 200, "the shape is cut short of the card's straight edge");
        // the card's corner is rounded off, and nothing shows past its edges
        QCOMPARE(alpha(42, 42), 0);
        QCOMPARE(alpha(90, 30), 0);
        QCOMPARE(alpha(30, 90), 0);
    }

    void cornerCapsCoverWhatReachesPastARoundedCorner()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw with");
        }
        rig.window->setColor(Qt::transparent);
        // a page filling a rounded sheet edge to edge, as a sticker does
        // at the sheet's corner, capped in blue
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { x: 40; y: 40; width: 200; height: 120\n"
                          "  Rectangle { anchors.fill: parent; color: \"red\" }\n"
                          "  CornerCaps { anchors.fill: parent; radius: 24; color: \"blue\" } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();
        const QImage frame = rig.render();
        const qreal ratio = qreal(frame.width()) / rig.window->width();
        const auto at = [&](qreal x, qreal y) { return frame.pixel((QPointF(x, y) * ratio).toPoint()); };
        const auto blue = [](QRgb pixel) { return qBlue(pixel) > 200 && qRed(pixel) < 40; };
        const auto red = [](QRgb pixel) { return qRed(pixel) > 200 && qBlue(pixel) < 40; };
        // every corner's tip is capped
        for (const QPointF corner : {QPointF(41, 41), QPointF(239, 41), QPointF(41, 159), QPointF(239, 159)}) {
            QVERIFY2(blue(at(corner.x(), corner.y())), qPrintable(QStringLiteral("%1, %2").arg(corner.x()).arg(corner.y())));
        }
        // and nothing inside the rounding is: past the arc, along the edges
        // and in the middle
        QVERIFY(red(at(52, 52)));
        QVERIFY(red(at(140, 41)));
        QVERIFY(red(at(41, 100)));
        QVERIFY(red(at(140, 100)));
        // nor anything outside the sheet
        QCOMPARE(qAlpha(at(30, 30)), 0);
    }

    void titlesAreDrawnRound()
    {
        Rig rig;
        QVERIFY2(QFontDatabase::families().contains(QStringLiteral("Google Sans Flex")),
                 "the build's google sans flex did not load");
        const auto title = rig.show(QStringLiteral("TitleText"));
        QVERIFY(title);
        const QFont font = title->property("font").value<QFont>();
        QCOMPARE(font.family(), QStringLiteral("Google Sans Flex"));
        QCOMPARE(font.variableAxisValue(QFont::Tag("ROND")), 100.0f);
        QVERIFY(font.variableAxisValue(QFont::Tag("wdth")) > 100.0f);
    }

    void stickersKeepTheirPlacesAndHoldStill()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem { width: 600; height: 300\n"
                          "  property alias first: first; property alias again: again; property alias other: other\n"
                          "  Stickers { id: first; anchors.fill: parent; key: \"home\" }\n"
                          "  Stickers { id: again; anchors.fill: parent; key: \"home\" }\n"
                          "  Stickers { id: other; anchors.fill: parent; key: \"search\" } }",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
        const auto layoutOf = [&](const char *name) {
            const QVariant layout = object->property(name).value<QObject *>()->property("layout");
            return QJsonDocument::fromVariant(layout.value<QJSValue>().toVariant()).toJson();
        };
        // a page keeps its own scatter, and another page has another
        QCOMPARE(layoutOf("first"), layoutOf("again"));
        QVERIFY(layoutOf("first") != layoutOf("other"));

        // with nothing playing, nothing turns
        QQuickItem *sticker = object->property("first").value<QQuickItem *>()->childItems().value(0);
        QVERIFY(sticker);
        const qreal angle = sticker->property("spinAngle").toReal();
        QTest::qWait(300);
        QCOMPARE(sticker->property("spinAngle").toReal(), angle);
    }

    void aBurstFliesOutAndTakesItselfAway()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nShapeBurst { x: 100; y: 100; kind: \"hearts\" }", QUrl());
        QPointer<QQuickItem> burst(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(burst, qPrintable(component.errorString()));
        // as the window's host makes them, from qml, so it may destroy itself
        QQmlEngine::setObjectOwnership(burst, QQmlEngine::JavaScriptOwnership);
        burst->setParentItem(rig.window->contentItem());
        // ten hearts, setting off
        int hearts = 0;
        for (QQuickItem *piece : burst->childItems()) {
            hearts += piece->property("shape").toString() == QLatin1StringView("heart") ? 1 : 0;
        }
        QCOMPARE(hearts, 10);
        // gone within a second, nothing left running
        QTRY_VERIFY_WITH_TIMEOUT((QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete), !burst), 1500);
    }

    void viewsHoldStillAtRest_data()
    {
        QTest::addColumn<QString>("type");
        for (const char *type : {"HomeView", "SearchView", "PlaylistsView", "SavedSongsView", "LocalFilesView", "LocalTracksView", "Sidebar",
                                 "PlayerBar", "SettingsPages", "EmptyState", "Stickers", "ShapeIcon"}) {
            QTest::addRow("%s", type) << QString::fromLatin1(type);
        }
    }

    // docs/perf.md: nothing animates at rest. with nothing playing, once a
    // view's entrance has settled it asks for no more frames
    void viewsHoldStillAtRest()
    {
        QFETCH(QString, type);
        Rig rig;
        if (!rig.gpu) {
            QSKIP("needs the render control to count the frames asked for");
        }
        const auto view = rig.show(type);
        QVERIFY(view);
        // let the reveals and springs finish
        for (int frame = 0; frame < 90; ++frame) {
            QTest::qWait(16);
            rig.render();
        }
        int asked = 0;
        const auto counter = [&asked] { ++asked; };
        const auto a = QObject::connect(rig.control.get(), &QQuickRenderControl::renderRequested, counter);
        const auto b = QObject::connect(rig.control.get(), &QQuickRenderControl::sceneChanged, counter);
        QTest::qWait(600);
        QObject::disconnect(a);
        QObject::disconnect(b);
        QVERIFY2(asked == 0, qPrintable(QStringLiteral("%1 asked for %2 frames at rest").arg(type).arg(asked)));
    }

    void aShapeIconMorphsIntoANewShape()
    {
        Rig rig;
        const auto icon = rig.show(QStringLiteral("ShapeIcon"));
        QVERIFY(icon);
        QCOMPARE(icon->property("progress").toReal(), 1.0);
        icon->setProperty("shape", QStringLiteral("heart"));
        // it sets off from the cookie it was
        QCOMPARE(icon->property("fromShape").toString(), QStringLiteral("cookie9"));
        QVERIFY(icon->property("progress").toReal() < 1);
        QTRY_COMPARE(icon->property("progress").toReal(), 1.0);
        QCOMPARE(icon->property("target").toString(), QStringLiteral("heart"));
    }

    void aWavyCoverScallopsItsEdge()
    {
        Rig rig;
        if (!rig.gpu) {
            QSKIP("no gpu to draw the shaders with");
        }
        rig.window->setColor(Qt::transparent);
        const auto edgeRow = [&](qreal waviness) {
            QQmlComponent component(&rig.engine);
            component.setData(QStringLiteral("import QtQuick\nItem { width: 200; height: 200\n"
                                             "Rectangle { id: art; width: 160; height: 160; color: \"red\"; visible: false; layer.enabled: true }\n"
                                             "ShaderEffect { x: 20; y: 20; width: 160; height: 160\n"
                                             "    property variant source: art\n"
                                             "    property size itemSize: Qt.size(width, height)\n"
                                             "    property real radius: 12\n"
                                             "    property real borderWidth: 0\n"
                                             "    property color borderColor: \"transparent\"\n"
                                             "    property size uvScale: Qt.size(1, 1)\n"
                                             "    property point uvOffset: Qt.point(0, 0)\n"
                                             "    property real waviness: %1\n"
                                             "    property real wavelength: 20\n"
                                             "    property real phase: 0\n"
                                             "    property real ringWaves: 0\n"
                                             "    property real ringAmplitude: 0\n"
                                             "    property real ringThickness: 0\n"
                                             "    property real ringPhase: 0\n"
                                             "    property color ringColor: \"transparent\"\n"
                                             "    property real reveal: 1\n"
                                             "    fragmentShader: \"qrc:/shaders/rounded_image.frag.qsb\" } }")
                                  .arg(waviness).toUtf8(), QUrl());
            std::unique_ptr<QObject> object(component.create());
            qobject_cast<QQuickItem *>(object.get())->setParentItem(rig.window->contentItem());
            Rig::settle();
            rig.render();
            const QImage frame = rig.render();
            // a line just inside the top edge, clear of the corners
            QList<bool> drawn;
            for (int x = 50; x < 150; ++x) {
                drawn.append(qAlpha(frame.pixel(x, 22)) > 128);
            }
            return drawn;
        };
        const QList<bool> plain = edgeRow(0);
        QVERIFY(!plain.contains(false));
        // scalloped, the line runs through peaks and valleys by turns
        const QList<bool> wavy = edgeRow(4);
        int changes = 0;
        for (qsizetype x = 1; x < wavy.size(); ++x) {
            changes += wavy.at(x) != wavy.at(x - 1);
        }
        QVERIFY2(changes >= 6, qPrintable(QString::number(changes)));
    }

    /// home's shelves down the page keep their places but not their cards:
    /// every frame drew each card of every shelf, and hidden ones still held
    /// their covers decoded
    void homeBuildsOnlyTheShelvesInSight()
    {
        Rig rig;
        QList<Playlist> playlists;
        QList<Song> songs;
        QList<Artist> artists;
        QList<Album> albums;
        for (int index = 0; index < 12; ++index) {
            Playlist playlist;
            playlist.id = QStringLiteral("p%1").arg(index);
            playlist.name = QStringLiteral("Playlist %1").arg(index);
            playlists.append(playlist);
            Song song;
            song.instanceId = quint64(100 + index);
            song.name = QStringLiteral("Song %1").arg(index);
            song.uri = QStringLiteral("spotify:track:s%1").arg(index);
            songs.append(song);
            Artist artist;
            artist.id = QStringLiteral("a%1").arg(index);
            artist.name = QStringLiteral("Artist %1").arg(index);
            artists.append(artist);
            Album album;
            album.id = QStringLiteral("al%1").arg(index);
            album.name = QStringLiteral("Album %1").arg(index);
            albums.append(album);
        }
        rig.library.playlists()->setItems(playlists);
        rig.library.pinnedPlaylists()->setItems(playlists);
        rig.library.recentlyPlayed()->setSongs(songs);
        rig.library.recommendedArtists()->setItems(artists);
        rig.library.recommendedAlbums()->setItems(albums);
        const auto page = rig.show(QStringLiteral("HomeView"));
        QVERIFY(page);
        const std::function<int(QQuickItem *)> countCardsIn = [&](QQuickItem *item) {
            int cards = QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1StringView("MediaCard_QMLTYPE"));
            for (QQuickItem *child : item->childItems()) {
                cards += countCardsIn(child);
            }
            return cards;
        };
        // five shelves of covers, seven cards to a row this wide: the three
        // in or near sight are built, the other two are not
        QTRY_VERIFY(countCardsIn(page.get()) > 0);
        QVERIFY2(countCardsIn(page.get()) <= 24, qPrintable(QString::number(countCardsIn(page.get()))));
    }

    /// an artist's albums are built only near the screen: a long
    /// discography had every card built and every cover decoded at once
    void aLongDiscographyBuildsOnlyWhatIsInSight()
    {
        Rig rig;
        QList<Album> albums;
        for (int index = 0; index < 60; ++index) {
            Album album;
            album.id = QStringLiteral("album%1").arg(index);
            album.name = QStringLiteral("Album %1").arg(index);
            album.releaseDate = QStringLiteral("2020-01-01");
            albums.append(album);
        }
        rig.library.artistAlbums()->setItems(albums);
        const auto page = rig.show(QStringLiteral("ArtistView"));
        QVERIFY(page);
        // repeated items belong to their repeater, not to the page: they are
        // found down the visual tree
        const std::function<int(QQuickItem *)> countCardsIn = [&](QQuickItem *item) {
            int cards = QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1StringView("MediaCard_QMLTYPE"));
            for (QQuickItem *child : item->childItems()) {
                cards += countCardsIn(child);
            }
            return cards;
        };
        const auto countCards = [&] { return countCardsIn(page.get()); };
        // built a few frames' worth at a time
        QTRY_VERIFY(countCards() > 0);
        QVERIFY2(countCards() < 40, qPrintable(QString::number(countCards())));

        // scrolled to the end, the first ones are let go of and the last built
        page->setProperty("contentY", page->property("contentHeight").toReal() - page->height());
        QTRY_VERIFY(countCards() > 0);
        QVERIFY2(countCards() < 40, qPrintable(QString::number(countCards())));
    }

    void aPlaylistMadeHereShowsWithTheLocalFiles()
    {
        Rig rig;
        const Playlist made = rig.localFiles.createPlaylist(QStringLiteral("Mine"), {}, {}, {});
        QVERIFY(made.isValid());
        const std::function<QQuickItem *(QQuickItem *, const std::function<bool(QQuickItem *)> &)> find =
            [&](QQuickItem *item, const std::function<bool(QQuickItem *)> &wanted) -> QQuickItem * {
            if (wanted(item)) {
                return item;
            }
            for (QQuickItem *child : item->childItems()) {
                if (QQuickItem *found = find(child, wanted)) {
                    return found;
                }
            }
            return nullptr;
        };
        const auto isA = [](QQuickItem *item, const char *type) {
            return QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1StringView(type));
        };

        // Local Files is a grid: a new playlist's tile, every file's card,
        // then the playlists
        {
            const auto page = rig.show(QStringLiteral("LocalFilesView"));
            QVERIFY(page);
            const auto card = [&](const QString &title) {
                return find(page.get(), [&](QQuickItem *item) {
                    return isA(item, "MediaCard_QMLTYPE") && item->property("title").toString() == title
                        && item->isVisible();
                });
            };
            QTRY_VERIFY(card(QStringLiteral("Mine")));
            QVERIFY(card(QStringLiteral("Local Tracks")));
            QVERIFY(find(page.get(), [&](QQuickItem *item) {
                return item->property("text").toString() == QStringLiteral("New Playlist") && item->isVisible();
            }));
            QCOMPARE(page->property("count").toInt(), 3);
        }

        // opened, it is a playlist with nothing to download or share
        rig.library.selectPlaylist(made);
        const auto page = rig.show(QStringLiteral("PlaylistView"));
        QVERIFY(page);
        QTRY_VERIFY(find(page.get(), [&](QQuickItem *item) {
            return item->property("kind").toString() == QStringLiteral("LOCAL PLAYLIST");
        }));
        QVERIFY(!find(page.get(), [&](QQuickItem *item) {
            return item->property("symbol").toString() == QStringLiteral("download") && item->isVisible();
        }));
        // and takes tracks from anywhere
        QVERIFY(find(page.get(), [&](QQuickItem *item) {
            return item->property("text").toString() == QStringLiteral("Add Tracks") && item->isVisible();
        }));

        // and its editor has no public switch
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\nItem {\n"
                          "    property alias editor: editor\n"
                          "    PlaylistEditor { id: editor }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *holder = qobject_cast<QQuickItem *>(object.get());
        holder->setParentItem(rig.window->contentItem());
        auto *editor = object->property("editor").value<QObject *>();
        const auto publicRow = [&] {
            return find(rig.window->contentItem(), [&](QQuickItem *item) {
                return item->property("text").toString() == QStringLiteral("Public playlist");
            });
        };
        QMetaObject::invokeMethod(editor, "openForCreateLocal", Q_ARG(QVariant, QVariant()));
        QTRY_VERIFY(editor->property("opened").toBool());
        QVERIFY(publicRow());
        QVERIFY(!publicRow()->isVisible());
        QMetaObject::invokeMethod(editor, "close");
        QTRY_VERIFY(!editor->property("visible").toBool());
        QMetaObject::invokeMethod(editor, "openForCreate");
        QTRY_VERIFY(editor->property("opened").toBool());
        QVERIFY(publicRow()->isVisible());
    }

    void aNarrowWindowClosesItsRightPanels()
    {
        Rig rig;
        rig.settings.setTrayIcon(false);
        rig.settings.setPerformanceMode(true);
        QQmlComponent component(&rig.engine, QUrl(QStringLiteral("qrc:/qt/qml/Swiftify/qml/Main.qml")));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        QCOMPARE(window->minimumWidth(), 526);
        QCOMPARE(window->minimumHeight(), 410);
        window->setWidth(1000);
        Rig::settle();
        QVERIFY(window->property("rightPanelFits").toBool());
        rig.player.setShowingLyrics(true);
        QVERIFY(rig.player.isShowingLyrics());
        // 980 - 232 sidebar - 320 inspector - 32 gutters = 396 points.
        window->setWidth(980);
        Rig::settle();
        QVERIFY(!rig.player.isShowingLyrics());
        QVERIFY(!rig.player.isShowingQueue());
        window->setWidth(526);
        rig.player.setShowingQueue(true);
        Rig::settle();
        QVERIFY(!rig.player.isShowingQueue());
        window->setWidth(1000);
        Rig::settle();
        QVERIFY(!rig.player.isShowingLyrics());
        QVERIFY(!rig.player.isShowingQueue());
    }

    void narrowShelfHeadingsKeepTheirActionsClear()
    {
        Rig rig;
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "SectionHeader { title: 'Pinned Playlists'; symbol: 'push_pin'; showsSeeAll: true }", QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *header = qobject_cast<QQuickItem *>(object.get());
        header->setParentItem(rig.window->contentItem());
        auto *title = header->findChild<QQuickItem *>(QStringLiteral("sectionTitle"));
        auto *action = header->findChild<QQuickItem *>(QStringLiteral("sectionSeeAll"));
        QVERIFY(title && action);
        for (const int width : {500, 340, 280, 240}) {
            header->setWidth(width);
            Rig::settle();
            rig.render();
            QVERIFY(title->width() > 0);
            QVERIFY(title->x() + title->width() <= action->x());
            QVERIFY(action->x() + action->width() <= header->width());
            if (width <= 280) QVERIFY(action->property("iconOnly").toBool());
        }
    }

    void narrowPlayerControlsNeverOverlapTheSong()
    {
        Rig rig;
        rig.playSomething();
        const auto bar = rig.show(QStringLiteral("PlayerBar"));
        QVERIFY(bar);
        auto *transport = bar->findChild<QQuickItem *>(QStringLiteral("playerTransport"));
        auto *song = bar->findChild<QQuickItem *>(QStringLiteral("playerNowPlaying"));
        auto *utilities = bar->findChild<QQuickItem *>(QStringLiteral("playerUtilities"));
        QVERIFY(transport && song && utilities);
        for (const int width : {720, 560, 480, 400, 354, 319, 280}) {
            bar->setWidth(width);
            Rig::settle();
            rig.render();
            QVERIFY(song->width() >= 0);
            QVERIFY(transport->x() + transport->width() <= song->x());
            QVERIFY(song->x() + song->width() <= utilities->x());
            QVERIFY(utilities->x() + utilities->width() <= bar->width());
        }
    }

    void focusedSearchOverlaysStationaryPlaylistActions()
    {
        Rig rig;
        rig.settings.setPerformanceMode(true);
        const Playlist playlist = rig.localFiles.createPlaylist(QStringLiteral("Mine"), {}, {}, {});
        rig.library.selectPlaylist(playlist);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { width: 355; height: 160\n"
                          "Window { id: host; visible: false }\n"
                          "Rectangle { id: page; anchors.fill: parent; color: 'green' }\n"
                          "TopBar { objectName: 'bar'; width: parent.width; window: host; backdropSource: page } }", QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *holder = qobject_cast<QQuickItem *>(object.get());
        holder->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();
        auto *bar = holder->findChild<QQuickItem *>(QStringLiteral("bar"));
        auto *actions = holder->findChild<QQuickItem *>(QStringLiteral("playlistActions"));
        auto *search = holder->findChild<QQuickItem *>(QStringLiteral("toolbarSearch"));
        QVERIFY(bar && actions && search);
        const qreal actionX = actions->x();
        const qreal actionWidth = actions->width();
        const qreal collapsedWidth = search->width();
        QVERIFY(QMetaObject::invokeMethod(bar, "focusSearch"));
        Rig::settle();
        rig.render();
        QVERIFY(search->property("editing").toBool());
        QCOMPARE(actions->x(), actionX);
        QCOMPARE(actions->width(), actionWidth);
        QVERIFY(search->width() > collapsedWidth);
        QVERIFY(search->x() < actions->x() + actions->width());
        QVERIFY(search->z() > actions->z());
    }

    void narrowCollectionActionsStayWithinTheirHero()
    {
        Rig rig;
        const Playlist playlist = rig.localFiles.createPlaylist(QStringLiteral("A playlist with a long title"), {}, {}, {});
        rig.library.selectPlaylist(playlist);
        const auto page = rig.show(QStringLiteral("PlaylistView"));
        QVERIFY(page);
        for (const int width : {404, 355, 280}) {
            page->setWidth(width);
            Rig::settle();
            rig.render();
            int actions = 0;
            for (QQuickItem *item : page->findChildren<QQuickItem *>()) {
                if (!QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1StringView("TextButton_QMLTYPE"))) continue;
                const QString label = item->property("text").toString();
                if (label != QStringLiteral("Play") && label != QStringLiteral("Shuffle") && label != QStringLiteral("Add Tracks")) continue;
                ++actions;
                QVERIFY(!item->property("iconOnly").toBool());
                QVERIFY(item->width() > item->height());
                QVERIFY(item->x() >= 0);
                QVERIFY(item->x() + item->width() <= item->parentItem()->width());
            }
            QCOMPARE(actions, 3);
        }
    }

    void windowControlsOnRightMovesControlsAndInspector()
    {
        Rig rig;
        rig.settings.setPerformanceMode(true);
        rig.settings.setShowWindowControls(true);
        rig.settings.setWindowControlsOnRight(false);
        QQmlComponent component(&rig.engine, QUrl(QStringLiteral("qrc:/qt/qml/Swiftify/qml/Main.qml")));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        window->setWidth(1280);
        auto *controls = window->findChild<QQuickItem *>(QStringLiteral("appWindowControls"));
        auto *inspector = window->findChild<QQuickItem *>(QStringLiteral("mainInspector"));
        auto *close = window->findChild<QQuickItem *>(QStringLiteral("closeWindowButton"));
        auto *minimize = window->findChild<QQuickItem *>(QStringLiteral("minimizeWindowButton"));
        QVERIFY(controls && inspector && close && minimize);
        Rig::settle();

        // Default: controls on left
        QCOMPARE(controls->x(), 12.0);
        QCOMPARE(inspector->property("y").toReal(), 8.0);
        QVERIFY(close->x() < minimize->x());

        // Toggle window controls to right
        rig.settings.setWindowControlsOnRight(true);
        Rig::settle();

        if (rig.app.isMacOS()) {
            // macOS uses native traffic lights, not the in-app controls.
            // Moving hidden controls must not move the inspector.
            QCOMPARE(inspector->property("y").toReal(), 8.0);
            QVERIFY(!controls->isVisible());
            return;
        }

        // Controls on right: x is window width - controls width - 12
        QCOMPARE(controls->x(), window->width() - controls->width() - 12.0);
        // Inspector moved down by 36: top margin 44 (8 + 36)
        QCOMPARE(inspector->property("y").toReal(), 44.0);
        // RightToLeft layout: minimize is on the left, close is on the right
        QVERIFY(minimize->x() < close->x());

        // Hide window controls
        rig.settings.setShowWindowControls(false);
        Rig::settle();
        QVERIFY(!controls->isVisible());
        // Inspector returns to top margin 8
        QCOMPARE(inspector->property("y").toReal(), 8.0);

        // Verify SettingsPages Appearance toggle grays out when window deco is off
        QQmlComponent settingsComponent(&rig.engine, QUrl(QStringLiteral("qrc:/qt/qml/Swiftify/qml/settings/SettingsPages.qml")));
        std::unique_ptr<QObject> settingsObject(settingsComponent.create());
        QVERIFY2(settingsObject, qPrintable(settingsComponent.errorString()));
        settingsObject->setProperty("page", 4);
        Rig::settle();
        auto *switchItem = settingsObject->findChild<QQuickItem *>(QStringLiteral("windowControlsOnRightSwitch"));
        QVERIFY(switchItem);

        // showWindowControls is currently false -> switch is disabled and grayed out
        QCOMPARE(switchItem->isEnabled(), false);
        QVERIFY(switchItem->opacity() < 0.5);

        // When showWindowControls is true -> switch is enabled
        rig.settings.setShowWindowControls(true);
        Rig::settle();
        QCOMPARE(switchItem->isEnabled(), true);
        QCOMPARE(switchItem->opacity(), 1.0);
    }

    void clickingSelectedSortOptionReversesOrder()
    {
        Rig rig;
        Song song1;
        song1.name = QStringLiteral("Zebra");
        song1.artists = QStringLiteral("Artist B");
        song1.albumName = QStringLiteral("Album C");
        song1.durationMs = 120000;
        song1.uri = QStringLiteral("file:///song1.flac");

        Song song2;
        song2.name = QStringLiteral("Apple");
        song2.artists = QStringLiteral("Artist A");
        song2.albumName = QStringLiteral("Album B");
        song2.durationMs = 240000;
        song2.uri = QStringLiteral("file:///song2.flac");

        Song song3;
        song3.name = QStringLiteral("Mango");
        song3.artists = QStringLiteral("Artist C");
        song3.albumName = QStringLiteral("Album A");
        song3.durationMs = 180000;
        song3.uri = QStringLiteral("file:///song3.flac");

        const Playlist playlist = rig.localFiles.createPlaylist(QStringLiteral("Test Playlist"), {}, {}, {song1, song2, song3});
        rig.library.selectPlaylist(playlist);
        rig.library.setPlaylistSongsForTesting({song1, song2, song3});

        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Playlist);
        QCOMPARE(rig.library.playlistSortReversed(), false);
        QCOMPARE(rig.library.displayedPlaylistSongs()->count(), 3);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(0).name, QStringLiteral("Zebra"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(1).name, QStringLiteral("Apple"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(2).name, QStringLiteral("Mango"));
        QVERIFY(rig.library.canReorderPlaylist());

        // Reverse playlist order
        rig.library.togglePlaylistSortOrder(LibraryStore::SortOrder::Playlist);
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Playlist);
        QCOMPARE(rig.library.playlistSortReversed(), true);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(0).name, QStringLiteral("Mango"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(1).name, QStringLiteral("Apple"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(2).name, QStringLiteral("Zebra"));
        QVERIFY(!rig.library.canReorderPlaylist());

        // Toggle back to forward playlist order
        rig.library.togglePlaylistSortOrder(LibraryStore::SortOrder::Playlist);
        QCOMPARE(rig.library.playlistSortReversed(), false);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(0).name, QStringLiteral("Zebra"));
        QVERIFY(rig.library.canReorderPlaylist());

        // Sort by Title (first click: ascending, reversed is false)
        rig.library.togglePlaylistSortOrder(LibraryStore::SortOrder::Title);
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Title);
        QCOMPARE(rig.library.playlistSortReversed(), false);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(0).name, QStringLiteral("Apple"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(1).name, QStringLiteral("Mango"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(2).name, QStringLiteral("Zebra"));

        // Sort by Title again (second click: descending, reversed is true)
        rig.library.togglePlaylistSortOrder(LibraryStore::SortOrder::Title);
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Title);
        QCOMPARE(rig.library.playlistSortReversed(), true);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(0).name, QStringLiteral("Zebra"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(1).name, QStringLiteral("Mango"));
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(2).name, QStringLiteral("Apple"));

        // Switch to Duration (first click: ascending, reversed resets to false)
        rig.library.togglePlaylistSortOrder(LibraryStore::SortOrder::Duration);
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Duration);
        QCOMPARE(rig.library.playlistSortReversed(), false);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(0).durationMs, 120000);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(1).durationMs, 180000);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(2).durationMs, 240000);

        // Click Duration again (reverses to descending)
        rig.library.togglePlaylistSortOrder(LibraryStore::SortOrder::Duration);
        QCOMPARE(rig.library.playlistSortReversed(), true);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(0).durationMs, 240000);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(1).durationMs, 180000);
        QCOMPARE(rig.library.displayedPlaylistSongs()->at(2).durationMs, 120000);

        // Selecting a playlist resets sort to Playlist order and reversed to false
        rig.library.selectPlaylist(playlist);
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Playlist);
        QCOMPARE(rig.library.playlistSortReversed(), false);
    }

    void topBarSortMenuTogglesReverseAndArrow()
    {
        Rig rig;
        const Playlist playlist = rig.localFiles.createPlaylist(QStringLiteral("Mine"), {}, {}, {});
        rig.library.selectPlaylist(playlist);
        QQmlComponent component(&rig.engine);
        component.setData("import QtQuick\nimport Swiftify\n"
                          "Item { width: 1280; height: 160\n"
                          "Window { id: host; visible: false }\n"
                          "Rectangle { id: page; anchors.fill: parent }\n"
                          "TopBar { id: bar; objectName: 'bar'; width: parent.width; window: host; backdropSource: page } }", QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *holder = qobject_cast<QQuickItem *>(object.get());
        holder->setParentItem(rig.window->contentItem());
        Rig::settle();
        rig.render();

        auto *sortButton = holder->findChild<QQuickItem *>(QStringLiteral("sortButton"));
        QVERIFY(sortButton);
        auto *sortMenuLoader = holder->findChild<QObject *>(QStringLiteral("sortMenu"));
        QVERIFY(sortMenuLoader);
        QVERIFY(QMetaObject::invokeMethod(sortMenuLoader, "popupAt", Q_ARG(QQuickItem *, sortButton), Q_ARG(qreal, 0), Q_ARG(qreal, sortButton->height())));
        Rig::settle();
        rig.render();

        auto *menu = sortMenuLoader->property("item").value<QObject *>();
        QVERIFY(menu);

        QQuickItem *titleItem = nullptr;
        const int count = menu->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, i));
            if (item && item->property("text").toString() == QStringLiteral("Title")) {
                titleItem = item;
                break;
            }
        }
        QVERIFY(titleItem);
        QCOMPARE(titleItem->property("checked").toBool(), false);
        QCOMPARE(titleItem->property("trailingSymbol").toString(), QString());

        // First click: selects Title (ascending)
        QVERIFY(QMetaObject::invokeMethod(titleItem, "triggered") || QMetaObject::invokeMethod(titleItem, "trigger"));
        Rig::settle();
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Title);
        QCOMPARE(rig.library.playlistSortReversed(), false);
        QCOMPARE(titleItem->property("checked").toBool(), true);
        QCOMPARE(titleItem->property("trailingSymbol").toString(), QStringLiteral("arrow_upward"));

        // Second click: reverses order (descending)
        QVERIFY(QMetaObject::invokeMethod(titleItem, "triggered") || QMetaObject::invokeMethod(titleItem, "trigger"));
        Rig::settle();
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Title);
        QCOMPARE(rig.library.playlistSortReversed(), true);
        QCOMPARE(titleItem->property("checked").toBool(), true);
        QCOMPARE(titleItem->property("trailingSymbol").toString(), QStringLiteral("arrow_downward"));

        // Third click: toggles back to ascending
        QVERIFY(QMetaObject::invokeMethod(titleItem, "triggered") || QMetaObject::invokeMethod(titleItem, "trigger"));
        Rig::settle();
        QCOMPARE(rig.library.playlistSortOrder(), LibraryStore::SortOrder::Title);
        QCOMPARE(rig.library.playlistSortReversed(), false);
        QCOMPARE(titleItem->property("checked").toBool(), true);
        QCOMPARE(titleItem->property("trailingSymbol").toString(), QStringLiteral("arrow_upward"));
    }

    void playlistPlayButtonPlaysReversedOrder()
    {
        Rig rig;
        Song song1;
        song1.name = QStringLiteral("Zebra");
        song1.artists = QStringLiteral("Artist B");
        song1.albumName = QStringLiteral("Album C");
        song1.durationMs = 120000;
        song1.uri = QStringLiteral("file:///music/song1.flac");

        Song song2;
        song2.name = QStringLiteral("Apple");
        song2.artists = QStringLiteral("Artist A");
        song2.albumName = QStringLiteral("Album B");
        song2.durationMs = 240000;
        song2.uri = QStringLiteral("file:///music/song2.flac");

        Playlist playlist;
        playlist.id = QStringLiteral("test_playlist_1");
        playlist.name = QStringLiteral("Reversed Playlist");
        playlist.songCount = 2;

        rig.library.selectPlaylist(playlist);
        rig.library.setPlaylistSongsForTesting({song1, song2});

        // Reverse the playlist order
        rig.library.togglePlaylistSortOrder(LibraryStore::SortOrder::Playlist);
        QCOMPARE(rig.library.playlistSortReversed(), true);
        QCOMPARE(rig.library.displayedPlaylistSongs()->count(), 2);

        const auto page = rig.show(QStringLiteral("PlaylistView"));
        QVERIFY(page);

        auto *playButton = page->findChild<QQuickItem *>(QStringLiteral("playButton"));
        QVERIFY(playButton);
        QCOMPARE(playButton->isEnabled(), true);

        QVERIFY(QMetaObject::invokeMethod(playButton, "clicked"));
        Rig::settle();

        // Queue should match the reversed displayed order: song2 (Apple), then song1 (Zebra)
        const auto &queue = rig.player.activeQueue();
        QCOMPARE(queue.size(), 2);
        QCOMPARE(queue.at(0).name, QStringLiteral("Apple"));
        QCOMPARE(queue.at(1).name, QStringLiteral("Zebra"));
        QCOMPARE(rig.player.currentSong().name, QStringLiteral("Apple"));
    }

    void views_data()
    {
        QTest::addColumn<QString>("type");
        for (const char *type : {"SoundPanel", "SleepTimerPanel", "ImmersiveQueue", "ImmersiveLyrics", "SegmentedControl",
                                 "StyledSwitch", "RollingText", "PlayerBar", "LyricsPanel", "QueuePanel", "Sidebar",
                                 "PageHost", "HomeView", "SearchView", "ArtistView", "AlbumView", "PlaylistView", "PlaylistsView", "SavedSongsView", "LocalFilesView", "LocalTracksView", "StartupView",
                                 "SetupView", "CompactMeter", "MaterialShape", "ShapeIcon", "SquiggleRing", "WavyLine",
                                 "LoadingSpinner", "ShapeButton", "WavyPill", "ToolbarButton", "PlayButton", "ScrollingText"}) {
            QTest::newRow(type) << QString::fromLatin1(type);
        }
    }

    void views()
    {
        QFETCH(QString, type);
        Rig rig;
        rig.playSomething();
        QVERIFY(rig.show(type));
    }
};

QTEST_MAIN(ViewsTest)
#include "tst_views.moc"
