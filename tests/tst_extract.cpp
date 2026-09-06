// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Extraction contract checks (TECH_SPEC.md section 12): poster geometry and
// content, storyboard sample plan/timestamps/atlas, rotation handling,
// corrupt-input tolerance. Reads the original fixture read-only; generated
// clips are tiny and count against the fixture budget.
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "media/Extract.h"

using namespace itub;

#ifdef ITUB_DEFAULT_SOURCE_FIXTURE
static QString fixturePath()
{
    const QByteArray env = qgetenv("ITUB_TEST_SOURCE_VIDEO");
    return env.isEmpty() ? QStringLiteral(ITUB_DEFAULT_SOURCE_FIXTURE)
                         : QString::fromLocal8Bit(env);
}
#endif

class TestExtract : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void samplePlanForShortClips();
    void posterGeometryAndContent();
    void storyboardAtlasAndTimestamps();
    void rotatedClipProducesPortraitPoster();
    void truncatedInputStopsWithReason();

private:
    // Generates a small clip; counts against the derived-media budget.
    QString generateClip(const QStringList& extraArgs, const QString& name);
    QString ffmpeg() const { return m_ffmpeg; }

    QTemporaryDir m_temp;
    QString m_ffmpeg;
    QString m_rotated;
    QString m_short;
};

void TestExtract::initTestCase()
{
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!m_ffmpeg.isEmpty(), "ffmpeg required");
#ifdef ITUB_DEFAULT_SOURCE_FIXTURE
    if (fixturePath().isEmpty() || !QFileInfo::exists(fixturePath()))
        QSKIP("Source fixture unavailable");
#else
    QSKIP("No fixture compiled in");
#endif
}

QString TestExtract::generateClip(const QStringList& extraArgs, const QString& name)
{
    const QString out = m_temp.filePath(name);
    QProcess p;
    QStringList args{QStringLiteral("-v"), QStringLiteral("error"),
                     QStringLiteral("-nostdin"),
                     QStringLiteral("-f"), QStringLiteral("lavfi"),
                     QStringLiteral("-i"), QStringLiteral("testsrc=duration=6:size=640x360:rate=25")};
    args += extraArgs;
    args << QStringLiteral("-y") << out;
    p.start(m_ffmpeg, args);
    if (!p.waitForFinished(60000))
        return QString();
    if (p.exitCode() != 0)
        return QString();
    return out;
}

void TestExtract::samplePlanForShortClips()
{
    // Unknown duration: no plan (poster-only path).
    QVERIFY(Extract::samplePlanMs(-1, 24).isEmpty());
    QVERIFY(Extract::samplePlanMs(0, 24).isEmpty());
    // Long video: full 24 samples from first frame to before the end.
    const QVector<qint64> long1 = Extract::samplePlanMs(667500, 24);
    QCOMPARE(long1.size(), 24);
    QCOMPARE(long1.first(), 0);
    QVERIFY(long1.last() < 667500);
    for (int i = 1; i < long1.size(); ++i)
        QVERIFY(long1.at(i) > long1.at(i - 1)); // strictly increasing
    // A 3 s clip: fewer samples (≥2).
    const QVector<qint64> short1 = Extract::samplePlanMs(3000, 24);
    QCOMPARE(short1.size(), 6); // 3000/500
    QCOMPARE(short1.first(), 0);
}

void TestExtract::posterGeometryAndContent()
{
    PosterRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = fixturePath();
    request.outputPath = m_temp.filePath(QStringLiteral("poster.jpg"));
    request.durationMs = 667500;
    request.selectedStreamIndex = 0;
    const ExtractResult result = Extract::poster(request);
    QVERIFY2(result.ok, qUtf8Printable(result.error));
    QVERIFY(QFile::exists(result.outputPath));
    QImage image(result.outputPath);
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 320);
    // 16:9 preserved: ~180 height (even).
    QCOMPARE(image.height(), 180);
    // A real decoded frame at 10 % of the fixture: not a uniform block.
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
    StoryboardRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = fixturePath();
    request.outputDir = m_temp.filePath(QStringLiteral("sb-tmp"));
    request.outputPath = m_temp.filePath(QStringLiteral("atlas.jpg"));
    request.durationMs = 667500;
    request.selectedStreamIndex = 0;
    const ExtractResult result = Extract::storyboard(request);
    QVERIFY2(result.ok, qUtf8Printable(result.error));
    // 24 samples recorded with actual delivered timestamps.
    QCOMPARE(result.sampleTimesMs.size(), 24);
    // Actual times track the requested plan closely for this fixture
    // (recorded values, not assumed seeks — §6).
    const QVector<qint64> plan = Extract::samplePlanMs(667500, 24);
    for (int i = 0; i < plan.size(); ++i)
        QVERIFY2(qAbs(result.sampleTimesMs.at(i) - plan.at(i)) < 2000,
                 qPrintable(QStringLiteral("sample %1 off: %2 vs %3")
                                .arg(i).arg(result.sampleTimesMs.at(i)).arg(plan.at(i))));
    QImage atlas(result.outputPath);
    QVERIFY(!atlas.isNull());
    QCOMPARE(atlas.width(), 6 * 320); // 6-column atlas
    QCOMPARE(atlas.height(), 4 * 180);
}

void TestExtract::rotatedClipProducesPortraitPoster()
{
    m_rotated = generateClip({QStringLiteral("-t"), QStringLiteral("3"),
                              QStringLiteral("-vf"), QStringLiteral("transpose=1"),
                              QStringLiteral("-an"), QStringLiteral("-c:v"), QStringLiteral("libx264")},
                             QStringLiteral("rotated.mp4"));
    QVERIFY2(!m_rotated.isEmpty(), "rotated clip generation failed");

    // Probe it to obtain the display matrix and stream selection.
    const ProbeResult probe = Probe::run(
        QStandardPaths::findExecutable(QStringLiteral("ffprobe")), m_rotated);
    QVERIFY(probe.ok);
    // transpose bakes the rotation into the coded frames (no display matrix
    // side data in this build's toolchain — the matrix path is covered by
    // the synthetic-JSON unit tests in tst_probe).
    QCOMPARE(probe.rotationDeg, 0);
    QCOMPARE(probe.displayWidth, 360);
    QCOMPARE(probe.displayHeight, 640);

    PosterRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = m_rotated;
    request.outputPath = m_temp.filePath(QStringLiteral("rotated-poster.jpg"));
    request.durationMs = probe.durationMs;
    request.selectedStreamIndex = probe.selectedStreamIndex;
    const ExtractResult result = Extract::poster(request);
    QVERIFY2(result.ok, qUtf8Printable(result.error));
    QImage image(result.outputPath);
    QVERIFY(!image.isNull());
    // Display-oriented: portrait poster from a portrait video (§1, §6).
    QVERIFY2(image.height() > image.width(),
             qPrintable(QStringLiteral("poster not portrait: %1x%2")
                            .arg(image.width()).arg(image.height())));
}

void TestExtract::truncatedInputStopsWithReason()
{
    // Truncated media: poster extraction fails with a bounded reason and no
    // artifact left behind.
    const QString src = m_temp.filePath(QStringLiteral("truncated.mp4"));
    QFile orig(fixturePath());
    QVERIFY(orig.open(QIODevice::ReadOnly));
    const QByteArray head = orig.read(64 * 1024);
    orig.close();
    {
        QFile out(src);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(head);
    }

    PosterRequest request;
    request.ffmpegPath = ffmpeg();
    request.sourcePath = src;
    request.outputPath = m_temp.filePath(QStringLiteral("no-poster.jpg"));
    request.durationMs = 60000;
    const ExtractResult result = Extract::poster(request);
    QVERIFY2(!result.ok, "truncated input must not produce a poster");
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!QFile::exists(request.outputPath));
}

QTEST_GUILESS_MAIN(TestExtract)
#include "tst_extract.moc"
