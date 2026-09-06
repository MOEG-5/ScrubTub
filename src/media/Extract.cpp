// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "Extract.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace itub {

namespace {

constexpr int kMaxLogBytes = 64 * 1024; // §5: bounded stderr per failure

QStringList posterArguments(const QString& source, const QString& out, int streamIndex,
                            double seekSeconds, int targetWidth)
{
    QStringList args{QStringLiteral("-v"), QStringLiteral("error"),
                     QStringLiteral("-nostdin"),
                     QStringLiteral("-threads"), QStringLiteral("1")};
    if (seekSeconds > 0)
        args << QStringLiteral("-ss")
             << QString::number(seekSeconds, 'f', 3);
    args << QStringLiteral("-i") << source
         << QStringLiteral("-map") << QStringLiteral("0:%1").arg(streamIndex)
         << QStringLiteral("-an") << QStringLiteral("-sn")
         << QStringLiteral("-frames:v") << QStringLiteral("1")
         << QStringLiteral("-vf") << QStringLiteral("scale=%1:-2").arg(targetWidth)
         << QStringLiteral("-threads") << QStringLiteral("1")
         << QStringLiteral("-q:v") << QStringLiteral("3")
         << QStringLiteral("-y") << out;
    return args;
}

// Per-sample seek + one frame, with showinfo so the actual delivered
// timestamp can be recorded instead of the requested one (§6).
QStringList sampleArguments(const QString& source, const QString& out, int streamIndex,
                            double seekSeconds, int targetWidth)
{
    QStringList args{QStringLiteral("-v"), QStringLiteral("info"),
                     QStringLiteral("-nostdin"),
                     QStringLiteral("-threads"), QStringLiteral("1")};
    if (seekSeconds > 0)
        args << QStringLiteral("-ss") << QString::number(seekSeconds, 'f', 3);
    args << QStringLiteral("-i") << source
         << QStringLiteral("-map") << QStringLiteral("0:%1").arg(streamIndex)
         << QStringLiteral("-an") << QStringLiteral("-sn")
         << QStringLiteral("-frames:v") << QStringLiteral("1")
         << QStringLiteral("-vf")
         << QStringLiteral("scale=%1:-2,showinfo").arg(targetWidth)
         << QStringLiteral("-filter_threads") << QStringLiteral("1")
         << QStringLiteral("-threads") << QStringLiteral("1")
         << QStringLiteral("-f") << QStringLiteral("image2")
         << QStringLiteral("-y") << out;
    return args;
}

// Runs a process with the shared hard-timeout behavior; returns false on
// timeout. Output bound by kMaxLogBytes.
bool runBounded(const QString& program, const QStringList& args, int timeoutMs,
                QByteArray* stdOut, QByteArray* stdErr, bool* timedOut,
                const PidSink& pidSink = {}, const std::atomic_bool* cancelled = nullptr)
{
    if (cancelled && cancelled->load())
        return false;
    QProcess process;
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] { ::setsid(); });
#endif
    process.start(program, args);
    if (pidSink)
        pidSink(process.processId());
    *timedOut = false;
    if (!process.waitForStarted(10000)) {
        if (pidSink)
            pidSink(0);
        if (stdErr)
            *stdErr = process.errorString().toUtf8();
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        if (pidSink)
            pidSink(0);
#ifdef Q_OS_UNIX
        ::kill(-process.processId(), SIGTERM);
#else
        process.terminate();
#endif
        if (!process.waitForFinished(2000)) {
#ifdef Q_OS_UNIX
            ::kill(-process.processId(), SIGKILL);
#else
            process.kill();
#endif
            process.waitForFinished(2000);
        }
        *timedOut = true;
        return false;
    }
    if (pidSink)
        pidSink(0);
    if (stdOut)
        *stdOut = process.readAllStandardOutput();
    if (stdErr) {
        *stdErr = process.readAllStandardError();
        if (stdErr->size() > kMaxLogBytes)
            stdErr->truncate(kMaxLogBytes);
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

// Parses the last showinfo line's pts_time (seconds) from stderr.
qint64 deliveredPtsMs(const QByteArray& log)
{
    static const QRegularExpression re(
        QStringLiteral("showinfo.*pts_time:([0-9]+\\.?[0-9]*)"));
    qint64 ptsMs = -1;
    QRegularExpressionMatchIterator it = re.globalMatch(QString::fromUtf8(log));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        ptsMs = static_cast<qint64>(std::llround(m.captured(1).toDouble() * 1000.0));
    }
    return ptsMs;
}

} // namespace

QVector<qint64> Extract::samplePlanMs(qint64 durationMs, int sampleCount)
{
    QVector<qint64> plan;
    if (durationMs <= 0)
        return plan;
    // Fewer samples for very short clips (§6); keep at least two.
    qint64 count = sampleCount;
    if (durationMs < 12000)
        count = std::max<qint64>(2, durationMs / 500);
    count = std::min<qint64>(count, sampleCount);
    plan.reserve(static_cast<int>(count));
    for (qint64 k = 0; k < count; ++k)
        plan.append(durationMs * k / count);
    return plan;
}

ExtractResult Extract::poster(const PosterRequest& request, const PidSink& pidSink,
                              const std::atomic_bool* cancelled)
{
    qWarning("DBG Extract::poster enter: src=%s out=%s", qUtf8Printable(request.sourcePath), qUtf8Printable(request.outputPath));
    ExtractResult result;
    // 10 % of the duration; 0 s when unknown or very short (§6).
    const double seek =
        request.durationMs > 2000 ? request.durationMs * 0.10 / 1000.0 : 0.0;

    // App-owned temp destination; the artifact is renamed into place only
    // after validation (§5).
    const QString tempPath = request.outputPath + QStringLiteral(".tmp.jpg");
    QFile::remove(tempPath);
    QFile::remove(request.outputPath);
    bool timedOut = false;
    QByteArray stdErr;
    const bool ok = runBounded(request.ffmpegPath,
                               posterArguments(request.sourcePath, tempPath,
                                               request.selectedStreamIndex, seek,
                                               request.targetWidth),
                               request.timeoutMs, nullptr, &stdErr, &timedOut,
                               pidSink, cancelled);
    auto fail = [&](const QString& reason) {
        result.error = reason;
        QFile::remove(tempPath);
        return result;
    };
    if (cancelled && cancelled->load()) {
        result.timedOut = true;
        return fail(QStringLiteral("poster extraction cancelled"));
    }
    if (timedOut) {
        result.timedOut = true;
        return fail(QStringLiteral("poster extraction timed out"));
    }
    if (!ok) {
        return fail(QStringLiteral("poster extraction failed: %1")
                        .arg(QString::fromUtf8(stdErr.left(500))));
    }
    QImage image(tempPath);
    if (image.isNull())
        return fail(QStringLiteral("poster output is not a readable image"));
    if (!QFile::rename(tempPath, request.outputPath))
        return fail(QStringLiteral("could not move poster into the cache"));
    result.pixelSize = image.size();
    result.outputPath = request.outputPath;
    result.ok = true;
    return result;
}

ExtractResult Extract::storyboard(const StoryboardRequest& request, const PidSink& pidSink,
                                  const std::atomic_bool* cancelled)
{
    ExtractResult result;
    const QVector<qint64> plan = samplePlanMs(request.durationMs, request.sampleCount);
    if (plan.isEmpty()) {
        result.error = QStringLiteral("duration unknown; no storyboard plan");
        return result;
    }

    QDir().mkpath(request.outputDir);
    const QString framesDir = request.outputDir + QStringLiteral("/frames");
    QDir().mkpath(framesDir);

    QVector<qint64> actualTimes;
    for (int i = 0; i < plan.size(); ++i) {
        if (cancelled && cancelled->load()) {
            result.error = QStringLiteral("storyboard cancelled");
            break;
        }
        const QString framePath = framesDir + QStringLiteral("/sample-%1.jpg").arg(i, 3, 10, QLatin1Char('0'));
        QFile::remove(framePath);
        bool timedOut = false;
        QByteArray stdErr;
        const bool ok = runBounded(request.ffmpegPath,
                                   sampleArguments(request.sourcePath, framePath,
                                                   request.selectedStreamIndex,
                                                   plan.at(i) / 1000.0, request.tileWidth),
                                   request.timeoutMs, nullptr, &stdErr, &timedOut,
                                   pidSink, cancelled);
        if (timedOut) {
            result.timedOut = true;
            result.error = QStringLiteral("storyboard sample %1 timed out").arg(i);
            break;
        }
        if (!ok) {
            // A failure at one timestamp may leave a partial usable
            // storyboard (§6): stop expanding but keep collected samples.
            result.error = QStringLiteral("storyboard stopped at sample %1: %2")
                               .arg(i)
                               .arg(QString::fromUtf8(stdErr.left(300)));
            break;
        }
        const qint64 pts = deliveredPtsMs(stdErr);
        actualTimes.append(pts >= 0 ? pts : plan.at(i));
    }

    if (actualTimes.isEmpty()) {
        result.error = result.error.isEmpty()
            ? QStringLiteral("no storyboard samples extracted") : result.error;
        return result;
    }

    // Compose the atlas from the decoded tiles; one atlas at a time keeps
    // decoded RAM bounded (§6).
    QImage tile0(framesDir + QStringLiteral("/sample-000.jpg"));
    if (tile0.isNull()) {
        result.error = QStringLiteral("first storyboard tile unreadable");
        return result;
    }
    const int tileW = tile0.width();
    const int tileH = tile0.height();
    const int columns = qMax(1, request.columns);
    const int rows = (actualTimes.size() + columns - 1) / columns;
    QImage atlas(tileW * columns, tileH * rows, QImage::Format_RGB32);
    atlas.fill(Qt::black);
    QPainter painter(&atlas);
    for (int i = 0; i < actualTimes.size(); ++i) {
        QImage tile(framesDir + QStringLiteral("/sample-%1.jpg").arg(i, 3, 10, QLatin1Char('0')));
        if (tile.isNull())
            break;
        const int col = i % columns;
        const int row = i / columns;
        if (tile.size() != QSize(tileW, tileH))
            tile = tile.scaled(tileW, tileH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        painter.drawImage(col * tileW, row * tileH, tile);
    }
    painter.end();

    // Validated atlas: temp file, then atomic rename into the cache (§5).
    const QString atlasTemp = request.outputPath + QStringLiteral(".tmp.jpg");
    if (!atlas.save(atlasTemp, "jpg", 85)) {
        result.error = QStringLiteral("could not write storyboard atlas");
        QFile::remove(atlasTemp);
        return result;
    }
    if (!QFile::rename(atlasTemp, request.outputPath)) {
        QFile::remove(atlasTemp);
        result.error = QStringLiteral("could not move the atlas into the cache");
        return result;
    }

    result.outputPath = request.outputPath;
    result.sampleTimesMs = actualTimes;
    result.pixelSize = atlas.size();
    result.ok = true; // partial failures keep their reason in error

    // Intermediate frames are removed; the atlas is the owned artifact.
    QDir(framesDir).removeRecursively();
    return result;
}

} // namespace itub
