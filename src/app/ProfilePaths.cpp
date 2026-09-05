// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "ProfilePaths.h"

#include <QDir>
#include <QStandardPaths>

namespace itub {

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
    return joinProfile(base, profileId);
}

QString ProfilePaths::profileCacheDir(const QString& profileId)
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return joinProfile(base + QStringLiteral("/itub"), profileId);
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

} // namespace itub
