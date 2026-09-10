// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "FixtureCorpus.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace scrubtub::testsupport {

FixtureCorpus::FixtureCorpus(const QString& rootDir)
    : m_dir(rootDir)
{
    m_dir.mkpath(QStringLiteral("."));
}

QString FixtureCorpus::defaultBaseDir()
{
    const QString home =
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return home + QStringLiteral("/.scrubtub-agent-test-corpora");
}

bool FixtureCorpus::accountBytes(qint64 bytes, QString* error)
{
    if (m_bytes + bytes > kMaxTotalBytes) {
        if (error)
            *error = QStringLiteral("Test data budget exhausted (%1 bytes limit)")
                         .arg(kMaxTotalBytes);
        return false;
    }
    m_bytes += bytes;
    return true;
}

bool FixtureCorpus::addCopyOfMedia(const QString& sourceVideo, const QString& relativeName,
                                   QString* error)
{
    if (m_mediaFiles >= kMaxMediaFiles) {
        if (error)
            *error = QStringLiteral("Media file budget exhausted (%1 limit)")
                         .arg(kMaxMediaFiles);
        return false;
    }
    const QFileInfo sourceInfo(sourceVideo);
    if (!sourceInfo.isFile()) {
        if (error)
            *error = QStringLiteral("Source media is not a file: %1").arg(sourceVideo);
        return false;
    }
    const QString target = mediaPath(relativeName);
    QDir().mkpath(QFileInfo(target).absolutePath());

    // Prefer a copy-on-write reflink so the corpus does not consume real
    // storage; fall back to a plain copy. cp is used because Qt has no reflink
    // API; this only reads the source.
    bool copied = false;
    QProcess cp;
    cp.start(QStringLiteral("cp"),
             {QStringLiteral("--reflink=auto"), sourceInfo.absoluteFilePath(), target});
    if (cp.waitForStarted(5000) && cp.waitForFinished(120000))
        copied = cp.exitStatus() == QProcess::NormalExit && cp.exitCode() == 0;
    if (!copied)
        copied = QFile::copy(sourceInfo.absoluteFilePath(), target);
    if (!copied) {
        if (error)
            *error = QStringLiteral("Could not copy fixture to %1").arg(target);
        return false;
    }

    const QFileInfo targetInfo(target);
    if (!accountBytes(targetInfo.size(), error))
        return false;
    ++m_mediaFiles;
    if (error)
        *error = QString();
    return true;
}

bool FixtureCorpus::addTextFile(const QString& relativeName, const QByteArray& contents,
                                QString* error)
{
    const QString target = mediaPath(relativeName);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(target);
        return false;
    }
    file.write(contents);
    if (!accountBytes(contents.size(), error))
        return false;
    if (error)
        *error = QString();
    return true;
}

bool FixtureCorpus::mkdir(const QString& relativeDir)
{
    return m_dir.mkpath(relativeDir);
}

} // namespace scrubtub::testsupport
