// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Bounded disposable fixture corpus (PLAN.md "Agent media limits and privacy
// boundary"): at most 20 derived media files, at most 1 GiB of generated test
// data, created inside the caller's temporary directory. Enforced here so a
// runaway test cannot exceed the owner's limits.
#pragma once

#include <QDir>
#include <QString>

namespace itub::testsupport {

class FixtureCorpus {
public:
    static constexpr int kMaxMediaFiles = 20;
    static constexpr qint64 kMaxTotalBytes = 1024LL * 1024LL * 1024LL; // 1 GiB

    explicit FixtureCorpus(const QString& rootDir);

    // Corpora must live on a filesystem where the platform Trash works for
    // trash-mutation tests; /tmp mounts usually reject the trash root.
    static QString defaultBaseDir();

    // Copies sourceVideo into the corpus under the given name (reflink where
    // the filesystem supports it, plain copy otherwise). The source file is
    // only ever read. Fails once the media-file or byte budget is exhausted.
    bool addCopyOfMedia(const QString& sourceVideo, const QString& relativeName,
                        QString* error);

    // Non-media helper file (e.g. extension-candidate text file); counted
    // against the byte budget but not the media-file count.
    bool addTextFile(const QString& relativeName, const QByteArray& contents, QString* error);

    bool mkdir(const QString& relativeDir);

    QString root() const { return m_dir.absolutePath(); }
    QString mediaPath(const QString& relativeName) const
    {
        return m_dir.filePath(relativeName);
    }

private:
    bool accountBytes(qint64 bytes, QString* error);

    QDir m_dir;
    int m_mediaFiles = 0;
    qint64 m_bytes = 0;
};

} // namespace itub::testsupport
