#include "system/MediaTimeline.h"

#include <QTest>

using swiftify::MediaTimeline;

class MediaTimelineTest : public QObject {
    Q_OBJECT
private slots:
    void publishesDurationInWindowsTimeSpanUnits()
    {
        const auto timeline = MediaTimeline::fromMilliseconds(true, 240000, 0, 1500);
        QCOMPARE(timeline.end, qint64(2400000000)); // exceeds a signed 32-bit integer
        QCOMPARE(timeline.position, qint64(15000000));
    }

    void fallsBackToMetadataWhileTheEngineLoads()
    {
        const auto timeline = MediaTimeline::fromMilliseconds(true, 0, 180000, 2000);
        QCOMPARE(timeline.end, qint64(1800000000));
        QCOMPARE(timeline.position, qint64(20000000));
        QCOMPARE(MediaTimeline::fromMilliseconds(true, 160000, 180000, 2000).end, qint64(1600000000));
    }

    void clampsAndClearsTimelineWithoutLeakingPreviousTrackDuration()
    {
        QCOMPARE(MediaTimeline::fromMilliseconds(true, 1000, 0, 5000).position, qint64(10000000));
        const auto empty = MediaTimeline::fromMilliseconds(false, 240000, 180000, 3000);
        QCOMPARE(empty.end, qint64(0)); QCOMPARE(empty.position, qint64(0));
        const auto unknown = MediaTimeline::fromMilliseconds(true, 0, -10, 3000);
        QCOMPARE(unknown.end, qint64(0)); QCOMPARE(unknown.position, qint64(0));
        QCOMPARE(MediaTimeline::fromMilliseconds(true, 10800000, 0, 10000000).position, qint64(100000000000));
    }
};

QTEST_GUILESS_MAIN(MediaTimelineTest)
#include "tst_media_timeline.moc"
