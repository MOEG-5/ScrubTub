// Reproduces the user's report: add their moved folder, scan, probe.
#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
using namespace itub;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Catalogue cat;
    CatalogueModel model;
    QString err;
    const QString profile = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + "/itub-repro-profile-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    if (!cat.initialize(profile, "/usr/bin/ffprobe", "/usr/bin/ffmpeg", &err)) {
        printf("init failed: %s\n", qUtf8Printable(err)); return 1;
    }
    QObject::connect(&cat, &Catalogue::rowsChanged, &model, &CatalogueModel::applyRows);
    QObject::connect(&cat, &Catalogue::operationFailed, [](const QString& m) {
        printf("operationFailed: %s\n", qUtf8Printable(m));
    });
    QObject::connect(&cat, &Catalogue::rootRejected, [](const QString& m) {
        printf("rootRejected: %s\n", qUtf8Printable(m));
    });
    cat.addRoot("/home/moeg/projects/itub/[SubsPlease] Kanojo, Okarishimasu (01-12) (1080p) [Batch]");
    // Wait up to 90 s for probing to settle, printing state.
    QTimer t;
    int elapsed = 0;
    QObject::connect(&t, &QTimer::timeout, [&] {
        elapsed += 1000;
        int ok=0, errCount=0, pending=0;
        for (int i = 0; i < model.rowCount(); ++i) {
            const QModelIndex idx = model.index(i, 0);
            const QString ps = idx.data(CatalogueModel::ProbeStatusRole).toString();
            if (ps == "ok") ++ok; else if (ps == "pending") ++pending; else ++errCount;
        }
        printf("t=%3ds rows=%d ok=%d pending=%d err/timeout=%d\n",
               elapsed/1000, model.rowCount(), ok, pending, errCount);
        if (elapsed >= 90000 || (model.rowCount() > 0 && pending == 0)) {
            // Print a couple of rows in detail.
            for (int i = 0; i < qMin(3, model.rowCount()); ++i) {
                const QModelIndex idx = model.index(i, 0);
                printf("  %s | avail=%s probe=%s dur=%lld\n",
                       qUtf8Printable(idx.data(CatalogueModel::NameRole).toString()),
                       qUtf8Printable(idx.data(CatalogueModel::AvailabilityRole).toString()),
                       qUtf8Printable(idx.data(CatalogueModel::ProbeStatusRole).toString()),
                       idx.data(CatalogueModel::DurationRole).toLongLong());
            }
            app.quit();
        }
    });
    t.start(1000);
    return app.exec();
}
