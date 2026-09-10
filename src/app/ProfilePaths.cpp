// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "ProfilePaths.h"

#include <QDir>
#include <QStandardPaths>

namespace scrubtub {

namespace {
QString joinProfile(const QString& base, const QString& profileId)
{
    QString clean = profileId;
    clean.remove('/').remove('\\').remove("..");
    return base + QStringLiteral("/profiles/") + clean;
}
} // namespace

QString ProfilePaths::profileDataDir(const QString& profileId)
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    // Keep existing libraries and annotations accessible after the rename.
    const QString legacy = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/itub-project/itub");
    if (!QDir(base).exists() && QDir(legacy).exists())
        return joinProfile(legacy, profileId);
    return joinProfile(base, profileId);
}

QString ProfilePaths::profileCacheDir(const QString& profileId)
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return joinProfile(base + QStringLiteral("/scrubtub"), profileId);
}

bool ProfilePaths::ensureDirs(const QString& profileId, QString* error)
{
    const QString dataDir = profileDataDir(profileId);
    const QString cacheDir = profileCacheDir(profileId);
    for (const QString& dir : {dataDir, cacheDir}) {
        if (!QDir().mkpath(dir)) {
            if (error)
                *error = QStringLiteral("Could not create app-owned directory: %1").arg(dir);
            return false;
        }
    }
    if (error)
        *error = QString();
    return true;
}

} // namespace scrubtub
