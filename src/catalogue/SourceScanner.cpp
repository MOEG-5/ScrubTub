// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "SourceScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <chrono>
#include <filesystem>
#include <system_error>

namespace scrubtub {

namespace {

namespace fs = std::filesystem;

QString qstringFromFsPath(const fs::path& p)
{
    const std::u8string u8 = p.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(u8.data()),
                             static_cast<qsizetype>(u8.size()));
}

// True when the native path bytes convert to QString and back unchanged.
// On Linux the native encoding is bytes; anything that is not valid UTF-8
// would be silently corrupted by lossy conversion, so it must be skipped
// with a visible issue (TECH_SPEC.md section 3). Qt substitutes U+FFFD for
// every invalid sequence, so scanning for the replacement character is
// equivalent to the round-trip test and allocates nothing.
bool nativePathRoundTrips(const QString& converted)
{
#if defined(Q_OS_UNIX)
    return !converted.contains(QChar::ReplacementCharacter);
#else
    Q_UNUSED(converted);
    return true; // Windows: paths are UTF-16 natively; no lossy narrow conversion.
#endif
}

bool isHiddenName(const QString& name)
{
    return name.startsWith(QLatin1Char('.'));
}

// Suffix of a path segment, lowercased, without building a QFileInfo.
QString pathSuffix(const QString& path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot <= slash + 1 || dot == path.size() - 1)
        return {};
    return path.mid(dot + 1).toLower();
}

// Trailing path segment, without building a QFileInfo.
QString pathName(const QString& path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

qint64 mtimeMs(const fs::directory_entry& entry, std::error_code& ec)
{
    const auto fileTime = entry.last_write_time(ec);
    if (ec)
        return -1;
    const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(fileTime);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        sysTime.time_since_epoch()).count();
}

void deliver(QVector<DiscoveredFile>& pending, SourceScanner::BatchSink& sink)
{
    if (sink && !pending.isEmpty())
        sink(std::move(pending));
    pending.clear();
}

} // namespace

DiscoveryResult SourceScanner::enumerateRoot(const DiscoveryOptions& options,
                                             const std::atomic_bool& cancel,
                                             BatchSink sink)
{
    DiscoveryResult result;

    const QByteArray rootBytes = options.rootPath.toUtf8();
    const fs::path root(reinterpret_cast<const char8_t*>(rootBytes.constData()));

    std::error_code ec;
    const fs::file_status rootStatus = fs::symlink_status(root, ec);
    if (ec || !fs::exists(rootStatus)) {
        result.issues.push_back({options.rootPath,
                                 QStringLiteral("Root path does not exist or cannot be accessed")});
        return result;
    }
    if (fs::is_symlink(rootStatus)) {
        result.issues.push_back({options.rootPath,
                                 QStringLiteral("Root is a symlink; add the real directory instead")});
        return result;
    }
    if (!fs::is_directory(rootStatus)) {
        result.issues.push_back({options.rootPath, QStringLiteral("Root is not a directory")});
        return result;
    }
    result.rootAccepted = true;

    QSet<QString> lowerExtensions;
    for (const QString& ext : options.extensions)
        lowerExtensions.insert(ext.toLower());

    const bool allowHidden = options.includeHidden;
    QVector<DiscoveredFile> pending;

    try {
        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            result.issues.push_back({options.rootPath,
                                     QStringLiteral("Cannot enumerate root: %1")
                                         .arg(QString::fromStdString(ec.message()))});
            return result;
        }

        // recursive_directory_iterator without follow_directory_symlink never
        // descends into linked directories; every symlink is reported here so
        // the skip is visible (TECH_SPEC.md section 3).
        for (const fs::directory_entry& entry : it) {
            if (cancel.load(std::memory_order_relaxed))
                return result;

            const fs::path entryPath = entry.path();
            const QString qPath = qstringFromFsPath(entryPath);
            std::error_code statusEc;
            const fs::file_status status = entry.symlink_status(statusEc);
            if (statusEc) {
                result.issues.push_back({qPath, QStringLiteral("Cannot stat entry")});
                continue;
            }

            if (fs::is_symlink(status)) {
                result.issues.push_back({qPath, QStringLiteral("Symlink skipped")});
                it.disable_recursion_pending(); // harmless for files; keeps links unread
                continue;
            }

            if (fs::is_directory(status)) {
                if (!allowHidden && isHiddenName(pathName(qPath)))
                    it.disable_recursion_pending(); // do not descend into hidden dirs
                continue;
            }

            if (!fs::is_regular_file(status))
                continue; // devices, FIFOs, sockets: never read

            if (!nativePathRoundTrips(qPath)) {
                result.issues.push_back({qPath,
                                         QStringLiteral("Filename encoding is not supported; skipped")});
                continue;
            }

            const QString suffix = pathSuffix(qPath);
            if (!lowerExtensions.contains(suffix))
                continue;

            std::error_code statEc;
            const quint64 size = entry.file_size(statEc);
            if (statEc) {
                result.issues.push_back({qPath, QStringLiteral("Cannot stat file size")});
                continue;
            }
            const qint64 mtime = mtimeMs(entry, statEc);
            if (statEc)
                result.issues.push_back({qPath, QStringLiteral("Cannot stat file mtime")});

            DiscoveredFile file;
            file.absolutePath = qPath;
            file.sizeBytes = size;
            file.mtimeMs = mtime;
            if (sink) {
                pending.append(file);
                if (pending.size() >= kBatchSize)
                    deliver(pending, sink);
            } else {
                result.files.append(std::move(file));
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        result.issues.push_back({options.rootPath,
                                 QStringLiteral("Enumeration stopped: %1")
                                     .arg(QString::fromLocal8Bit(e.what()))});
        deliver(pending, sink);
        return result;
    }

    deliver(pending, sink);
    result.completed = !cancel.load(std::memory_order_relaxed);
    return result;
}

} // namespace scrubtub
