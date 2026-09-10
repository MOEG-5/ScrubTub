// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QThread>

#include <cstdio>

#include <QQmlEngine>

#include "app/ProfilePaths.h"
#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/CatalogueProxy.h"
#include "media/HoverFrameItem.h"
#include "media/HoverSession.h"
#include "media/PreviewProvider.h"
#include "media/Extract.h"

namespace {

QFile* g_logFile = nullptr;

// Qt routes warnings to the system journal when stderr is not a TTY; the
// product needs a deterministic, app-owned log (TECH_SPEC.md section 10:
// expose errors). stderr keeps receiving output when it exists.
void fileMessageHandler(QtMsgType type, const QMessageLogContext& context,
                        const QString& message)
{
    const char* level = "debug";
    switch (type) {
    case QtInfoMsg: level = "info"; break;
    case QtWarningMsg: level = "warning"; break;
    case QtCriticalMsg: level = "critical"; break;
    case QtFatalMsg: level = "fatal"; break;
    case QtDebugMsg: break;
    }
    const QByteArray line =
        QStringLiteral("[%1] %2\n").arg(QLatin1String(level), message).toUtf8();
    if (g_logFile && g_logFile->isOpen()) {
        g_logFile->write(line);
        g_logFile->flush();
    }
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
    if (type == QtFatalMsg)
        abort();
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    // Convenience for tests and first-run automation: add a root at startup.
    QString autoRoot;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--add-root") && i + 1 < args.size())
            autoRoot = args.at(++i);
    }
    QGuiApplication::setOrganizationName(QStringLiteral("scrubtub-project"));
    QGuiApplication::setApplicationName(QStringLiteral("scrubtub"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("ScrubTub"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // App-owned profile directories live under QStandardPaths, never inside a
    // media root (TECH_SPEC.md section 3).
    QString profileError;
    if (!scrubtub::ProfilePaths::ensureDirs(QStringLiteral("default"), &profileError)) {
        qFatal("Profile directories unusable: %s", qUtf8Printable(profileError));
        return 1;
    }

    QFile logFile(scrubtub::ProfilePaths::profileCacheDir() + QStringLiteral("/scrubtub.log"));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        g_logFile = &logFile;
    qInstallMessageHandler(fileMessageHandler);

    auto* catalogue = new scrubtub::Catalogue;
    auto* catalogueModel = new scrubtub::CatalogueModel(&app);
    scrubtub::CatalogueProxy proxy(catalogue, catalogueModel, &app);

    auto* catalogueThread = new QThread(&app);
    catalogue->moveToThread(catalogueThread);
    QObject::connect(catalogueThread, &QThread::finished, catalogue, &QObject::deleteLater);
    catalogueThread->start();

    // ffprobe from PATH or the bundled directory; a missing tool surfaces as a
    // visible error on scan, not a silent failure.
    const QString ffprobePath = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffprobePath.isEmpty() || ffmpegPath.isEmpty())
        qWarning("ffprobe/ffmpeg not found on PATH: metadata extraction will fail "
                 "until a bundled copy is available");

    QString initError;
    bool initialized = false;
    QMetaObject::invokeMethod(
        catalogue,
        [&] { initialized = catalogue->initialize(scrubtub::ProfilePaths::profileDataDir(),
                                                  ffprobePath, ffmpegPath, &initError); },
        Qt::BlockingQueuedConnection);
    if (!initialized) {
        qCritical("Catalogue unavailable: %s", qUtf8Printable(initError));
        catalogueThread->quit();
        catalogueThread->wait();
        return 1;
    }

    // Rows and progress cross threads as queued signal deliveries.
    QObject::connect(catalogue, &scrubtub::Catalogue::rowsChanged,
                     catalogueModel, &scrubtub::CatalogueModel::applyRows);
    QObject::connect(catalogue, &scrubtub::Catalogue::scanProgress,
                     catalogueModel, &scrubtub::CatalogueModel::applyProgress);
    QObject::connect(catalogue, &scrubtub::Catalogue::rowsPageReady,
                     catalogueModel, &scrubtub::CatalogueModel::applyPage);
    // Restore the previous session now that the UI bridges are connected.
    proxy.loadExistingState();

    scrubtub::HoverSession hoverSession(&app);

    // Detail paging: the model requests pages of 200 rows by ID (§8) through
    // the proxy; results return as queued rowsChanged deliveries.
    catalogueModel->setFetchCallback([&proxy](const QList<qint64>& ids, quint64 generation) {
        QVariantList variantIds;
        for (const qint64 id : ids)
            variantIds.append(id);
        proxy.fetchRowsPage(variantIds, generation);
    });

    QQmlApplicationEngine engine;
    qmlRegisterType<scrubtub::HoverFrameItem>("scrubtub.media", 1, 0, "HoverFrameItem");
    // Poster/atlas paths are derived deterministically from the profile cache
    // directory; no database access happens on the UI thread (§2).
    const QString thumbsDir =
        scrubtub::ProfilePaths::profileDataDir() + QStringLiteral("/thumbs");
    engine.addImageProvider(
        QStringLiteral("previews"),
        new scrubtub::PreviewProvider([thumbsDir](qint64 videoId, qint64 revision, bool poster) {
            const QString profile = poster ? QStringLiteral("poster-320-v1")
                                           : QLatin1String(scrubtub::kSparsePreviewProfile);
            if (videoId <= 0 || revision <= 0)
                return QString();
            return thumbsDir + QStringLiteral("/%1-%2-%3.jpg")
                .arg(videoId)
                .arg(revision)
                .arg(profile);
        }));
    engine.rootContext()->setContextProperty(QStringLiteral("hoverSession"), &hoverSession);
    engine.rootContext()->setContextProperty(QStringLiteral("catalogue"), &proxy);
    engine.rootContext()->setContextProperty(QStringLiteral("catalogueModel"),
                                             catalogueModel);
    engine.loadFromModule("scrubtub.ui", "Main");
    if (engine.rootObjects().isEmpty()) {
        qCritical("Failed to load the main QML module");
        catalogueThread->quit();
        catalogueThread->wait();
        return 1;
    }
    if (!autoRoot.isEmpty())
        QMetaObject::invokeMethod(catalogue, [catalogue, autoRoot] {
            catalogue->addRoot(autoRoot);
        }, Qt::QueuedConnection);

    const int exitCode = app.exec();

    catalogueThread->quit();
    catalogueThread->wait();
    g_logFile = nullptr;
    return exitCode;
}
