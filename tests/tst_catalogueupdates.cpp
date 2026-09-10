// SPDX-License-Identifier: GPL-3.0-only
// Regression checks use in-memory rows and disposable catalogue metadata only.
// No fixture media is copied, generated, or changed.
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/CatalogueProxy.h"
#include "catalogue/Database.h"
#include "media/Extract.h"

using namespace scrubtub;

static VideoRow row(qint64 id)
{
    VideoRow result;
    result.id = id;
    result.fileName = QString::number(id);
    return result;
}

class TestCatalogueUpdates : public QObject {
    Q_OBJECT
private slots:
    void proxyDeliversMaintenanceResults();
    void queuedPagesYieldAndPreserveOrder();
    void synchronousPagesAndMissingVideos();
    void stalePagesAfterResetAreDiscarded();
    void removingRootsPreservesRemainingRowsAndProgress();
    void extensionlessBackupRestores();
    void cachedStoryboardsAreReused();
    void startupRetiresLegacyPreviews();
    void sparsePlanHasMidpointAndBounds();
    void secondPassWaitsForPrimaryWork();
    void sparseCacheBudgetStopsGeneration();
};

void TestCatalogueUpdates::proxyDeliversMaintenanceResults()
{
    QTemporaryDir temp;
    Catalogue catalogue;
    CatalogueModel model;
    CatalogueProxy proxy(&catalogue, &model);
    QString error;
    QVERIFY2(catalogue.initialize(temp.filePath("profile"), {}, {}, &error), qPrintable(error));
    QSignalSpy usage(&proxy, &CatalogueProxy::cacheUsageReady);
    proxy.requestCacheUsage();
    QTRY_COMPARE(usage.count(), 1);
    QCOMPARE(usage.first().first().toLongLong(), 0);
    QSignalSpy trash(&proxy, &CatalogueProxy::trashResult);
    proxy.trashVideos({999}); // Unknown ID: reports failure without touching files.
    QTRY_COMPARE(trash.count(), 1);
    QVERIFY(!trash.first().at(1).toBool());
    QSignalSpy info(&proxy, &CatalogueProxy::fileInfoReady);
    proxy.requestFileInfo(999);
    QTRY_COMPARE(info.count(), 1);
    QVERIFY(info.first().at(1).toMap().isEmpty());
    QSignalSpy settings(&proxy, &CatalogueProxy::settingsReady);
    catalogue.restoreSettings();
    QTRY_COMPARE(settings.count(), 1);
}

void TestCatalogueUpdates::queuedPagesYieldAndPreserveOrder()
{
    CatalogueModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.setPageSize(2);
    int requests = 0;
    model.setFetchCallback([&](const QList<qint64>& ids, quint64 generation) {
        ++requests;
        QTimer::singleShot(0, &model, [&, ids, generation] {
            QList<VideoRow> rows;
            for (auto id : ids) rows.append(row(id));
            model.applyPage(rows, ids, generation);
        });
    });
    const QList<qint64> order{5, 3, 1, 4, 2};
    model.setOrder(order, true);
    QCOMPARE(requests, 1); // Must return before the queued response arrives.
    QTRY_COMPARE(model.index(4).data(CatalogueModel::IdRole).toLongLong(), 2);
    QCOMPARE(requests, 3);
    QCOMPARE(model.count(), order.size()); // Paging must not duplicate IDs.
    for (int i = 0; i < order.size(); ++i)
        QCOMPARE(model.index(i).data(CatalogueModel::IdRole).toLongLong(), order[i]);
}

void TestCatalogueUpdates::synchronousPagesAndMissingVideos()
{
    CatalogueModel model;
    model.setPageSize(2);
    int requests = 0;
    model.setFetchCallback([&](const QList<qint64>& ids, quint64 generation) {
        ++requests;
        QList<VideoRow> rows;
        for (auto id : ids)
            if (id != 1 && id != 2) rows.append(row(id));
        model.applyPage(rows, ids, generation);
    });
    model.setOrder({1, 2, 3, 4, 5}, true);
    QCOMPARE(requests, 3);
    QCOMPARE(model.count(), 3);
    QCOMPARE(model.index(0).data(CatalogueModel::IdRole).toLongLong(), 3);
    QCOMPARE(model.index(2).data(CatalogueModel::IdRole).toLongLong(), 5);
}

void TestCatalogueUpdates::stalePagesAfterResetAreDiscarded()
{
    CatalogueModel model;
    quint64 oldGeneration = 0;
    int requests = 0;
    model.setFetchCallback([&](const QList<qint64>&, quint64 generation) {
        if (++requests == 1) oldGeneration = generation;
    });
    model.setOrder({1, 2}, true);
    model.applyRows({row(3)}, true); // Folder removal reloads surviving rows.
    model.applyPage({row(1), row(2)}, {1, 2}, oldGeneration);
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.index(0).data(CatalogueModel::IdRole).toLongLong(), 3);
    model.setOrder({4, 3}, true);
    const auto generation = model.generation();
    model.applyPage({}, {4}, generation); // Deleted after the search completed.
    QCOMPARE(model.count(), 1);
    QCOMPARE(requests, 2);
}

void TestCatalogueUpdates::removingRootsPreservesRemainingRowsAndProgress()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    Catalogue catalogue;
    CatalogueModel model;
    QString error;
    QVERIFY2(catalogue.initialize(temp.filePath("profile"), {}, {}, &error), qPrintable(error));
    connect(&catalogue, &Catalogue::rowsChanged, &model, &CatalogueModel::applyRows);
    connect(&catalogue, &Catalogue::scanProgress, &model, &CatalogueModel::applyProgress);
    QSignalSpy progress(&catalogue, &Catalogue::scanProgress);
    QSignalSpy errors(&catalogue, &Catalogue::operationFailed);
    // Empty folders exercise root lifecycle; rows below are metadata, not files.
    for (int i = 1; i <= 3; ++i) {
        const QString path = temp.filePath(QString::number(i));
        QVERIFY(QDir().mkpath(path));
        catalogue.addRoot(path);
    }
    catalogue.pauseScanning(); // No media subprocess needs to run for this test.
    Database db;
    QVERIFY(db.open(temp.filePath("profile/catalogue.db"), &error));
    QVERIFY(db.exec(
        "INSERT INTO videos(id,root_id,rel_path,cmp_key,file_name,added_ms,probe_status) VALUES"
        "(1,1,'a','a','a',0,'ok'),(2,1,'b','b','b',0,'pending'),"
        "(3,2,'c','c','c',0,'ok'),(4,3,'d','d','d',0,'pending'),"
        "(5,2,'e','e','e',0,'error');"
        "INSERT INTO cache_entries(video_id,revision,profile,kind,rel_path,bytes,last_access_ms) "
        "VALUES(1,1,'poster','poster','a.jpg',0,0),(3,1,'poster','poster','c.jpg',0,0);"
        "INSERT INTO jobs(video_id,revision,kind,state) VALUES"
        "(2,1,'probe','queued'),(4,1,'probe','queued'),(5,1,'probe','error'),"
        "(1,1,'storyboard','queued');"));
    catalogue.loadExistingState();
    QCOMPARE(model.count(), 5);
    QCOMPARE(model.remaining(), 2);
    QCOMPARE(model.processed(), 2);
    QCOMPARE(model.failed(), 1);
    catalogue.requestPoster(1);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE kind='poster'").value(), 0);
    // A probe-to-poster handoff must not count one video twice.
    QVERIFY(db.exec("INSERT INTO jobs(video_id,revision,kind,state) VALUES(2,1,'poster','queued')"));
    catalogue.loadExistingState();
    QCOMPARE(model.remaining(), 2);
    progress.clear();
    catalogue.removeRoot(3); // Most recently scanned root: used to cancel all jobs.
    QCOMPARE(model.count(), 4);
    QCOMPARE(model.remaining(), 1);
    QCOMPARE(model.processed(), 2);
    QCOMPARE(model.scanState(), QStringLiteral("paused"));
    QCOMPARE(db.scalarInt("SELECT SUM(scan_generation) FROM roots").value(), 2);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE state='queued'").value(), 3);
    for (const auto& args : progress)
        QVERIFY(qvariant_cast<ScanProgress>(args[0]).state != QStringLiteral("enumerating"));
    catalogue.removeRoot(1);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.remaining(), 0);
    QCOMPARE(model.processed(), 1);
    catalogue.removeRoot(2);
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.processed(), 0);
    QCOMPARE(model.failed(), 0);
    QCOMPARE(errors.count(), 0);
}

void TestCatalogueUpdates::extensionlessBackupRestores()
{
    QTemporaryDir temp;
    Catalogue source;
    Catalogue restored;
    QString error;
    QVERIFY(source.initialize(temp.filePath("source"), {}, {}, &error));
    QVERIFY(restored.initialize(temp.filePath("restored"), {}, {}, &error));
    Database db;
    QVERIFY(db.open(temp.filePath("source/catalogue.db"), &error));
    QVERIFY(db.exec("INSERT INTO settings(key,value) VALUES('backup_test','retained')"));
    const QString path = temp.filePath("legacy backup");
    QSignalSpy exported(&source, &Catalogue::backupExported);
    source.exportBackup(QUrl::fromLocalFile(path).toString());
    QCOMPARE(exported.count(), 1);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.read(16), QByteArray("SQLite format 3\0", 16));
    file.close();
    QSignalSpy imported(&restored, &Catalogue::backupImported);
    restored.importBackup(QUrl::fromLocalFile(path).toString());
    QCOMPARE(imported.count(), 1);
    Database restoredDb;
    QVERIFY(restoredDb.open(temp.filePath("restored/catalogue.db"), &error));
    QCOMPARE(restoredDb.scalarText("SELECT value FROM settings WHERE key='backup_test'").value(),
             QStringLiteral("retained"));
}

void TestCatalogueUpdates::cachedStoryboardsAreReused()
{
    QTemporaryDir temp;
    Catalogue catalogue;
    QString error;
    QVERIFY(catalogue.initialize(temp.filePath("profile"), {}, {}, &error));
    Database db;
    QVERIFY(db.open(temp.filePath("profile/catalogue.db"), &error));
    QVERIFY(db.exec(
        "INSERT INTO roots(id,path,cmp_key) VALUES(1,'metadata-only','metadata-only');"
        "INSERT INTO videos(id,root_id,rel_path,cmp_key,file_name,added_ms,probe_status,duration_ms) "
        "VALUES(1,1,'video','video','video',0,'ok',30000);"
        "INSERT INTO cache_entries(video_id,revision,profile,kind,rel_path,bytes,last_access_ms) "
        "VALUES(1,1,'poster-320-v1','poster','poster.jpg',0,0),"
        "(1,1,'sb-240-5-v2','storyboard','storyboard.jpg',0,0);"));
    QSignalSpy progress(&catalogue, &Catalogue::scanProgress);
    catalogue.requestStoryboard(1);
    catalogue.requestStoryboard(1);
    catalogue.requestPoster(1);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs").value(), 0);
    QTest::qWait(20);
    QCOMPARE(progress.count(), 0); // A cache hit does not wake the dispatcher.

    catalogue.pauseScanning(); // Exercise queue decisions without reading media.
    QVERIFY(db.exec("DELETE FROM cache_entries WHERE kind='storyboard'"));
    catalogue.requestStoryboard(1);
    catalogue.requestStoryboard(1);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE kind='storyboard'").value(), 1);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE kind='poster'").value(), 0);

    QVERIFY(db.exec(
        "DELETE FROM jobs;"
        "INSERT INTO cache_entries(video_id,revision,profile,kind,rel_path,bytes,last_access_ms) "
        "VALUES(1,1,'sb-240-5-v2','storyboard','storyboard.jpg',0,0);"
        "UPDATE videos SET revision=2 WHERE id=1"));
    catalogue.requestStoryboard(1);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE revision=2").value(), 2);
    // Old-revision previews cannot suppress generation for a changed video.
}

void TestCatalogueUpdates::startupRetiresLegacyPreviews()
{
    QTemporaryDir temp;
    QString error;
    const QString profile = temp.filePath("profile");
    {
        Catalogue original;
        QVERIFY(original.initialize(profile, {}, {}, &error));
        Database db;
        QVERIFY(db.open(profile + "/catalogue.db", &error));
        QVERIFY(db.exec(
            "INSERT INTO roots(id,path,cmp_key) VALUES(1,'metadata-only','metadata-only');"
            "INSERT INTO videos(id,root_id,rel_path,cmp_key,file_name,added_ms) "
            "VALUES(1,1,'video','video','video',0);"
            "INSERT INTO jobs(video_id,revision,kind,state) VALUES"
            "(1,1,'probe','queued'),(1,1,'poster','queued'),(1,1,'storyboard','running');"
            "INSERT INTO cache_entries(video_id,revision,profile,kind,rel_path,bytes,last_access_ms) "
            "VALUES(1,1,'sb-320-v1','storyboard','1-1-sb-320-v1.jpg',3,0);"));
        for (const QString& name : {QStringLiteral("1-1-sb-320-v1.jpg"), QStringLiteral("notes.txt")}) {
            QFile file(profile + "/thumbs/" + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("old");
        }
    }
    Catalogue restarted;
    QVERIFY(restarted.initialize(profile, {}, {}, &error));
    Database db;
    QVERIFY(db.open(profile + "/catalogue.db", &error));
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE kind='storyboard' AND state='queued'").value(), 1);
    QVERIFY(!QFile::exists(profile + "/thumbs/1-1-sb-320-v1.jpg"));
    QVERIFY(QFile::exists(profile + "/thumbs/notes.txt"));
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM cache_entries WHERE profile='sb-320-v1'").value(), 0);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE kind IN ('probe','poster')").value(), 2);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM videos").value(), 1);
}

void TestCatalogueUpdates::sparsePlanHasMidpointAndBounds()
{
    QCOMPARE(Extract::samplePlanMs(100000, 5), QVector<qint64>({10000,30000,50000,70000,90000}));
    QVERIFY(Extract::samplePlanMs(100000, 0).isEmpty());
    const auto shortPlan = Extract::samplePlanMs(500, 5);
    QVERIFY(!shortPlan.isEmpty());
    QVERIFY(shortPlan.first() >= 0 && shortPlan.last() < 500);
}

void TestCatalogueUpdates::secondPassWaitsForPrimaryWork()
{
    QTemporaryDir temp;
    Catalogue catalogue;
    QString error;
    QVERIFY(catalogue.initialize(temp.filePath("profile"), {}, {}, &error));
    Database db;
    QVERIFY(db.open(temp.filePath("profile/catalogue.db"), &error));
    QVERIFY(db.exec(
        "INSERT INTO roots(id,path,cmp_key) VALUES(1,'metadata-only','metadata-only');"
        "INSERT INTO videos(id,root_id,rel_path,cmp_key,file_name,added_ms,probe_status,availability,duration_ms) "
        "VALUES(1,1,'video','video','video',0,'ok','available',30000);"
        "INSERT INTO cache_entries(video_id,revision,profile,kind,rel_path,bytes,last_access_ms) "
        "VALUES(1,1,'poster-320-v1','poster','poster.jpg',0,0);"
        "INSERT INTO jobs(video_id,revision,kind,state) VALUES(1,1,'probe','running');"));
    catalogue.loadExistingState();
    QTest::qWait(20);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE kind='storyboard'").value(), 0);
    // An explicit preview request must also wait behind the running probe.
    catalogue.requestStoryboard(1);
    QTest::qWait(20);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs WHERE kind='storyboard' AND state='queued'").value(), 1);
}

void TestCatalogueUpdates::sparseCacheBudgetStopsGeneration()
{
    QTemporaryDir temp;
    Catalogue catalogue;
    QString error;
    QVERIFY(catalogue.initialize(temp.filePath("profile"), {}, {}, &error));
    Database db;
    QVERIFY(db.open(temp.filePath("profile/catalogue.db"), &error));
    QVERIFY(db.exec(
        "INSERT INTO roots(id,path,cmp_key) VALUES(1,'metadata-only','metadata-only');"
        "INSERT INTO videos(id,root_id,rel_path,cmp_key,file_name,added_ms,probe_status,availability,duration_ms) "
        "VALUES(1,1,'video','video','video',0,'ok','available',30000),"
        "(2,1,'other','other','other',0,'ok','available',30000);"
        "INSERT INTO cache_entries(video_id,revision,profile,kind,rel_path,bytes,last_access_ms) "
        "VALUES(1,1,'poster-320-v1','poster','poster.jpg',0,0),"
        "(2,1,'poster-320-v1','poster','other.jpg',0,0),"
        "(2,1,'sb-240-5-v2','storyboard','atlas.jpg',134217728,0);"));
    QSignalSpy progress(&catalogue, &Catalogue::scanProgress);
    catalogue.loadExistingState();
    QTest::qWait(20);
    QCOMPARE(db.scalarInt("SELECT COUNT(*) FROM jobs").value(), 0);
    QCOMPARE(db.scalarInt("SELECT SUM(bytes) FROM cache_entries WHERE kind='storyboard'").value(),
             kSparsePreviewCacheBytes);
    QCOMPARE(qvariant_cast<ScanProgress>(progress.last()[0]).state, QStringLiteral("complete"));
}

QTEST_GUILESS_MAIN(TestCatalogueUpdates)
#include "tst_catalogueupdates.moc"
