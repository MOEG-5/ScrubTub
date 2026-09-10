// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "Extract.h"

#include <QBuffer>
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace scrubtub {

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

QString samplePath(const QString& framesDir, int index)
{
    return framesDir + QStringLiteral("/sample-%1.jpg").arg(index, 3, 10, QLatin1Char('0'));
}

// A tile is only usable when the JPEG is complete. A process killed mid-write
// (timeout/cancel) leaves a truncated file behind; the end-of-image marker
// tells the complete tiles apart from that last partial one.
bool completeTile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 4)
        return false;
    return file.seek(file.size() - 2) && file.read(2) == QByteArray::fromHex("ffd9");
}

// One process produces every storyboard tile: one input per sample time (an
// input seek, so only the keyframe..sample span is decoded) and one JPEG
// output per sample. A single invocation removes the per-sample process,
// demuxer and decoder startup cost while keeping the same per-tile work.
QStringList storyboardArguments(const QString& source, const QString& framesDir,
                                int streamIndex, const QVector<qint64>& planMs,
                                int targetWidth, int threads)
{
    // -copyts keeps the original presentation timestamps: without it, the
    // input-seek shift re-bases the delivered pts_time to ~0 and the recorded
    // "actual" sample times would be wrong (§6: record actual timestamps).
    QStringList args{QStringLiteral("-v"), QStringLiteral("info"),
                     QStringLiteral("-nostdin"),
                     QStringLiteral("-copyts"),
                     QStringLiteral("-threads"), QString::number(threads)};
    for (const qint64 ms : planMs) {
        const double seekSeconds = ms / 1000.0;
        if (seekSeconds > 0)
            args << QStringLiteral("-ss") << QString::number(seekSeconds, 'f', 3);
        args << QStringLiteral("-i") << source;
    }
    // showinfo reports the delivered pts_time per tile (§6).
    QStringList graph;
    graph.reserve(planMs.size());
    for (int k = 0; k < planMs.size(); ++k) {
        graph << QStringLiteral("[%1:%2]scale=%3:%3:force_original_aspect_ratio=decrease"
                                ":force_divisible_by=2,showinfo[s%4]")
                     .arg(k).arg(streamIndex).arg(targetWidth).arg(k);
    }
    args << QStringLiteral("-filter_complex") << graph.join(QLatin1Char(';'));
    for (int k = 0; k < planMs.size(); ++k) {
        args << QStringLiteral("-map") << QStringLiteral("[s%1]").arg(k)
             << QStringLiteral("-an") << QStringLiteral("-sn")
             << QStringLiteral("-frames:v") << QStringLiteral("1")
             << QStringLiteral("-f") << QStringLiteral("image2")
             << QStringLiteral("-y") << samplePath(framesDir, k);
    }
    return args;
}

// Runs a process with the shared hard-timeout behavior; returns false on
// timeout or cancellation. Output bound by kMaxLogBytes.
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
    // The child is killed on timeout or cancellation; the process tree gets
    // SIGTERM, then SIGKILL if it lingers.
    const auto killTree = [&process] {
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
    };
    // Wait in slices so the cancellation flag terminates the running process
    // promptly (the old per-sample loop only checked between samples).
    QElapsedTimer waiting;
    waiting.start();
    while (!process.waitForFinished(100)) {
        if (cancelled && cancelled->load()) {
            if (pidSink)
                pidSink(0);
            killTree();
            return false;
        }
        if (waiting.elapsed() >= timeoutMs) {
            if (pidSink)
                pidSink(0);
            killTree();
            *timedOut = true;
            return false;
        }
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

// Delivered pts_time (ms) of the first frame each filter branch emits — the
// frame that actually ends up in that branch's tile. One entry per branch
// that produced a frame (§6: record actual timestamps, not requested ones).
QVector<qint64> deliveredPtsMs(const QByteArray& log)
{
    static const QRegularExpression re(
        QStringLiteral("showinfo.*\\bn:\\s*0\\s+pts:.*pts_time:([0-9]+\\.?[0-9]*)"));
    QVector<qint64> times;
    QRegularExpressionMatchIterator it = re.globalMatch(QString::fromUtf8(log));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        times.append(static_cast<qint64>(std::llround(m.captured(1).toDouble() * 1000.0)));
    }
    return times;
}

} // namespace

QVector<qint64> Extract::samplePlanMs(qint64 durationMs, int sampleCount)
{
    QVector<qint64> plan;
    if (durationMs <= 0 || sampleCount <= 0)
        return plan;
    // Fewer samples for very short clips (§6); keep at least two.
    qint64 count = sampleCount;
    if (durationMs < 12000)
        count = std::max<qint64>(2, durationMs / 500);
    count = std::min<qint64>(count, sampleCount);
    plan.reserve(static_cast<int>(count));
    for (qint64 k = 0; k < count; ++k)
        plan.append(durationMs * (2 * k + 1) / (2 * count));
    return plan;
}

ExtractResult Extract::poster(const PosterRequest& request, const PidSink& pidSink,
                              const std::atomic_bool* cancelled)
{
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
    QTemporaryDir temporaryFrames(request.outputDir + QStringLiteral("/frames-XXXXXX"));
    if (!temporaryFrames.isValid()) {
        result.error = QStringLiteral("could not create temporary preview directory");
        return result;
    }
    const QString framesDir = temporaryFrames.path();

    // One decode process per video; the hard timeout bounds the whole job.
    bool timedOut = false;
    QByteArray stdErr;
    // Bounded from the core count: storyboard jobs run at most two at a time,
    // so half the cores per process keeps the machine busy without
    // oversubscribing it (§5).
    const int threads = std::clamp(QThread::idealThreadCount() / 2, 2, 8);
    const bool ok = runBounded(request.ffmpegPath,
                               storyboardArguments(request.sourcePath, framesDir,
                                                   request.selectedStreamIndex, plan,
                                                   request.tileWidth, threads),
                               request.timeoutMs, nullptr, &stdErr, &timedOut,
                               pidSink, cancelled);

    // Tiles that actually reached disk. A sample ffmpeg could not produce is
    // skipped and the earlier ones still compose a usable partial atlas (§6).
    QVector<int> produced;
    for (int i = 0; i < plan.size(); ++i) {
        if (completeTile(samplePath(framesDir, i)))
            produced.append(i);
    }
    int firstMissing = plan.size();
    for (int i = 0; i < plan.size(); ++i) {
        if (i >= produced.size() || produced.at(i) != i) {
            firstMissing = i;
            break;
        }
    }

    if (cancelled && cancelled->load())
        result.error = QStringLiteral("storyboard cancelled");
    else if (timedOut) {
        result.timedOut = true;
        result.error = QStringLiteral("storyboard sample %1 timed out").arg(firstMissing);
    } else if (!ok) {
        result.error = QStringLiteral("storyboard stopped at sample %1: %2")
                           .arg(firstMissing)
                           .arg(QString::fromUtf8(stdErr.left(300)));
    }

    if (produced.isEmpty()) {
        if (result.error.isEmpty())
            result.error = QStringLiteral("no storyboard samples extracted");
        return result;
    }

    // showinfo lines interleave across the branches, but both the tile order
    // and the delivered timestamps are ascending, so sorting restores the
    // per-sample mapping. A count mismatch (e.g. a truncated log) falls back
    // to the requested times rather than mis-assigning actual ones.
    QVector<qint64> delivered = deliveredPtsMs(stdErr);
    std::sort(delivered.begin(), delivered.end());
    const bool timesAvailable = delivered.size() == produced.size();

    QVector<qint64> times;
    QVector<QImage> tiles;
    times.reserve(produced.size());
    tiles.reserve(produced.size());
    for (int j = 0; j < produced.size(); ++j) {
        const int i = produced.at(j);
        const QImage tile(samplePath(framesDir, i));
        if (tile.isNull())
            continue; // unreadable tile: drop it with its timestamp
        tiles.append(tile);
        times.append(timesAvailable ? delivered.at(j) : plan.at(i));
    }
    if (tiles.isEmpty()) {
        result.error = QStringLiteral("storyboard tiles unreadable");
        return result;
    }

    // Compose the atlas from the decoded tiles; one atlas at a time keeps
    // decoded RAM bounded (§6).
    const QImage& tile0 = tiles.first();
    const int tileW = tile0.width();
    const int tileH = tile0.height();
    const int columns = qMax(1, request.columns);
    const int rows = (tiles.size() + columns - 1) / columns;
    QImage atlas(tileW * columns, tileH * rows, QImage::Format_RGB32);
    atlas.fill(Qt::black);
    QPainter painter(&atlas);
    for (int i = 0; i < tiles.size(); ++i) {
        QImage tile = tiles.at(i);
        const int col = i % columns;
        const int row = i / columns;
        if (tile.size() != QSize(tileW, tileH))
            tile = tile.scaled(tileW, tileH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        painter.drawImage(col * tileW, row * tileH, tile);
    }
    painter.end();

    // Validated atlas: temp file, then atomic rename into the cache (§5).
    // A stale artifact at the destination makes QFile::rename fail — remove
    // it first (the cache row for this revision is being replaced anyway).
    const QString atlasTemp = request.outputPath + QStringLiteral(".tmp.jpg");
    QByteArray encoded;
    int quality = 65;
    for (;;) {
        encoded.clear();
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        if (!atlas.save(&buffer, "jpg", quality)) {
            result.error = QStringLiteral("could not encode preview atlas");
            return result;
        }
        if (encoded.size() <= kSparsePreviewMaxBytes)
            break;
        if (quality > 35) {
            quality -= 10;
        } else {
            // Keep tile boundaries integral when exceptionally noisy images
            // need a smaller canvas to fit the per-video storage ceiling.
            atlas = atlas.scaled(columns * qMax(1, atlas.width() / columns / 2),
                                 rows * qMax(1, atlas.height() / rows / 2),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            quality = 65;
        }
    }
    QFile output(atlasTemp);
    if (!output.open(QIODevice::WriteOnly) || output.write(encoded) != encoded.size()) {
        result.error = QStringLiteral("could not write preview atlas");
        output.close();
        QFile::remove(atlasTemp);
        return result;
    }
    output.close();
    QFile::remove(request.outputPath);
    if (!QFile::rename(atlasTemp, request.outputPath)) {
        QFile::remove(atlasTemp);
        result.error = QStringLiteral("could not move the atlas into the cache");
        return result;
    }

    result.outputPath = request.outputPath;
    result.sampleTimesMs = times;
    result.pixelSize = atlas.size();
    result.ok = true; // partial failures keep their reason in error

    // Intermediate frames are removed; the atlas is the owned artifact.
    QDir(framesDir).removeRecursively();
    return result;
}

} // namespace scrubtub
