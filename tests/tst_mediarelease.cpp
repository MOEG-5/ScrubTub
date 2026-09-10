// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QStandardPaths>
#include "media/Probe.h"
#include "media/Extract.h"
#include "testsupport/MediaFixtures.h"

using namespace scrubtub;
using namespace scrubtub::testsupport;

class TestMediaRelease : public QObject {
    Q_OBJECT
private:
    QString fingerprint;
private slots:
    void initTestCase() { fingerprint = vidsFingerprint(); }
    void cleanupTestCase() { QCOMPARE(vidsFingerprint(), fingerprint); }
    void corpus_data()
    {
        QTest::addColumn<QString>("path");
        if (!vidsAvailable())
            QSKIP("vids/ corpus unavailable");
        if (qEnvironmentVariableIsEmpty("SCRUBTUB_RELEASE_MEDIA_DIR"))
            QSKIP("Set SCRUBTUB_RELEASE_MEDIA_DIR to test a private media build");
        for (const auto& name : vidsFiles()) {
            const QString suffix = QFileInfo(name).suffix().toLower();
            if (QStringList{QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("wmv"), QStringLiteral("flv")}.contains(suffix))
                QTest::newRow(qPrintable(name)) << QDir(vidsDir()).filePath(name);
        }
    }
    void corpus()
    {
        QFETCH(QString, path);
        const QString dir = qEnvironmentVariable("SCRUBTUB_RELEASE_MEDIA_DIR");
        const QString probe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"), {dir});
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"), {dir});
        QVERIFY(!probe.isEmpty());
        QVERIFY(!ffmpeg.isEmpty());
        const auto reference = Probe::run(QStandardPaths::findExecutable(QStringLiteral("ffprobe")), path);
        const auto actual = Probe::run(probe, path);
        QCOMPARE(actual.ok, reference.ok);
        if (!reference.ok)
            QSKIP("System ffprobe also rejects this input");
        QCOMPARE(actual.codec, reference.codec);
        QCOMPARE(actual.durationMs, reference.durationMs);
        QCOMPARE(actual.displayWidth, reference.displayWidth);
        QCOMPARE(actual.displayHeight, reference.displayHeight);
        QCOMPARE(actual.selectedStreamIndex, reference.selectedStreamIndex);
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        PosterRequest request;
        request.ffmpegPath = ffmpeg;
        request.sourcePath = path;
        request.outputPath = cache.filePath(QStringLiteral("poster.jpg"));
        request.durationMs = actual.durationMs;
        request.selectedStreamIndex = actual.selectedStreamIndex;
        const auto result = Extract::poster(request);
        QVERIFY2(result.ok, qPrintable(result.error));
    }
};
QTEST_GUILESS_MAIN(TestMediaRelease)
#include "tst_mediarelease.moc"
