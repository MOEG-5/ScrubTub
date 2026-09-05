// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Read-only recursive discovery (TECH_SPEC.md sections 3 and 5).
//
// This scanner never writes to the source tree: it only stats and enumerates.
// Symlinks, junctions, reparse points, and special files are never followed;
// symlinked directories are skipped and reported. Paths are preserved in their
// native spelling; names whose native encoding does not round-trip through the
// application's Unicode representation are skipped with a visible error.
#pragma once

#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

namespace itub {

struct DiscoveryOptions {
    QString rootPath;             // native absolute path; must not be a symlink
    bool includeHidden = false;   // include directories starting with '.' (POSIX)
    QStringList extensions = {QStringLiteral("mp4"), QStringLiteral("m4v"),
                              QStringLiteral("mkv"), QStringLiteral("webm"),
                              QStringLiteral("avi"), QStringLiteral("mov"),
                              QStringLiteral("wmv"), QStringLiteral("flv"),
                              QStringLiteral("mpg"), QStringLiteral("mpeg"),
                              QStringLiteral("ogv")};
};

struct DiscoveredFile {
    QString absolutePath;      // native spelling as presented to the app
    quint64 sizeBytes = 0;
    qint64 mtimeMs = -1;       // system-clock milliseconds; -1 when unavailable
};

struct DiscoveryIssue {
    QString path;              // item affected (root, directory, or file)
    QString message;
};

struct DiscoveryResult {
    QVector<DiscoveredFile> files;   // all files when no batch sink is used
    QVector<DiscoveryIssue> issues;  // non-fatal skips (permissions, symlinks, encodings)
    bool rootAccepted = false;
    bool completed = false;          // false when cancelled or aborted early
};

class SourceScanner {
public:
    // Maximum files per batch callback; a tuning default, not a constant
    // contract (TECH_SPEC.md section 5: 500).
    static constexpr int kBatchSize = 500;

    using BatchSink = std::function<void(QVector<DiscoveredFile>&& batch)>;

    // Enumerates options.rootPath recursively. Does not follow symlinks;
    // never writes; honors *cancel between entries. When sink is set, files
    // are delivered in bounded batches and Result.files stays empty.
    static DiscoveryResult enumerateRoot(const DiscoveryOptions& options,
                                         const std::atomic_bool& cancel,
                                         BatchSink sink = {});
};

} // namespace itub
