// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Probe contract checks (TECH_SPEC.md section 12 "Extraction"):
// video-stream duration over subtitle duration, stream selection excluding
// attached pictures, rotation/display dimensions, corrupt input, timeouts.
// JSON parse cases are table-driven and need no media; the run case reads the
// supplied original fixture read-only.
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <sys/stat.h>
#include <sys/types.h>

#include "media/Probe.h"

using namespace scrubtub;

#ifdef SCRUBTUB_DEFAULT_SOURCE_FIXTURE
static QString fixturePath()
{
    const QByteArray env = qgetenv("SCRUBTUB_TEST_SOURCE_VIDEO");
    return env.isEmpty() ? QStringLiteral(SCRUBTUB_DEFAULT_SOURCE_FIXTURE)
                         : QString::fromLocal8Bit(env);
}
#endif

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

} // namespace

class TestProbe : public QObject {
    Q_OBJECT

private slots:
    void videoStreamDurationWinsOverSubtitle();
    void attachedPictureStreamIsSkipped();
    void rotationSwapsDisplayDimensions();
    void noVideoStreamIsBadMedia();
    void malformedJsonIsBadMedia();
    void runOnOriginalFixture();
    void corruptInputFails();
    void timeoutIsClassified();

private:
    QTemporaryDir m_temp;
};

void TestProbe::videoStreamDurationWinsOverSubtitle()
{
    // The supplied fixture reports a longer mov_text subtitle stream; the
    // probe must use the video stream's duration (667.500 s) and never the
    // subtitle or container value when a stream duration exists.
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

void TestProbe::runOnOriginalFixture()
{
#ifdef SCRUBTUB_DEFAULT_SOURCE_FIXTURE
    const QString source = fixturePath();
    if (source.isEmpty() || !QFileInfo::exists(source))
        QSKIP("Source fixture unavailable");
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY(!ffprobe.isEmpty());

    const ProbeResult result = Probe::run(ffprobe, source);
    QVERIFY2(result.ok, qUtf8Printable(result.error));
    // Video-stream duration (667.500 s), not subtitle (669.360 s) or
    // container (667.573696 s) — TECH_SPEC.md section 12.
    QCOMPARE(result.durationMs, 667500);
    QCOMPARE(result.codedWidth, 1920);
    QCOMPARE(result.codedHeight, 1080);
    QCOMPARE(result.displayWidth, 1920);
    QCOMPARE(result.displayHeight, 1080);
    QCOMPARE(result.rotationDeg, 0);
    QCOMPARE(result.codec, QStringLiteral("h264"));
#else
    QSKIP("No fixture compiled in");
#endif
}

void TestProbe::corruptInputFails()
{
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY(!ffprobe.isEmpty());

    // Truncated media (first 64 KiB of a synthetic header) is a bad-media
    // error, not a crash and not an endless retry.
    const QString path = m_temp.filePath(QStringLiteral("corrupt.mp4"));
    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(QByteArray(64 * 1024, '\x12'));
    out.close();

    const ProbeResult result = Probe::run(ffprobe, path);
    QVERIFY(!result.ok);
    QVERIFY(result.failKind == ProbeResult::FailKind::BadMedia
            || result.failKind == ProbeResult::FailKind::TransientIo);
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
