// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Performance budgets for the paths a large library exercises on every
// keystroke and every finished media job. These are regression fences, not
// benchmarks: the budgets are several times the measured cost on a development
// machine, so they stay green on slow CI while still failing if a quadratic
// scan or a per-row query creeps back in.
//
// Media-free by design: rows are inserted straight into the profile database,
// so this suite runs everywhere and touches no corpus.
#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/Database.h"

#include <cstdio>

using namespace scrubtub;

namespace {

// Sizes are chosen so the pre-fix quadratic implementations blow past the
// budgets here, while the current linear ones sit ~10x under them.
constexpr int kSearchRows = 20000;
constexpr int kModelRows = 50000;

// A row set with the shape of a real library: names, folders, unicode, sizes.
QList<VideoRow> syntheticRows(int count)
{
    static const char* const kNames[] = {
        "Summer_Trip-2024_1080p.mp4", "Café 東京 01.mp4", "a04XGv_460sv.mp4",
        "2022-12-07 17-06-28.mkv", "testvideo1.mp4", "ADifferent_Name.webm",
        "some_long_series_name_episode_012.mp4",
    };
    QList<VideoRow> rows;
    rows.reserve(count);
    for (int i = 1; i <= count; ++i) {
        VideoRow row;
        row.id = i;
        row.rootId = 1;
        row.fileName = QStringLiteral("%1-%2").arg(i / 7).arg(QLatin1String(kNames[i % 7]));
        row.relPath = QStringLiteral("Holidays/Japan %1/%2").arg(i % 12).arg(row.fileName);
        row.sizeBytes = 1'000'000 + i;
        row.mtimeMs = i * 1000;
        row.durationMs = 60'000 + i;
        row.displayWidth = 1920;
        row.displayHeight = 1080;
        row.codec = QStringLiteral("h264");
        row.addedMs = i * 1000;
        row.availability = QStringLiteral("available");
        row.probeStatus = QStringLiteral("ok");
        row.posterReady = true;
        rows.append(row);
    }
    return rows;
}

qint64 elapsedFor(const std::function<void()>& work)
{
    QElapsedTimer timer;
    timer.start();
    work();
    return timer.elapsed();
}

} // namespace

class TestPerformance : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void searchStaysFastOnLargeLibraries();
    void modelStaysLinearOnLargeLibraries();

private:
    void fillDatabase(const QString& profile, int rows);

    QTemporaryDir m_base;
    QString m_ffprobe;
    QString m_ffmpeg;
};

void TestPerformance::initTestCase()
{
    QVERIFY(m_base.isValid());
    qputenv("XDG_DATA_HOME", m_base.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_base.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_base.filePath(QStringLiteral("config")).toUtf8());
    // No media subprocess runs in this suite; paths stay empty on purpose.
}

// 20k rows with the same shape the scanner inserts, written directly.
void TestPerformance::fillDatabase(const QString& profile, int rows)
{
    Database db;
    QString error;
    QVERIFY(db.open(profile + QStringLiteral("/catalogue.db"), &error));
    QVERIFY(db.exec("INSERT INTO roots(id,path,cmp_key) VALUES(1,'/media','/media')"));
    QVERIFY(db.exec("BEGIN IMMEDIATE"));
    Statement ins = db.prepare(
        "INSERT INTO videos(id,root_id,rel_path,cmp_key,file_name,size_bytes,mtime_ms,"
        "revision,duration_ms,display_width,display_height,codec,views,added_ms,"
        "availability,probe_status,seen_generation) "
        "VALUES(?,1,?,'k'||?,?,?,?,1,?,1920,1080,'h264',0,?, 'available','ok',1)");
    for (int i = 1; i <= rows; ++i) {
        const QString name = QStringLiteral("%1-clip-%2.mp4").arg(i / 7).arg(i);
        ins.reset();
        ins.bind(1, qint64(i));
        ins.bind(2, QStringLiteral("Holidays/Japan %1/%2").arg(i % 12).arg(name));
        ins.bind(3, qint64(i));
        ins.bind(4, name);
        ins.bind(5, qint64(1'000'000 + i));
        ins.bind(6, qint64(i) * 1000);
        ins.bind(7, qint64(60'000 + i));
        ins.bind(8, qint64(i) * 1000);
        QVERIFY(ins.run());
    }
    QVERIFY(db.exec("COMMIT"));
}

// The UI thread does this work while scanning and while delivering search
// results. The pre-fix implementation scanned the whole display order once per
// row and once per page, so page delivery cost grew with the square of the
// library size; the budgets below are roughly 10x the measured cost, which
// still catches that at 20k rows without being sensitive to CI load.
void TestPerformance::modelStaysLinearOnLargeLibraries()
{
    const QList<VideoRow> rows = syntheticRows(kModelRows);
    CatalogueModel model;
    model.applyRows(rows, true); // warm-up outside the timing
    model.applyRows(QList<VideoRow>{}, true);

    const qint64 loadMs = elapsedFor([&] { model.applyRows(rows, true); });
    QCOMPARE(model.rowCount(), kModelRows);

    // One row update per finished probe/poster job.
    QList<VideoRow> single{rows.first()};
    const qint64 updateMs = elapsedFor([&] {
        for (int i = 0; i < kModelRows; ++i) {
            single[0] = rows.at(i);
            single[0].durationMs += 1;
            model.applyRows(single, false);
        }
    });

    // Search result delivery: order these IDs, then fetch details in pages.
    QList<qint64> order;
    order.reserve(kModelRows);
    for (int i = kModelRows; i >= 1; --i)
        order.append(i);
    const qint64 orderMs = elapsedFor([&] { model.setOrder(order, true); });

    CatalogueModel pager;
    pager.setFetchCallback([&](const QList<qint64>& ids, quint64 generation) {
        QList<VideoRow> page;
        page.reserve(ids.size());
        for (const qint64 id : ids)
            page.append(rows.at(int(id) - 1));
        pager.applyPage(page, ids, generation);
    });
    const qint64 pageMs = elapsedFor([&] { pager.setOrder(order, true); });
    QCOMPARE(pager.rowCount(), kModelRows);

    qInfo("%d rows: load %lld ms | %d row updates %lld ms | setOrder %lld ms | "
          "paged detail fetch %lld ms",
          kModelRows, loadMs, kModelRows, updateMs, orderMs, pageMs);

    QVERIFY2(pageMs < 500,
             qPrintable(QStringLiteral("paged detail fetch took %1 ms").arg(pageMs)));
    QVERIFY2(updateMs < 500,
             qPrintable(QStringLiteral("row updates took %1 ms").arg(updateMs)));
    QVERIFY2(loadMs < 2000,
             qPrintable(QStringLiteral("initial load took %1 ms").arg(loadMs)));
}

// A text query walks every candidate record; folding, typo distance and the
// relevance sort must not renormalize per comparison.
void TestPerformance::searchStaysFastOnLargeLibraries()
{
    const QString profile = m_base.filePath(QStringLiteral("profile-search"));
    Catalogue catalogue;
    QString error;
    QVERIFY2(catalogue.initialize(profile, m_ffprobe, m_ffmpeg, &error), qUtf8Printable(error));
    fillDatabase(profile, kSearchRows);

    QList<qint64> hits;
    connect(&catalogue, &Catalogue::searchCompleted, this,
            [&hits](quint64, const QList<qint64>& ids, const QString&) { hits = ids; },
            Qt::DirectConnection);

    // Bulk snapshot fill plus the first query.
    QuerySpec first;
    first.text = QStringLiteral("clip");
    const qint64 firstMs = elapsedFor([&] { catalogue.search(first); });
    QVERIFY(hits.size() > kSearchRows / 2);

    const QList<QString> queries{
        QStringLiteral("clip 1234"),   // multiple tokens
        QStringLiteral("clpi"),        // typo within the edit budget
        QStringLiteral("japan"),       // folder-only matches
        QStringLiteral("zzzznomatch"), // full rejection
        QStringLiteral("h264"),        // technical tag
    };
    qint64 worstMs = 0;
    QString worstQuery;
    for (const QString& text : queries) {
        QuerySpec spec;
        spec.text = text;
        const qint64 ms = elapsedFor([&] { catalogue.search(spec); });
        if (ms > worstMs) {
            worstMs = ms;
            worstQuery = text;
        }
    }
    qInfo("search @%d rows: first query (incl. record fill) %lld ms, worst repeat query "
          "('%s') %lld ms", kSearchRows, firstMs, qUtf8Printable(worstQuery), worstMs);

    // The 50 ms debounce is the user-visible budget; leave room for the UI work
    // that surrounds the query.
    QVERIFY2(worstMs < 800,
             qPrintable(QStringLiteral("query '%1' took %2 ms").arg(worstQuery).arg(worstMs)));
    QVERIFY2(firstMs < 3000,
             qPrintable(QStringLiteral("first query including fill took %1 ms").arg(firstMs)));
}

QTEST_GUILESS_MAIN(TestPerformance)
#include "tst_performance.moc"
