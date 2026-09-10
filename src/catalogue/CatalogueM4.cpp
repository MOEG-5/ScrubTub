// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Milestone-4 catalogue layer: explicit Trash action, catalogue backup and
// restore, cache controls, and UI settings persistence. Included by
// Catalogue.cpp; every function runs on the catalogue thread
// (TECH_SPEC.md sections 3, 4, 6, 10).
#include "Catalogue.h"
#include "Database.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

namespace scrubtub {

namespace {
// Settings persisted across restarts (§10): useful UI state only.
const char* kSettingCardSize = "ui_card_size";
const char* kSettingSortKey = "ui_sort_key";
const char* kSettingSortDesc = "ui_sort_desc";
const char* kSettingCachedOnly = "ui_cached_only";
const char* kSettingStartupRefresh = "ui_startup_refresh";
const char* kSettingDiskCacheBytes = "disk_cache_bytes";
} // namespace

// --------------------------- explicit deletion (§3) -------------------------

void Catalogue::trashVideos(const QList<qint64>& videoIds)
{
    for (const qint64 videoId : videoIds) {
        Statement info = m_db->prepare(
            "SELECT v.rel_path, r.path, v.size_bytes, v.mtime_ms, v.revision "
            "FROM videos v JOIN roots r ON r.id = v.root_id WHERE v.id=?");
        info.bind(1, videoId);
        if (!info.step()) {
            emit trashResult(videoId, false, QStringLiteral("Unknown video"));
            continue;
        }
        const QString relPath = info.text(0);
        const QString rootPath = info.text(1);
        const qint64 sizeBytes = info.isNull(2) ? -1 : info.int64(2);
        const qint64 mtimeMs = info.isNull(3) ? -1 : info.int64(3);
        const QString absPath = rootPath + QLatin1Char('/') + relPath;

        // Re-stat and verify identity immediately before the operation (§3).
        const QFileInfo fileInfo(absPath);
        if (!fileInfo.isFile() || fileInfo.isSymLink()) {
            emit trashResult(videoId, false,
                             QStringLiteral("File is not a regular file: %1").arg(absPath));
            continue;
        }
        if (sizeBytes >= 0
            && (static_cast<qint64>(fileInfo.size()) != sizeBytes
                || fileInfo.lastModified().toMSecsSinceEpoch() != mtimeMs)) {
            // The file changed or became a link since confirmation: abort
            // this item and ask for a fresh action (§3).
            emit trashResult(videoId, false,
                             QStringLiteral("File changed since confirmation; "
                                            "refresh and confirm again"));
            continue;
        }

        // Tear down any hover session touching this file before removal:
        // handled UI-side; here the path is only read via the platform call.
        // QFile::moveToTrash uses the platform trash facility where
        // available. A final path/identity race is a known platform
        // limitation: the window between check and call is kept short and
        // failures fail closed (§3).
        const bool trashed = QFile::moveToTrash(absPath);
        if (!trashed) {
            // Failure MUST leave the catalogue entry and show the reason;
            // no fallback to permanent deletion (§3).
            emit trashResult(videoId, false,
                             QStringLiteral("Could not move to Trash: %1").arg(absPath));
            continue;
        }

        // The media is gone from its old path; annotations are preserved and
        // the row reports missing until an explicit purge (§3).
        m_db->transaction([this, videoId] {
            Statement st = m_db->prepare(
                "UPDATE videos SET availability='missing' WHERE id=?");
            st.bind(1, videoId);
            st.run();
            Statement delJobs = m_db->prepare(
                "DELETE FROM jobs WHERE video_id=? AND state='queued'");
            delJobs.bind(1, videoId);
            delJobs.run();
            return true;
        });
        refreshSearchRecord(videoId);
        emitRows(QList<VideoRow>{readRow(videoId)});
        emit trashResult(videoId, true, QString());
    }
}

// --------------------------- backup and restore (§4) ------------------------

void Catalogue::exportBackup(const QString& destPathIn)
{
    const QString destPath = localPathFromUserInput(destPathIn);
    const QFileInfo destInfo(destPath);
    if (destInfo.exists() && !destInfo.isFile()) {
        emit operationFailed(QStringLiteral("Backup destination is not a file"));
        return;
    }
    // App-owned temp destination, then atomic rename (§5).
    const QString tempPath = destPath + QStringLiteral(".scrubtub-tmp");
    QFile::remove(tempPath);
    QString error;
    if (!m_db->backupTo(tempPath, &error)) {
        QFile::remove(tempPath);
        emit operationFailed(QStringLiteral("Backup failed: %1").arg(error));
        return;
    }
    if (destInfo.exists())
        QFile::remove(destPath); // overwrite was confirmed in the UI
    if (!QFile::rename(tempPath, destPath)) {
        QFile::remove(tempPath);
        emit operationFailed(QStringLiteral("Could not move the backup into place"));
        return;
    }
    emit backupExported(destPath);
}

void Catalogue::importBackup(const QString& srcPathIn)
{
    const QString srcPath = localPathFromUserInput(srcPathIn);
    const QFileInfo srcInfo(srcPath);
    if (!srcInfo.isFile()) {
        emit operationFailed(QStringLiteral("Backup file not found: %1").arg(srcPath));
        return;
    }

    // Validate on an app-owned copy before touching the active database (§4).
    const QString stagingPath = m_profileDataDir + QStringLiteral("/restore-staging.db");
    QFile::remove(stagingPath);
    if (!QFile::copy(srcPath, stagingPath)) {
        emit operationFailed(QStringLiteral("Could not stage the backup for validation"));
        return;
    }
    {
        Database staged;
        QString error;
        if (!staged.open(stagingPath, &error)) {
            QFile::remove(stagingPath);
            emit operationFailed(QStringLiteral("Backup is not a valid database: %1")
                                     .arg(error));
            return;
        }
        const auto version = staged.scalarInt(
            "SELECT value FROM meta WHERE key='schema_version'");
        if (!version.has_value()) {
            QFile::remove(stagingPath);
            emit operationFailed(QStringLiteral("Backup has no schema version"));
            return;
        }
        if (version.value() > Database::kSchemaVersion) {
            QFile::remove(stagingPath);
            emit operationFailed(QStringLiteral(
                "Backup schema v%1 is newer than supported v%2")
                .arg(version.value()).arg(Database::kSchemaVersion));
            return;
        }
        const QString integrity = staged.integrityCheckError();
        if (!integrity.isEmpty()) {
            QFile::remove(stagingPath);
            emit operationFailed(QStringLiteral("Backup failed integrity check: %1")
                                     .arg(integrity));
            return;
        }
    }

    // Preserve the current database as the pre-restore backup (§4).
    QString preserveError;
    const QString preservePath = m_profileDataDir + QStringLiteral("/catalogue.db.pre-restore");
    QFile::remove(preservePath);
    if (!m_db->backupTo(preservePath, &preserveError)) {
        QFile::remove(stagingPath);
        emit operationFailed(QStringLiteral("Could not preserve the current database: %1")
                                 .arg(preserveError));
        return;
    }

    QString restoreError;
    if (!m_db->restoreFrom(stagingPath, &restoreError)) {
        QFile::remove(stagingPath);
        emit operationFailed(QStringLiteral("Restore failed: %1").arg(restoreError));
        return;
    }
    QFile::remove(stagingPath);
    m_searchRecords.clear();
    emit rowsChanged(QList<VideoRow>{}, true);
    emit backupImported();
    loadExistingState();
}

// --------------------------- cache controls (§6) ----------------------------

// Removes cached artifact files that no longer correspond to a cache entry
// (root removals, missing-video purges, interrupted extractions). Only files
// inside the app-owned thumbs directory are ever deleted (§3, §6).
void Catalogue::purgeOrphanedCacheFiles()
{
    QDir cacheDir(m_cacheDir);
    if (!cacheDir.exists())
        return;
    QSet<QString> tracked;
    {
        Statement st = m_db->prepare("SELECT rel_path FROM cache_entries");
        while (st.step())
            tracked.insert(st.text(0));
    }
    const QStringList entries =
        cacheDir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString& name : entries) {
        if (!tracked.contains(name)) {
            // Owned artifacts match the "<video>-<revision>-<profile>.jpg"
            // pattern; anything else in this directory is not ours to touch.
            if (name.endsWith(QLatin1String(".jpg"))
                && QRegularExpression(QStringLiteral(R"(^\d+-\d+-[a-z0-9-]+\.jpg$)"))
                       .match(name)
                       .hasMatch())
                QFile::remove(m_cacheDir + QLatin1Char('/') + name);
        }
    }
    const QStringList tempDirs =
        cacheDir.entryList(QStringList{QStringLiteral("tmp-*")}, QDir::Dirs);
    for (const QString& dir : tempDirs)
        QDir(m_cacheDir + QLatin1Char('/') + dir).removeRecursively();
}

// Drops cache entries and their files for videos that no longer resolve
// (missing after a complete enumeration, or removed roots).
void Catalogue::purgeCacheForVideos(const QList<qint64>& videoIds)
{
    if (videoIds.isEmpty())
        return;
    QStringList relPaths;
    m_db->transaction([this, videoIds, &relPaths] {
        for (const qint64 id : videoIds) {
            Statement sel = m_db->prepare(
                "SELECT rel_path FROM cache_entries WHERE video_id=?");
            sel.bind(1, id);
            while (sel.step())
                relPaths.append(sel.text(0));
            Statement del = m_db->prepare("DELETE FROM cache_entries WHERE video_id=?");
            del.bind(1, id);
            del.run();
        }
        return true;
    });
    for (const QString& rel : relPaths)
        QFile::remove(m_cacheDir + QLatin1Char('/') + rel);
    if (!relPaths.isEmpty())
        m_cacheBytes = -1; // unknown; recomputed on the next check
}

void Catalogue::clearPreviews()
{
    m_sparsePassQueued = false;
    // Remove only owned artifacts: rows first (paths), then the files, then
    // any leftover temp directories from interrupted extractions.
    QStringList relPaths;
    m_db->transaction([this, &relPaths] {
        Statement st = m_db->prepare("SELECT rel_path FROM cache_entries");
        while (st.step())
            relPaths.append(st.text(0));
        Statement del = m_db->prepare("DELETE FROM cache_entries");
        del.run();
        return true;
    });
    for (const QString& rel : relPaths)
        QFile::remove(m_cacheDir + QLatin1Char('/') + rel);
    QDir cacheDir(m_cacheDir);
    const QStringList tempDirs =
        cacheDir.entryList(QStringList{QStringLiteral("tmp-*")}, QDir::Dirs);
    for (const QString& dir : tempDirs)
        QDir(m_cacheDir + QLatin1Char('/') + dir).removeRecursively();
    m_cacheBytes = 0;
    emit rowsChanged(QList<VideoRow>{}, true);
    loadExistingState();
    emit cacheUsageReady(0);
}

void Catalogue::requestCacheUsage()
{
    Statement st = m_db->prepare("SELECT COALESCE(SUM(bytes),0) FROM cache_entries");
    st.step();
    m_cacheBytes = st.int64(0);
    emit cacheUsageReady(m_cacheBytes);
}

// --------------------------- settings persistence (§10) ---------------------

void Catalogue::restoreSettings()
{
    QVariantMap settings;
    settings.insert(QStringLiteral("cardSize"),
                    static_cast<int>(settingInt(kSettingCardSize, 240)));
    settings.insert(QStringLiteral("sortKey"), settingText(kSettingSortKey, QStringLiteral("added")));
    settings.insert(QStringLiteral("sortDescending"),
                    settingInt(kSettingSortDesc, 0) != 0);
    settings.insert(QStringLiteral("cachedOnly"),
                    settingInt(kSettingCachedOnly, 0) != 0);
    settings.insert(QStringLiteral("startupRefresh"),
                    settingInt(kSettingStartupRefresh, 1) != 0);
    emit settingsReady(settings);
}

qint64 Catalogue::settingInt(const char* key, qint64 fallback) const
{
    Statement st = m_db->prepare("SELECT value FROM settings WHERE key=?");
    st.bind(1, QString::fromLatin1(key));
    if (!st.step())
        return fallback;
    bool ok = false;
    const qint64 value = st.text(0).toLongLong(&ok);
    return ok ? value : fallback;
}

void Catalogue::setSettingInt(const char* key, qint64 value)
{
    Statement st = m_db->prepare(
        "INSERT OR REPLACE INTO settings(key, value, version) VALUES(?,?,1)");
    st.bind(1, QString::fromLatin1(key));
    st.bind(2, QString::number(value));
    st.run();
}

void Catalogue::saveUiSettingsMap(const QVariantMap& settings)
{
    if (settings.contains(QStringLiteral("cardSize")))
        setSettingInt(kSettingCardSize, settings.value(QStringLiteral("cardSize")).toInt());
    if (settings.contains(QStringLiteral("sortKey")))
        setSettingText(kSettingSortKey, settings.value(QStringLiteral("sortKey")).toString());
    if (settings.contains(QStringLiteral("sortDescending")))
        setSettingInt(kSettingSortDesc,
                      settings.value(QStringLiteral("sortDescending")).toBool() ? 1 : 0);
    if (settings.contains(QStringLiteral("cachedOnly")))
        setSettingInt(kSettingCachedOnly,
                      settings.value(QStringLiteral("cachedOnly")).toBool() ? 1 : 0);
    if (settings.contains(QStringLiteral("startupRefresh")))
        setSettingInt(kSettingStartupRefresh,
                      settings.value(QStringLiteral("startupRefresh")).toBool() ? 1 : 0);
    if (settings.contains(QStringLiteral("diskCacheBytes")))
        setSettingInt(kSettingDiskCacheBytes,
                      settings.value(QStringLiteral("diskCacheBytes")).toLongLong());
}

QString Catalogue::settingText(const char* key, const QString& fallback) const
{
    Statement st = m_db->prepare("SELECT value FROM settings WHERE key=?");
    st.bind(1, QString::fromLatin1(key));
    return st.step() ? st.text(0) : fallback;
}

void Catalogue::setSettingText(const char* key, const QString& value)
{
    Statement st = m_db->prepare(
        "INSERT OR REPLACE INTO settings(key, value, version) VALUES(?,?,1)");
    st.bind(1, QString::fromLatin1(key));
    st.bind(2, value);
    st.run();
}

} // namespace scrubtub
