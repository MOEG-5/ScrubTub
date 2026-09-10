// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Tree snapshot and diff used by the no-source-writes contract check
// (TECH_SPEC.md section 12). Test support only; not linked into the product.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace scrubtub::testsupport {

struct FileEntry {
    quint64 size = 0;
    qint64 mtimeSec = 0;
    qint64 mtimeNsec = 0;
    quint32 mode = 0;
    bool isSymlink = false;
    QString linkTarget;
    QByteArray sha256; // regular files only
};

struct TreeSnapshot {
    QHash<QString, FileEntry> files;        // relative native path → entry
    QSet<QString> dirs;                     // relative directory paths
    QHash<QString, QStringList> dirEntries; // relative dir → sorted child names
};

// Recursively records metadata (size, nanosecond mtime, mode, SHA-256,
// symlink targets) and directory entry lists under root. Read-only.
bool snapshotTree(const QString& root, TreeSnapshot* out, QString* error);

// Human-readable differences before → after; empty means byte-identical
// metadata, identical contents, and unchanged directory listings.
QStringList diffTrees(const TreeSnapshot& before, const TreeSnapshot& after);

} // namespace scrubtub::testsupport
