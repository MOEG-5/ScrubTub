// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QStandardPaths>
#include "media/Extract.h"
#include "media/HoverSession.h"
#include "media/Probe.h"
#include "testsupport/MediaFixtures.h"

using namespace scrubtub;

class TestReleaseSmoke : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString video, ffmpeg, ffprobe, mpv;
    ProbeResult reference;
    QByteArray originalHash;
    QByteArray hash() const {
        QFile file(video);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    }
private slots:
    void initTestCase() {
        const QString directory = qEnvironmentVariable("SCRUBTUB_RELEASE_MEDIA_DIR");
        if (directory.isEmpty()) QSKIP("Set SCRUBTUB_RELEASE_MEDIA_DIR to validate a private build");
        ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"), {directory});
        ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"), {directory});
        mpv = QStandardPaths::findExecutable(QStringLiteral("mpv"), {directory});
        QVERIFY(!ffmpeg.isEmpty()); QVERIFY(!ffprobe.isEmpty()); QVERIFY(!mpv.isEmpty());
        QVERIFY(temporary.isValid());
        QString error;
        QVERIFY2(testsupport::makeSyntheticVideo(temporary.path(), QStringLiteral("original.mp4"), 3000, &error), qPrintable(error));
        video = temporary.filePath(QStringLiteral("original.mp4"));
        originalHash = hash();
        QVERIFY(!originalHash.isEmpty());
        reference = Probe::run(QStandardPaths::findExecutable(QStringLiteral("ffprobe")), video);
        QVERIFY2(reference.ok, qPrintable(reference.error));
    }
    void cleanupTestCase() {
        if (!originalHash.isEmpty()) QCOMPARE(hash(), originalHash);
    }
    void probeAndExtract() {
        const auto actual = Probe::run(ffprobe, video);
        QVERIFY2(actual.ok, qPrintable(actual.error));
        QCOMPARE(actual.durationMs, reference.durationMs);
        QCOMPARE(actual.codec, reference.codec);
        QCOMPARE(actual.displayWidth, reference.displayWidth);
        QCOMPARE(actual.displayHeight, reference.displayHeight);
        PosterRequest poster;
        poster.ffmpegPath = ffmpeg; poster.sourcePath = video;
        poster.outputPath = temporary.filePath(QStringLiteral("poster.jpg"));
        poster.durationMs = actual.durationMs; poster.selectedStreamIndex = actual.selectedStreamIndex;
        const auto image = Extract::poster(poster);
        QVERIFY2(image.ok, qPrintable(image.error));
        StoryboardRequest storyboard;
        storyboard.ffmpegPath = ffmpeg; storyboard.sourcePath = video;
        storyboard.outputDir = temporary.filePath(QStringLiteral("frames"));
        storyboard.outputPath = temporary.filePath(QStringLiteral("storyboard.jpg"));
        storyboard.durationMs = actual.durationMs;
        storyboard.selectedStreamIndex = actual.selectedStreamIndex;
        const auto atlas = Extract::storyboard(storyboard);
        QVERIFY2(atlas.ok, qPrintable(atlas.error));
        QVERIFY(!atlas.sampleTimesMs.isEmpty());
    }
    void liveScrubbing() {
        HoverSession session;
        session.setMpvPath(mpv);
        session.engage(1, 1, video, reference.durationMs, reference.durationMs / 3);
        QTRY_VERIFY_WITH_TIMEOUT(!session.lastFrame().isNull(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(session.status(), QStringLiteral("ready"), 15000);
        const qint64 first = session.lastFramePtsMs();
        session.scrub(reference.durationMs * 2 / 3);
        QTRY_VERIFY_WITH_TIMEOUT(session.lastFramePtsMs() > first, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(session.status(), QStringLiteral("ready"), 15000);
        session.disengage();
        QCOMPARE(session.status(), QStringLiteral("idle"));
    }
};
QTEST_MAIN(TestReleaseSmoke)
#include "tst_releasesmoke.moc"
