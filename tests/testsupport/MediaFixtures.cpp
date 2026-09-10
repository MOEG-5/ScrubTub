// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "MediaFixtures.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace scrubtub::testsupport {

namespace {

// Video extensions the scanner accepts (SourceScanner.h defaults).
const char* const kVideoSuffixes[] = {"mp4", "m4v", "mkv", "webm", "avi", "mov", "wmv", "flv"};

bool isVideoName(const QString& name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    for (const char* candidate : kVideoSuffixes)
        if (suffix == QLatin1String(candidate))
            return true;
    return false;
}

} // namespace

QString vidsDir()
{
    QString dir = qEnvironmentVariable("SCRUBTUB_TEST_VIDS_DIR");
#ifdef SCRUBTUB_TEST_VIDS_DIR_DEFAULT
    if (dir.isEmpty())
        dir = QStringLiteral(SCRUBTUB_TEST_VIDS_DIR_DEFAULT);
#endif
    if (dir.isEmpty())
        return {};
    const QDir absolute(dir);
    return absolute.exists() ? absolute.absolutePath() : QString();
}

bool vidsAvailable()
{
    return !smallestVideo().isEmpty();
}

QStringList vidsFiles()
{
    const QString dir = vidsDir();
    if (dir.isEmpty())
        return {};
    return QDir(dir).entryList(QDir::Files, QDir::Name);
}

QString smallestVideo(const QString& suffix)
{
    const QString dir = vidsDir();
    if (dir.isEmpty())
        return {};
    const QDir root(dir);
    QString best;
    qint64 bestSize = -1;
    for (const QString& name : root.entryList(QDir::Files, QDir::Name)) {
        if (!isVideoName(name))
            continue;
        if (!suffix.isEmpty() && QFileInfo(name).suffix().compare(suffix, Qt::CaseInsensitive) != 0)
            continue;
        const qint64 size = QFileInfo(root.filePath(name)).size();
        if (size <= 0)
            continue;
        if (bestSize < 0 || size < bestSize) {
            bestSize = size;
            best = root.filePath(name);
        }
    }
    return best;
}

QString videoNamed(const QString& fileName)
{
    const QString dir = vidsDir();
    if (dir.isEmpty())
        return {};
    const QString path = QDir(dir).filePath(fileName);
    return QFileInfo(path).isFile() ? path : QString();
}

QString vidsFingerprint()
{
    const QString dir = vidsDir();
    if (dir.isEmpty())
        return QStringLiteral("unavailable");
    const QDir root(dir);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString& name : root.entryList(QDir::Files, QDir::Name)) {
        const QFileInfo info(root.filePath(name));
        hash.addData(name.toUtf8());
        hash.addData(QByteArray::number(info.size()));
        hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool makeSyntheticVideo(const QString& directory, const QString& fileName,
                        int durationMs, QString* error)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        if (error)
            *error = QStringLiteral("ffmpeg is not installed; synthetic media unavailable");
        return false;
    }
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(directory);
        return false;
    }
    const QString path = QDir(directory).filePath(fileName);
    QFile::remove(path);
    const QString seconds = QString::number(durationMs / 1000.0, 'f', 3);
    QProcess process;
    process.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
                           QStringLiteral("-nostdin"),
                           QStringLiteral("-f"), QStringLiteral("lavfi"),
                           QStringLiteral("-i"),
                           QStringLiteral("testsrc2=size=320x240:rate=15:duration=") + seconds,
                           QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
                           QStringLiteral("-q:v"), QStringLiteral("6"),
                           QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                           QStringLiteral("-y"), path});
    if (!process.waitForStarted(10000) || !process.waitForFinished(60000)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error)
            *error = QStringLiteral("synthetic clip generation failed: %1")
                         .arg(QString::fromUtf8(process.readAllStandardError()).left(300));
        return false;
    }
    if (QFileInfo(path).size() <= 0) {
        if (error)
            *error = QStringLiteral("synthetic clip is empty: %1").arg(path);
        return false;
    }
    if (error)
        *error = QString();
    return true;
}

bool makeSyntheticVideos(const QString& directory, const QStringList& fileNames,
                         int durationMs, QString* error)
{
    for (const QString& name : fileNames)
        if (!makeSyntheticVideo(directory, name, durationMs, error))
            return false;
    return true;
}

} // namespace scrubtub::testsupport
