// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "app/ProfilePaths.h"
#include "catalogue/GridModel.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("itub-project"));
    QGuiApplication::setApplicationName(QStringLiteral("itub"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Video Catalogue"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // App-owned profile directories must exist and be writable before any
    // catalogue work; they live under QStandardPaths, never inside a media root
    // (TECH_SPEC.md section 3).
    QString profileError;
    if (!itub::ProfilePaths::ensureDirs(QStringLiteral("default"), &profileError)) {
        qCritical("Profile directories unusable: %s", qUtf8Printable(profileError));
        return 1;
    }

    itub::GridModel gridModel(2000);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("gridModel"), &gridModel);
    engine.loadFromModule("itub.ui", "Main");
    if (engine.rootObjects().isEmpty()) {
        qCritical("Failed to load the main QML module");
        return 1;
    }
    return app.exec();
}
