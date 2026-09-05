// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "TreeSnapshot.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sys/stat.h>

namespace itub::testsupport {

namespace {

#ifdef Q_OS_UNIX
bool statMetadata(const QString& path, FileEntry* out)
{
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return false;
    out->size = static_cast<quint64>(st.st_size);
    out->mtimeSec = static_cast<qint64>(st.st_mtim.tv_sec);
    out->mtimeNsec = static_cast<qint64>(st.st_mtim.tv_nsec);
    out->mode = static_cast<quint32>(st.st_mode);
    out->isSymlink = S_ISLNK(st.st_mode);
    return true;
}
#else
bool statMetadata(const QString& path, FileEntry* out)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return false;
    out->size = static_cast<quint64>(info.size());
    out->mtimeSec = info.lastModified().toSecsSinceEpoch();
    out->mtimeNsec = 0;
    out->mode = 0;
    out->isSymlink = info.isSymLink();
    return true;
}
#endif

QByteArray hashFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buffer[64 * 1024];
    while (!file.atEnd()) {
        const qint64 read = file.read(buffer, sizeof(buffer));
        if (read <= 0)
            return {};
        hash.addData(QByteArrayView(buffer, static_cast<int>(read)));
    }
    return hash.result().toHex();
}

void walk(const QDir& dir, const QString& root, TreeSnapshot* out)
{
    const QString relDir = root.isEmpty() ? QString() : dir.absolutePath().mid(root.size() + 1);
    out->dirs.insert(relDir);

    const QFileInfoList children =
        dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                          QDir::Name | QDir::DirsFirst);
    QStringList childNames;
    for (const QFileInfo& child : children) {
        childNames << child.fileName();
        const QString relPath = relDir.isEmpty() ? child.fileName()
                                                 : relDir + QLatin1Char('/') + child.fileName();
        FileEntry entry;
        if (!statMetadata(child.absoluteFilePath(), &entry))
            continue;
        if (entry.isSymlink) {
            entry.linkTarget = child.symLinkTarget();
        } else if (child.isDir()) {
            walk(QDir(child.absoluteFilePath()), root, out);
            continue;
        } else if (child.isFile()) {
            entry.sha256 = hashFile(child.absoluteFilePath());
        }
        out->files.insert(relPath, entry);
    }
    out->dirEntries.insert(relDir, childNames);
}

} // namespace

bool snapshotTree(const QString& root, TreeSnapshot* out, QString* error)
{
    QFileInfo rootInfo(root);
    if (!rootInfo.isDir()) {
        if (error)
            *error = QStringLiteral("Not a directory: %1").arg(root);
        return false;
    }
    *out = TreeSnapshot{};
    walk(QDir(root), QDir(root).absolutePath(), out);
    if (error)
        *error = QString();
    return true;
}

QStringList diffTrees(const TreeSnapshot& before, const TreeSnapshot& after)
{
    QStringList diffs;
    for (auto it = before.files.constBegin(); it != before.files.constEnd(); ++it) {
        const auto afterIt = after.files.constFind(it.key());
        if (afterIt == after.files.constEnd()) {
            diffs << QStringLiteral("REMOVED file %1").arg(it.key());
            continue;
        }
        const FileEntry& b = it.value();
        const FileEntry& afterEntry = afterIt.value();
        if (b.size != afterEntry.size || b.mtimeSec != afterEntry.mtimeSec
            || b.mtimeNsec != afterEntry.mtimeNsec || b.mode != afterEntry.mode
            || b.isSymlink != afterEntry.isSymlink || b.linkTarget != afterEntry.linkTarget
            || b.sha256 != afterEntry.sha256) {
            diffs << QStringLiteral("CHANGED file %1").arg(it.key());
        }
    }
    for (auto it = after.files.constBegin(); it != after.files.constEnd(); ++it) {
        if (!before.files.contains(it.key()))
            diffs << QStringLiteral("ADDED file %1").arg(it.key());
    }
    for (const QString& dir : before.dirs) {
        if (!after.dirs.contains(dir))
            diffs << QStringLiteral("REMOVED dir %1").arg(dir);
        const QStringList beforeChildren = before.dirEntries.value(dir);
        if (after.dirEntries.value(dir) != beforeChildren)
            diffs << QStringLiteral("CHANGED dir listing %1").arg(dir);
    }
    for (const QString& dir : after.dirs) {
        if (!before.dirs.contains(dir))
            diffs << QStringLiteral("ADDED dir %1").arg(dir);
    }
    diffs.sort();
    return diffs;
}

} // namespace itub::testsupport
