// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// App-owned storage locations (TECH_SPEC.md section 3): the database and cache
// live in fresh app-owned directories from QStandardPaths, never inside a
// catalogue root.
#pragma once

#include <QString>

namespace itub {

class ProfilePaths {
public:
    // Private profile directories; profileId distinguishes the disposable test
    // profile from the owner's normal profile. Never equal to or nested under a
    // media root (roots register themselves; see root validation in the
    // catalogue module).
    static QString profileDataDir(const QString& profileId = QStringLiteral("default"));
    static QString profileCacheDir(const QString& profileId = QStringLiteral("default"));

    // Creates the directories on a local filesystem and verifies writability.
    // Returns false with a human-readable reason on failure.
    static bool ensureDirs(const QString& profileId = QStringLiteral("default"),
                           QString* error = nullptr);
};

} // namespace itub
