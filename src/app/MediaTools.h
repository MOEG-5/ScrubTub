// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace scrubtub {

// Private release tools take precedence over PATH; explicit overrides still win.
inline QString findMediaTool(const QString& name, const char* overrideVariable)
{
    const QByteArray overridePath = qgetenv(overrideVariable);
    if (!overridePath.isEmpty())
        return QString::fromLocal8Bit(overridePath);
    const QString directory = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("media"));
    const QString bundled = QStandardPaths::findExecutable(name, {directory});
    return bundled.isEmpty() ? QStandardPaths::findExecutable(name) : bundled;
}

} // namespace scrubtub
