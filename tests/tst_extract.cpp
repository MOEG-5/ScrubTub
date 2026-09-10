// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "app/MediaTools.h"
// Extraction contract checks (TECH_SPEC.md section 12): poster geometry and
// content, storyboard sample plan/timestamps/atlas, rotation handling,
// cancellation, per-job timeout, partial-atlas usability and corrupt-input
// tolerance. Happy paths read the repository's vids/ corpus as-is (AGENTS.md:
// read-only, never copied or derived); cases that need broken media generate
// their own originals and mutate only those copies.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <cmath>

#include "media/Extract.h"
#include "media/Probe.h"
#include "testsupport/MediaFixtures.h"

using namespace scrubtub;
using namespace scrubtub::testsupport;

namespace {

// Delivered timestamps are the recorder's actual seek results, not the
// requested plan (§6); allow a generous fraction of the clip plus a small
// absolute floor so short clips stay meaningful without assuming the seek
// granularity of any particular implementation.
qint64 actualTimeSlack(qint64 durationMs)
{
    return qMax<qint64>(250, durationMs / 10);
}

// Tile geometry implied by the request semantics: the source is aspect-fitted
// inside tileWidth x tileWidth with even edges (Extract.h "longest edge, both
// dimensions bounded").
QSize fittedTile(int width, int height, int edge)
{
    if (width <= 0 || height <= 0 || edge <= 0)
        return {};
    const QSize scaled = width >= height
        ? QSize(edge, static_cast<int>(std::lround(static_cast<double>(edge) * height / width)))
        : QSize(static_cast<int>(std::lround(static_cast<double>(edge) * width / height)), edge);
    return QSize(qMax(2, scaled.width() & ~1), qMax(2, scaled.height() & ~1));
}

} // namespace

class TestExtract : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void samplePlanForShortClips();
    void posterGeometryAndContent();
    void storyboardAtlasAndTimestamps();
    void cancelledStoryboardKeepsPartialAtlasUsable();
    void cancelledStoryboardLeavesNoArtifact();
    void posterTimeoutLeavesNoArtifact();
    void cancelledPosterLeavesNoArtifact();
    void rotatedClipProducesPortraitPoster();
    void hostileInputsFailWithoutArtifact_data();
    void hostileInputsFailWithoutArtifact();

private:
    QString ffmpeg() const { return m_ffmpeg; }

    QTemporaryDir m_temp;
    QString m_ffmpeg;
    QString m_ffprobe;
    QString m_video;        // smallest corpus video (mp4 preferred), read-only
    ProbeResult m_probe;
    QString m_synthetic;    // generated original; hostile cases mutate copies
    QString m_syntheticError;
    QString m_fingerprint;
};

void TestExtract::initTestCase()
{
    // Read-only guard: the corpus must be byte-identical afterwards.
    m_fingerprint = vidsFingerprint();

    m_ffmpeg = findMediaTool(QStringLiteral("ffmpeg"), "SCRUBTUB_FFMPEG_PATH");
    m_ffprobe = findMediaTool(QStringLiteral("ffprobe"), "SCRUBTUB_FFPROBE_PATH");
    QVERIFY2(!m_ffmpeg.isEmpty(), "ffmpeg required");
    QVERIFY2(!m_ffprobe.isEmpty(), "ffprobe required");

    m_video = smallestVideo(QStringLiteral("mp4"));
    if (m_video.isEmpty())
        m_video = smallestVideo();
    if (!m_video.isEmpty())
        m_probe = Probe::run(m_ffprobe, m_video);

    if (!makeSyntheticVideo(m_temp.path(), QStringLiteral("hostile-source.mp4"), 2000,
                            &m_syntheticError))
        m_synthetic.clear();
    else
        m_synthetic = m_temp.filePath(QStringLiteral("hostile-source.mp4"));
}

void TestExtract::cleanupTestCase()
{
    QCOMPARE(vidsFingerprint(), m_fingerprint);
}

void TestExtract::samplePlanForShortClips()
{
    // Unknown duration: no plan (poster-only path).
    QVERIFY(Extract::samplePlanMs(-1, 24).isEmpty());
    QVERIFY(Extract::samplePlanMs(0, 24).isEmpty());
    // Long video: centered samples spread across the duration.
    const QVector<qint64> long1 = Extract::samplePlanMs(667500, 24);
    QCOMPARE(long1.size(), 24);
    QCOMPARE(long1.first(), 667500 / 48);
    QVERIFY(long1.last() < 667500);
    for (int i = 1; i < long1.size(); ++i)
        QVERIFY(long1.at(i) > long1.at(i - 1)); // strictly increasing
    // A 3 s clip: fewer samples (≥2).
    const QVector<qint64> short1 = Extract::samplePlanMs(3000, 24);
    QCOMPARE(short1.size(), 6); // 3000/500
    QCOMPARE(short1.first(), 250);
}

void TestExtract::posterGeometryAndContent()
{
    if (m_video.isEmpty())
        QSKIP("vids/ corpus unavailable (set SCRUBTUB_TEST_VIDS_DIR)");
    QVERIFY2(m_probe.ok, qUtf8Printable(m_probe.error));
    QVERIFY(m_probe.durationMs > 0);

    PosterRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = m_video;
    request.outputPath = m_temp.filePath(QStringLiteral("poster.jpg"));
    request.durationMs = m_probe.durationMs;
    request.selectedStreamIndex = m_probe.selectedStreamIndex;
    const ExtractResult result = Extract::poster(request);
    QVERIFY2(result.ok, qUtf8Printable(result.error));
    QCOMPARE(result.outputPath, request.outputPath);
    QVERIFY(QFile::exists(result.outputPath));
    // Atomic placement: the scratch file is gone by the time we look.
    QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));

    QImage image(result.outputPath);
    QVERIFY(!image.isNull());
    QCOMPARE(result.pixelSize, image.size());
    // Dimensions follow the requested scale and the source's display aspect.
    QCOMPARE(image.width(), request.targetWidth);
    const double aspect = static_cast<double>(m_probe.displayWidth) / m_probe.displayHeight;
    QVERIFY2(qAbs(image.width() / static_cast<double>(image.height()) - aspect) < 0.02,
             qPrintable(QStringLiteral("poster %1x%2 does not match display %3x%4")
                            .arg(image.width()).arg(image.height())
                            .arg(m_probe.displayWidth).arg(m_probe.displayHeight)));
    // A real decoded frame: not a uniform block.
    quint64 sum = 0, sumSq = 0;
    const int step = qMax(1, image.sizeInBytes() / 40000);
    int samples = 0;
    for (int i = 0; i < image.sizeInBytes(); i += step) {
        const quint8 v = static_cast<quint8>(image.constBits()[i]);
        sum += v;
        sumSq += static_cast<quint64>(v) * v;
        ++samples;
    }
    const double mean = static_cast<double>(sum) / samples;
    const double variance = static_cast<double>(sumSq) / samples - mean * mean;
    QVERIFY2(variance > 100.0, "poster appears blank; expected real content");
}

void TestExtract::storyboardAtlasAndTimestamps()
{
    if (m_video.isEmpty())
        QSKIP("vids/ corpus unavailable (set SCRUBTUB_TEST_VIDS_DIR)");
    QVERIFY2(m_probe.ok, qUtf8Printable(m_probe.error));

    StoryboardRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = m_video;
    request.outputDir = m_temp.filePath(QStringLiteral("sb-tmp"));
    request.outputPath = m_temp.filePath(QStringLiteral("atlas.jpg"));
    request.durationMs = m_probe.durationMs;
    request.selectedStreamIndex = m_probe.selectedStreamIndex;
    const QVector<qint64> plan = Extract::samplePlanMs(request.durationMs, request.sampleCount);
    QVERIFY(!plan.isEmpty());

    const ExtractResult result = Extract::storyboard(request);
    QVERIFY2(result.ok, qUtf8Printable(result.error));
    QCOMPARE(result.outputPath, request.outputPath);
    QVERIFY(QFile::exists(result.outputPath));
    QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));

    // The atlas holds exactly one tile per planned sample, and the recorded
    // timestamps are the actual delivered ones: inside the clip, ascending,
    // and close to (but not assumed equal to) the requested plan.
    const int tiles = plan.size();
    QCOMPARE(result.sampleTimesMs.size(), tiles);
    for (int i = 0; i < tiles; ++i) {
        const qint64 actual = result.sampleTimesMs.at(i);
        QVERIFY(actual >= 0);
        QVERIFY(actual <= request.durationMs);
        if (i > 0)
            QVERIFY2(actual > result.sampleTimesMs.at(i - 1),
                     "recorded sample timestamps must ascend");
        QVERIFY2(qAbs(actual - plan.at(i)) <= actualTimeSlack(request.durationMs),
                 qPrintable(QStringLiteral("sample %1 recorded %2 ms, plan %3 ms")
                                .arg(i).arg(actual).arg(plan.at(i))));
    }

    QImage atlas(result.outputPath);
    QVERIFY(!atlas.isNull());
    QCOMPARE(result.pixelSize, atlas.size());
    QVERIFY(QFileInfo(result.outputPath).size() <= kSparsePreviewMaxBytes);
    // One tile per planned sample in a single row, on a canvas no wider than
    // `columns` tiles and never taller than a tile.
    const QSize tile = fittedTile(m_probe.displayWidth, m_probe.displayHeight,
                                 request.tileWidth);
    QVERIFY(!tile.isEmpty());
    QVERIFY(tile.width() <= request.tileWidth);
    QVERIFY(tile.height() <= request.tileWidth);
    QVERIFY(tiles <= request.columns);
    QVERIFY2(qAbs(atlas.height() - tile.height()) <= 2,
             qPrintable(QStringLiteral("atlas is %1 px tall, not one %2 px tile row")
                            .arg(atlas.height()).arg(tile.height())));
    QVERIFY(atlas.width() >= tiles * tile.width() - 2);
    QVERIFY(atlas.width() <= request.columns * request.tileWidth);
    // The scratch frame directory is not left behind.
    const QStringList leftovers = QDir(request.outputDir)
                                      .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    QVERIFY2(leftovers.isEmpty(),
             qPrintable(QStringLiteral("scratch files left behind: %1")
                            .arg(leftovers.join(QLatin1Char(',')))));
}

void TestExtract::cancelledStoryboardKeepsPartialAtlasUsable()
{
    if (m_video.isEmpty())
        QSKIP("vids/ corpus unavailable (set SCRUBTUB_TEST_VIDS_DIR)");
    QVERIFY2(m_probe.ok, qUtf8Printable(m_probe.error));

    StoryboardRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = m_video;
    request.outputDir = m_temp.filePath(QStringLiteral("sb-partial-tmp"));
    request.outputPath = m_temp.filePath(QStringLiteral("partial-atlas.jpg"));
    request.durationMs = m_probe.durationMs;
    request.selectedStreamIndex = m_probe.selectedStreamIndex;
    const QVector<qint64> plan = Extract::samplePlanMs(request.durationMs, request.sampleCount);
    QVERIFY(!plan.isEmpty());

    // Cancel as soon as the first sample job has spawned. Whatever was already
    // delivered must still compose a usable atlas; the alternative is an
    // explicit failure with no artifact at all — never a half-written cache
    // entry (§5, §6).
    std::atomic_bool cancelled{false};
    const PidSink cancelOnFirstJob = [&cancelled](qint64 pid) {
        if (pid > 0)
            cancelled.store(true);
    };
    const ExtractResult result = Extract::storyboard(request, cancelOnFirstJob, &cancelled);

    if (result.sampleTimesMs.isEmpty()) {
        QVERIFY2(!result.ok, "no samples recorded yet the storyboard claims success");
        QVERIFY(!QFile::exists(request.outputPath));
        QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));
        return;
    }

    QVERIFY2(result.ok, qUtf8Printable(result.error));
    QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));
    // The delivered tiles are a prefix of the plan with ascending timestamps.
    QVERIFY(result.sampleTimesMs.size() <= plan.size());
    for (int i = 0; i < result.sampleTimesMs.size(); ++i) {
        QVERIFY(result.sampleTimesMs.at(i) >= 0);
        if (i > 0)
            QVERIFY(result.sampleTimesMs.at(i) > result.sampleTimesMs.at(i - 1));
        QVERIFY(qAbs(result.sampleTimesMs.at(i) - plan.at(i)) <= actualTimeSlack(request.durationMs));
    }
    QImage atlas(result.outputPath);
    QVERIFY2(!atlas.isNull(), "partial atlas is not a readable image");
    QCOMPARE(result.pixelSize, atlas.size());
    const QSize tile = fittedTile(m_probe.displayWidth, m_probe.displayHeight,
                                 request.tileWidth);
    QVERIFY(!tile.isEmpty());
    QVERIFY2(qAbs(atlas.height() - tile.height()) <= 2,
             "partial atlas is not one tile row tall");
    QVERIFY(atlas.width() >= result.sampleTimesMs.size() * tile.width() - 2);
    QVERIFY(atlas.width() <= request.columns * request.tileWidth);
    QVERIFY(QFileInfo(result.outputPath).size() <= kSparsePreviewMaxBytes);
    QVERIFY2(!result.error.isEmpty(), "a cancelled job reports why it stopped");
}

void TestExtract::cancelledStoryboardLeavesNoArtifact()
{
    std::atomic_bool cancelled{true};
    StoryboardRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = m_synthetic.isEmpty() ? m_temp.filePath(QStringLiteral("no-source.mp4"))
                                               : m_synthetic;
    request.outputDir = m_temp.filePath(QStringLiteral("sb-cancelled-tmp"));
    request.outputPath = m_temp.filePath(QStringLiteral("cancelled-atlas.jpg"));
    request.durationMs = 30000;
    const ExtractResult result = Extract::storyboard(request, {}, &cancelled);
    QVERIFY2(!result.ok, "a pre-cancelled storyboard must not report success");
    QVERIFY(result.sampleTimesMs.isEmpty());
    QVERIFY(!QFile::exists(request.outputPath));
    QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));
}

void TestExtract::posterTimeoutLeavesNoArtifact()
{
    // A stalled extractor must hit the per-job timeout and leave nothing in
    // the cache (§5).
    const QString script = m_temp.filePath(QStringLiteral("slow-ffmpeg.sh"));
    QFile sh(script);
    QVERIFY(sh.open(QIODevice::WriteOnly | QIODevice::Truncate));
    sh.write("#!/bin/sh\nsleep 30\n");
    sh.close();
    QVERIFY(QFile::setPermissions(script,
                                  QFile::ExeOwner | QFile::ReadOwner | QFile::WriteOwner));

    PosterRequest request;
    request.ffmpegPath = script;
    request.sourcePath = m_synthetic.isEmpty() ? m_temp.filePath(QStringLiteral("no-source.mp4"))
                                               : m_synthetic;
    request.outputPath = m_temp.filePath(QStringLiteral("timeout-poster.jpg"));
    request.durationMs = 1000;
    request.timeoutMs = 2000;
    const ExtractResult result = Extract::poster(request);
    QVERIFY(!result.ok);
    QVERIFY(result.timedOut);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!QFile::exists(request.outputPath));
    QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));
    QFile::remove(script);
}

void TestExtract::cancelledPosterLeavesNoArtifact()
{
    std::atomic_bool cancelled{true};
    PosterRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = m_synthetic.isEmpty() ? m_temp.filePath(QStringLiteral("no-source.mp4"))
                                               : m_synthetic;
    request.outputPath = m_temp.filePath(QStringLiteral("cancelled-poster.jpg"));
    request.durationMs = 1000;
    const ExtractResult result = Extract::poster(request, {}, &cancelled);
    QVERIFY(!result.ok);
    QVERIFY(result.timedOut); // cancelled jobs are reported like timeouts
    QVERIFY(!QFile::exists(request.outputPath));
    QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));
}

void TestExtract::rotatedClipProducesPortraitPoster()
{
    // A generated original (never corpus media): transpose bakes the rotation
    // into the coded frames, so the poster must come out portrait.
    if (m_synthetic.isEmpty())
        QSKIP(qPrintable(m_syntheticError));
    const QString rotated = m_temp.filePath(QStringLiteral("rotated.mp4"));
    QFile::remove(rotated);
    QProcess transpose;
    transpose.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), {QStringLiteral("-v"), QStringLiteral("error"),
                               QStringLiteral("-nostdin"), QStringLiteral("-i"), m_synthetic,
                               QStringLiteral("-t"), QStringLiteral("3"),
                               QStringLiteral("-vf"), QStringLiteral("transpose=1"),
                               QStringLiteral("-an"), QStringLiteral("-c:v"),
                               QStringLiteral("libx264"), QStringLiteral("-y"), rotated});
    QVERIFY(transpose.waitForStarted(10000));
    QVERIFY(transpose.waitForFinished(60000));
    QVERIFY2(transpose.exitStatus() == QProcess::NormalExit && transpose.exitCode() == 0,
             qUtf8Printable(QString::fromUtf8(transpose.readAllStandardError().left(300))));

    const ProbeResult probe = Probe::run(m_ffprobe, rotated);
    QVERIFY2(probe.ok, qUtf8Printable(probe.error));
    // transpose bakes the rotation into the coded frames (no display matrix
    // side data in this toolchain — the matrix path is covered by the
    // synthetic-JSON unit tests in tst_probe).
    QCOMPARE(probe.rotationDeg, 0);
    QVERIFY2(probe.displayHeight > probe.displayWidth,
             "the generated rotated clip is not portrait");

    PosterRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = rotated;
    request.outputPath = m_temp.filePath(QStringLiteral("rotated-poster.jpg"));
    request.durationMs = probe.durationMs;
    request.selectedStreamIndex = probe.selectedStreamIndex;
    const ExtractResult result = Extract::poster(request);
    QVERIFY2(result.ok, qUtf8Printable(result.error));
    QImage image(result.outputPath);
    QVERIFY(!image.isNull());
    // Bounded to the requested size: whichever edge the implementation treats
    // as the target (PosterRequest.targetWidth) must equal it.
    QVERIFY2(image.width() == request.targetWidth || image.height() == request.targetWidth,
             qPrintable(QStringLiteral("poster %1x%2 ignores target %3")
                            .arg(image.width()).arg(image.height()).arg(request.targetWidth)));
    // Display-oriented: portrait poster from a portrait video (§1, §6).
    QVERIFY2(image.height() > image.width(),
             qPrintable(QStringLiteral("poster not portrait: %1x%2")
                            .arg(image.width()).arg(image.height())));
    const double aspect = static_cast<double>(probe.displayWidth) / probe.displayHeight;
    QVERIFY(qAbs(image.width() / static_cast<double>(image.height()) - aspect) < 0.02);
}

void TestExtract::hostileInputsFailWithoutArtifact_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("truncated-clip") << QStringLiteral("truncated");
    QTest::newRow("non-media-bytes") << QStringLiteral("bytes");
    QTest::newRow("zero-length") << QStringLiteral("empty");
}

void TestExtract::hostileInputsFailWithoutArtifact()
{
    QFETCH(QString, kind);
    const QString source = m_temp.filePath(QStringLiteral("hostile-%1.mp4").arg(kind));
    QFile::remove(source);

    if (kind == QLatin1String("truncated")) {
        // A copy of a generated original, cut in half: no moov atom left.
        if (m_synthetic.isEmpty())
            QSKIP(qPrintable(m_syntheticError));
        QFile whole(m_synthetic);
        QVERIFY(whole.open(QIODevice::ReadOnly));
        const QByteArray bytes = whole.readAll();
        whole.close();
        QVERIFY(bytes.size() > 1024);
        QFile out(source);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(bytes.left(bytes.size() / 2)), qint64(bytes.size() / 2));
        out.close();
        // Vacuity guard: the intact original is readable media.
        QVERIFY(Probe::run(m_ffprobe, m_synthetic).ok);
    } else if (kind == QLatin1String("bytes")) {
        // Hostile bytes that are not media at all; nothing corpus-derived.
        const QByteArray junk(64 * 1024, '\x12');
        QFile out(source);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(junk), qint64(junk.size()));
        out.close();
    } else {
        // A zero-length file (no movie atom, no stream).
        QFile out(source);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.close();
        QCOMPARE(QFileInfo(source).size(), qint64(0));
    }

    PosterRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = source;
    request.outputPath = m_temp.filePath(QStringLiteral("hostile-%1-poster.jpg").arg(kind));
    request.durationMs = 60000;
    const ExtractResult result = Extract::poster(request);
    QVERIFY2(!result.ok, "hostile input must not produce a poster");
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!QFile::exists(request.outputPath));
    QVERIFY(!QFile::exists(request.outputPath + QStringLiteral(".tmp.jpg")));
}

QTEST_GUILESS_MAIN(TestExtract)
#include "tst_extract.moc"
