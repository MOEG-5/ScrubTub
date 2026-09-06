// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// VideoRow and shared row types used across catalogue, UI, and tests.
#pragma once

#include <QMetaType>
#include <QString>

namespace itub {

struct VideoRow {
    qint64 id = 0;
    qint64 rootId = 0;
    QString relPath;
    QString fileName;
    qint64 sizeBytes = -1;      // -1 = unknown
    qint64 mtimeMs = -1;
    qint64 revision = 1;
    qint64 durationMs = -1;     // -1 = unknown
    int displayWidth = 0;
    int displayHeight = 0;
    QString codec;
    int rating = 0;             // 0 = unrated; 1..5 otherwise
    qint64 views = 0;
    qint64 addedMs = 0;
    QString availability;       // unprobed|available|missing|unavailable
    QString probeStatus;        // pending|ok|error|timeout
};
struct RootInfo {
    qint64 id = 0;
    QString path;
    QString status;
    bool includeHidden = false;
};

struct ScanProgress {
    qint64 rootId = 0;
    QString rootPath;
    QString state;          // idle|enumerating|probing|paused|complete|cancelled|missing
    quint64 discovered = 0;
    quint64 probed = 0;
    quint64 errors = 0;
};

} // namespace itub

Q_DECLARE_METATYPE(itub::VideoRow)
Q_DECLARE_METATYPE(itub::RootInfo)
Q_DECLARE_METATYPE(itub::ScanProgress)
