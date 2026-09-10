// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Probe contract checks (TECH_SPEC.md section 12 "Extraction"):
// video-stream duration over subtitle duration, stream selection excluding
// attached pictures, rotation/display dimensions, corrupt input, timeouts and
// cancellation. JSON parse cases are table-driven and need no media; the run
// case reads the repository's vids/ corpus as-is (AGENTS.md: read-only) and
// cross-checks the result against an independently invoked ffprobe.
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <cmath>

#include "media/Probe.h"
#include "testsupport/MediaFixtures.h"

using namespace scrubtub;
using namespace scrubtub::testsupport;

namespace {

QByteArray makeProbeJson(const QJsonObject& stream, const QJsonObject& format = {})
{
    QJsonObject root;
    QJsonArray streams;
    streams.append(stream);
    root.insert(QStringLiteral("streams"), streams);
    if (!format.isEmpty())
        root.insert(QStringLiteral("format"), format);
    return QJsonDocument(root).toJson();
}

// ffprobe's own argument list, written out here rather than taken from
// Probe::arguments() so the runner's plumbing is verified against an
// independent invocation. Read-only.
QJsonObject rawProbe(const QString& ffprobe, const QString& path, bool* ok)
{
    QProcess process;
    process.start(ffprobe,
                  {QStringLiteral("-v"), QStringLiteral("error"),
                   QStringLiteral("-print_format"), QStringLiteral("json"),
                   QStringLiteral("-show_streams"), QStringLiteral("-show_format"), path});
    if (!process.waitForStarted(10000) || !process.waitForFinished(30000)
        || process.exitCode() != 0
        || process.exitStatus() != QProcess::NormalExit) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
}

// First real video stream, attached cover art excluded (the documented rule).
QJsonObject firstVideoStream(const QJsonObject& root)
{
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
    for (const QJsonValue& value : streams) {
        const QJsonObject stream = value.toObject();
        if (stream.value(QStringLiteral("codec_type")).toString() != QLatin1String("video"))
            continue;
        if (stream.value(QStringLiteral("disposition")).toObject()
                .value(QStringLiteral("attached_pic")).toInt(0) == 1)
            continue;
        return stream;
    }
    return {};
}

// ffprobe prints most numerics as strings; accept either form.
double number(const QJsonValue& value)
{
    if (value.isString())
        return value.toString().toDouble();
    return value.toDouble();
}

} // namespace

class TestProbe : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void videoStreamDurationWinsOverSubtitle();
    void attachedPictureStreamIsSkipped();
    void rotationSwapsDisplayDimensions();
    void noVideoStreamIsBadMedia();
    void malformedJsonIsBadMedia();
    void runOnRealCorpusFile();
    void hostileInputsAreBadMedia_data();
    void hostileInputsAreBadMedia();
    void cancelledProbeNeverStarts();
    void timeoutIsClassified();

private:
    QTemporaryDir m_temp;
    QString m_ffprobe;
    QString m_video;        // smallest corpus video, read-only
    QString m_synthetic;    // generated original; hostile cases mutate copies
    QString m_syntheticError;
    QString m_fingerprint;
};

void TestProbe::initTestCase()
{
    // Read-only guard: the corpus must be byte-identical afterwards.
    m_fingerprint = vidsFingerprint();

    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY2(!m_ffprobe.isEmpty(), "ffprobe required");

    m_video = smallestVideo(QStringLiteral("mp4"));
    if (m_video.isEmpty())
        m_video = smallestVideo();

    // Original clip for the hostile-input cases; the corpus is never touched.
    if (!makeSyntheticVideo(m_temp.path(), QStringLiteral("hostile-source.mp4"), 2000,
                            &m_syntheticError))
        m_synthetic.clear();
    else
        m_synthetic = m_temp.filePath(QStringLiteral("hostile-source.mp4"));
}

void TestProbe::cleanupTestCase()
{
    QCOMPARE(vidsFingerprint(), m_fingerprint);
}

void TestProbe::videoStreamDurationWinsOverSubtitle()
{
    // A long video stream with a longer mov_text subtitle stream: the probe
    // must use the video stream's duration and never the subtitle's.
    QJsonObject video;
    video.insert(QStringLiteral("codec_type"), QStringLiteral("video"));
    video.insert(QStringLiteral("index"), 0);
    video.insert(QStringLiteral("codec_name"), QStringLiteral("h264"));
    video.insert(QStringLiteral("width"), 1920);
    video.insert(QStringLiteral("height"), 1080);
    video.insert(QStringLiteral("duration"), 667.5);
    QJsonObject subtitle;
    subtitle.insert(QStringLiteral("codec_type"), QStringLiteral("subtitle"));
    subtitle.insert(QStringLiteral("index"), 2);
    subtitle.insert(QStringLiteral("duration"), 669.36);
    QJsonObject format;
    format.insert(QStringLiteral("duration"), 667.573696);

    QJsonObject root;
    QJsonArray streams{video, subtitle};
    root.insert(QStringLiteral("streams"), streams);
    root.insert(QStringLiteral("format"), format);

    const ProbeResult result = Probe::parse(QJsonDocument(root).toJson());
    QVERIFY(result.ok);
    QCOMPARE(result.durationMs, 667500);
    QVERIFY(result.durationMs < 669000); // not the subtitle duration
}

void TestProbe::attachedPictureStreamIsSkipped()
{
    // Cover art appears as a video stream with disposition attached_pic; the
    // real stream after it must be selected.
    QJsonObject cover;
    cover.insert(QStringLiteral("codec_type"), QStringLiteral("video"));
    cover.insert(QStringLiteral("index"), 0);
    cover.insert(QStringLiteral("codec_name"), QStringLiteral("mjpeg"));
    cover.insert(QStringLiteral("width"), 400);
    cover.insert(QStringLiteral("height"), 400);
    QJsonObject disposition;
    disposition.insert(QStringLiteral("attached_pic"), 1);
    cover.insert(QStringLiteral("disposition"), disposition);

    QJsonObject real;
    real.insert(QStringLiteral("codec_type"), QStringLiteral("video"));
    real.insert(QStringLiteral("index"), 1);
    real.insert(QStringLiteral("codec_name"), QStringLiteral("h264"));
    real.insert(QStringLiteral("width"), 1280);
    real.insert(QStringLiteral("height"), 720);
    real.insert(QStringLiteral("duration"), 12.5);

    QJsonObject root;
    QJsonArray streams{cover, real};
    root.insert(QStringLiteral("streams"), streams);

    const ProbeResult result = Probe::parse(QJsonDocument(root).toJson());
    QVERIFY(result.ok);
    QCOMPARE(result.selectedStreamIndex, 1);
    QCOMPARE(result.codedWidth, 1280);
    QCOMPARE(result.codedHeight, 720);
}

void TestProbe::rotationSwapsDisplayDimensions()
{
    QJsonObject stream;
    stream.insert(QStringLiteral("codec_type"), QStringLiteral("video"));
    stream.insert(QStringLiteral("index"), 0);
    stream.insert(QStringLiteral("codec_name"), QStringLiteral("h264"));
    stream.insert(QStringLiteral("width"), 1920);
    stream.insert(QStringLiteral("height"), 1080);
    QJsonArray sideData;
    QJsonObject matrix;
    matrix.insert(QStringLiteral("side_data_type"), QStringLiteral("Display Matrix"));
    matrix.insert(QStringLiteral("rotation"), -90);
    sideData.append(matrix);
    stream.insert(QStringLiteral("side_data_list"), sideData);

    const ProbeResult result = Probe::parse(makeProbeJson(stream));
    QVERIFY(result.ok);
    QCOMPARE(result.rotationDeg, 90);
    QCOMPARE(result.codedWidth, 1920);
    QCOMPARE(result.codedHeight, 1080);
    QCOMPARE(result.displayWidth, 1080);
    QCOMPARE(result.displayHeight, 1920);
}

void TestProbe::noVideoStreamIsBadMedia()
{
    QJsonObject audio;
    audio.insert(QStringLiteral("codec_type"), QStringLiteral("audio"));
    audio.insert(QStringLiteral("index"), 0);
    const ProbeResult result = Probe::parse(makeProbeJson(audio));
    QVERIFY(!result.ok);
    QCOMPARE(result.failKind, ProbeResult::FailKind::BadMedia);
}

void TestProbe::malformedJsonIsBadMedia()
{
    const ProbeResult result = Probe::parse("{not json");
    QVERIFY(!result.ok);
    QCOMPARE(result.failKind, ProbeResult::FailKind::BadMedia);
}

void TestProbe::runOnRealCorpusFile()
{
    if (m_video.isEmpty())
        QSKIP("vids/ corpus unavailable (set SCRUBTUB_TEST_VIDS_DIR)");

    const ProbeResult result = Probe::run(m_ffprobe, m_video);
    QVERIFY2(result.ok, qUtf8Printable(result.error));

    bool ok = false;
    const QJsonObject raw = rawProbe(m_ffprobe, m_video, &ok);
    QVERIFY2(ok, "reference ffprobe invocation failed");
    const QJsonObject stream = firstVideoStream(raw);
    QVERIFY2(!stream.isEmpty(), "reference ffprobe found no video stream");

    // Every reported value must equal what ffprobe reports for the same file.
    QCOMPARE(result.selectedStreamIndex, stream.value(QStringLiteral("index")).toInt());
    QCOMPARE(result.codec, stream.value(QStringLiteral("codec_name")).toString());
    QCOMPARE(result.codedWidth, stream.value(QStringLiteral("width")).toInt());
    QCOMPARE(result.codedHeight, stream.value(QStringLiteral("height")).toInt());
    QVERIFY(result.displayWidth > 0);
    QVERIFY(result.displayHeight > 0);

    // Duration: the selected video stream's duration, else the container's.
    const double streamDuration = number(stream.value(QStringLiteral("duration")));
    const double containerDuration = number(
        raw.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")));
    if (streamDuration > 0)
        QCOMPARE(result.durationMs, static_cast<qint64>(std::llround(streamDuration * 1000.0)));
    else if (containerDuration > 0)
        QCOMPARE(result.durationMs,
                 static_cast<qint64>(std::llround(containerDuration * 1000.0)));

    // Display dimensions equal the coded ones unless the display matrix is a
    // quarter turn (rotation is exercised above with synthetic JSON).
    QVERIFY(result.rotationDeg == 0 || result.rotationDeg == 90
            || result.rotationDeg == 180 || result.rotationDeg == 270);
    const bool swapped = result.rotationDeg == 90 || result.rotationDeg == 270;
    QCOMPARE(result.displayWidth, swapped ? result.codedHeight : result.codedWidth);
    QCOMPARE(result.displayHeight, swapped ? result.codedWidth : result.codedHeight);
    const QJsonArray sideData =
        stream.value(QStringLiteral("side_data_list")).toArray();
    bool rawHasRotation = false;
    for (const QJsonValue& value : sideData)
        rawHasRotation |= value.toObject().contains(QStringLiteral("rotation"));
    if (!rawHasRotation) {
        QCOMPARE(result.rotationDeg, 0);
        QCOMPARE(result.displayWidth, result.codedWidth);
        QCOMPARE(result.displayHeight, result.codedHeight);
    }
}

void TestProbe::hostileInputsAreBadMedia_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("truncated-clip") << QStringLiteral("truncated");
    QTest::newRow("non-media-bytes") << QStringLiteral("bytes");
    QTest::newRow("zero-length") << QStringLiteral("empty");
}

void TestProbe::hostileInputsAreBadMedia()
{
    QFETCH(QString, kind);
    const QString path = m_temp.filePath(QStringLiteral("hostile-%1.mp4").arg(kind));
    QFile::remove(path);

    if (kind == QLatin1String("truncated")) {
        // A copy of a generated original, cut in half: no moov atom left.
        if (m_synthetic.isEmpty())
            QSKIP(qPrintable(m_syntheticError));
        QFile source(m_synthetic);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray whole = source.readAll();
        source.close();
        QVERIFY(whole.size() > 1024);
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(whole.left(whole.size() / 2)), qint64(whole.size() / 2));
        out.close();
        // Vacuity guard: the intact original is readable media.
        QVERIFY(Probe::run(m_ffprobe, m_synthetic).ok);
    } else if (kind == QLatin1String("bytes")) {
        // Hostile bytes that are not media at all; nothing corpus-derived.
        const QByteArray junk(64 * 1024, '\x12');
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(junk), qint64(junk.size()));
        out.close();
    } else {
        // A zero-length file (no movie atom, no stream).
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.close();
        QCOMPARE(QFileInfo(path).size(), qint64(0));
    }

    const ProbeResult result = Probe::run(m_ffprobe, path);
    QVERIFY2(!result.ok, "hostile input must not probe successfully");
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.failKind == ProbeResult::FailKind::BadMedia
            || result.failKind == ProbeResult::FailKind::TransientIo);
}

void TestProbe::cancelledProbeNeverStarts()
{
    std::atomic_bool cancelled{true};
    QElapsedTimer timer;
    timer.start();
    const ProbeResult result = Probe::run(m_ffprobe,
                                          m_temp.filePath(QStringLiteral("unused.mp4")),
                                          30000, {}, &cancelled);
    QVERIFY(!result.ok);
    QCOMPARE(result.failKind, ProbeResult::FailKind::TransientIo);
    QVERIFY(result.error.contains(QLatin1String("cancel")));
    QVERIFY2(timer.elapsed() < 2000, "cancelled probe apparently ran to completion");
}

void TestProbe::timeoutIsClassified()
{
    // A stall is simulated deterministically: a "ffprobe" that sleeps forever.
    // The runner must enforce the hard timeout and classify it as Timeout.
    const QString script = m_temp.filePath(QStringLiteral("slow-probe.sh"));
    QFile sh(script);
    QVERIFY(sh.open(QIODevice::WriteOnly | QIODevice::Truncate));
    sh.write("#!/bin/sh\nsleep 30\n");
    sh.close();
    QVERIFY(QFile::setPermissions(script,
                                  QFile::ExeOwner | QFile::ReadOwner | QFile::WriteOwner));

    const ProbeResult result =
        Probe::run(script, m_temp.filePath(QStringLiteral("unused.mp4")), 2000);
    QCOMPARE(result.failKind, ProbeResult::FailKind::Timeout);
    QFile::remove(script);
}

QTEST_GUILESS_MAIN(TestProbe)
#include "tst_probe.moc"
