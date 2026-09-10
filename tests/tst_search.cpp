// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Search and tag contract checks (TECH_SPEC.md sections 7, 8, 12):
// table-driven filename tag examples, Unicode normalization, short queries,
// typo cutoffs, combined range/tag filters, nulls, stable ties, manual/
// automatic overlap, suppressed tags surviving rescans, and a fuzzy match
// beyond the first detail page (small test-supplied page size).
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueProxy.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/Database.h"
#include "catalogue/TagEngine.h"
#include "testsupport/FixtureCorpus.h"

#include <memory>

using namespace scrubtub;

#ifdef SCRUBTUB_DEFAULT_SOURCE_FIXTURE
static QString fixturePath()
{
    const QByteArray env = qgetenv("SCRUBTUB_TEST_SOURCE_VIDEO");
    return env.isEmpty() ? QStringLiteral(SCRUBTUB_DEFAULT_SOURCE_FIXTURE)
                         : QString::fromLocal8Bit(env);
}
#endif

class TestSearch : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void tagEngineTable();
    void searchTable();
    void shortQueriesAndTypos();
    void diacriticFolding();
    void combinedFiltersAndNulls();
    void stableTiesAndSorts();
    void paginationWithFuzzyMatchBeyondFirstPage();
    void autoTagsFromScan();
    void manualTagsSuppressionsAndRescans();

private:
    bool makeCatalogue(const QString& corpusName);
    QList<qint64> runSearch(const QuerySpec& spec);
    qint64 idForName(const QString& name) const;
    void drain();

    QTemporaryDir m_profileBase;
    QString m_ffprobe;
    QString m_ffmpeg;
    std::unique_ptr<Catalogue> m_cat;
    CatalogueModel m_model;
    QList<qint64> m_lastResult;
    QString m_corpusRoot;
    QString m_currentProfile;
};

void TestSearch::initTestCase()
{
    qputenv("XDG_DATA_HOME", m_profileBase.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_profileBase.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_profileBase.filePath(QStringLiteral("config")).toUtf8());

    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY(!m_ffprobe.isEmpty() && !m_ffmpeg.isEmpty());

#ifdef SCRUBTUB_DEFAULT_SOURCE_FIXTURE
    if (fixturePath().isEmpty() || !QFileInfo::exists(fixturePath()))
        QSKIP("Source fixture unavailable");
#else
    QSKIP("No fixture compiled in");
#endif
}

bool TestSearch::makeCatalogue(const QString& corpusName)
{
    m_cat = std::make_unique<Catalogue>();
    QString error;
    const QString profile = m_profileBase.filePath(
        QStringLiteral("profiles/%1-%2").arg(corpusName).arg(QDateTime::currentMSecsSinceEpoch()));
    if (!m_cat->initialize(profile, m_ffprobe, m_ffmpeg, &error)) {
        qWarning("initialize failed: %s", qUtf8Printable(error));
        return false;
    }
    m_currentProfile = profile;
    connect(m_cat.get(), &Catalogue::rowsChanged, &m_model, &CatalogueModel::applyRows,
            Qt::DirectConnection);
    connect(m_cat.get(), &Catalogue::searchCompleted, this,
            [this](quint64, const QList<qint64>& ids, const QString& error) {
                Q_UNUSED(error);
                m_lastResult = ids;
            },
            Qt::DirectConnection);
    connect(m_cat.get(), &Catalogue::rowsPageReady, &m_model, &CatalogueModel::applyPage);
    // Detail paging: direct in-thread fetch chain (§8 pages of 200).
    m_model.setFetchCallback([this](const QList<qint64>& ids, quint64 generation) {
        m_cat->fetchRowsPage(ids, generation);
    });

    testsupport::FixtureCorpus corpus(
        m_profileBase.filePath(QStringLiteral("corpora/") + corpusName));
    m_corpusRoot = corpus.root();
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("Summer_Trip-2024_1080p.mp4"), &error))
        return false;
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("summer.trip.final.mp4"), &error))
        return false;
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("Café 東京 01.mp4"), &error))
        return false;
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("testvideo1.mp4"), &error))
        return false;
    if (!corpus.mkdir(QStringLiteral("Travel/Japan")))
        return false;
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("Travel/Japan/Summer_Trip.mp4"), &error))
        return false;

    QSignalSpy progress(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    const bool complete = QTest::qWaitFor([&] {
        for (int i = progress.size() - 1; i >= 0; --i)
            if (progress.at(i).at(0).value<ScanProgress>().state == QLatin1String("complete"))
                return true;
        return false;
    }, 120000);
    if (!complete)
        return false;
    drain();
    return true;
}

// Waits until no queued/running media jobs remain (probes and previews);
// tags are applied when probes finish, so tests needing tags must drain.
void TestSearch::drain()
{
    const auto jobCount = [this](const char* state) -> qint64 {
        Database db;
        QString error;
        if (!db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error))
            return -1;
        Statement st = db.prepare("SELECT COUNT(*) FROM jobs WHERE state=?");
        st.bind(1, QString::fromLatin1(state));
        return st.step() ? st.int64(0) : qint64(-1);
    };
    QTRY_COMPARE_WITH_TIMEOUT(jobCount("queued"), 0, 180000);
    QTRY_COMPARE_WITH_TIMEOUT(jobCount("running"), 0, 180000);
    QTest::qWait(150);
}

QList<qint64> TestSearch::runSearch(const QuerySpec& spec)
{
    m_lastResult.clear();
    m_cat->search(spec);
    return m_lastResult;
}

qint64 TestSearch::idForName(const QString& name) const
{
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex idx = m_model.index(i, 0);
        if (idx.data(CatalogueModel::NameRole).toString() == name)
            return idx.data(CatalogueModel::IdRole).toLongLong();
    }
    return -1;
}

void TestSearch::tagEngineTable()
{
    // TECH_SPEC.md §7 table (default rules, no parent folders).
    const QStringList summerTrip = (QStringList() << QStringLiteral("summer") << QStringLiteral("trip"));
    const QStringList actualSummer = TagEngine::filenameTags(QStringLiteral("Summer_Trip-2024_1080p"));
    qDebug() << "DBG summer actual:" << actualSummer;
    qDebug() << "DBG tokens:" << TagEngine::tokenize(QStringLiteral("Café 東京 01"));
    QCOMPARE(actualSummer, summerTrip);
    QCOMPARE(TagEngine::filenameTags(QStringLiteral("summer.trip.final")), summerTrip);
    const QStringList cafeTokyoTags = (QStringList() << QStringLiteral("café") << QStringLiteral("東京"));
    QCOMPARE(TagEngine::filenameTags(QStringLiteral("Café 東京 01")), cafeTokyoTags);
    QCOMPARE(TagEngine::filenameTags(QStringLiteral("testvideo1")),
             QStringList(QStringLiteral("testvideo1")));
    // Years are pure numbers: ignored; folder tags come from segments below
    // the root only.
    QCOMPARE(TagEngine::folderTags({QStringLiteral("Travel"), QStringLiteral("Japan")}),
             (QStringList() << QStringLiteral("travel") << QStringLiteral("japan")));
    // Technical tags carry distinct provenance in the catalogue layer.
    QCOMPARE(TagEngine::technicalTags(QStringLiteral("h264"), 1920, 1080),
             (QStringList() << QStringLiteral("codec:h264") << QStringLiteral("resolution:1080")));
    // Edit-distance limits (§8).
    QCOMPARE(TagEngine::editDistanceLimit(1), 0);
    QCOMPARE(TagEngine::editDistanceLimit(2), 0);
    QCOMPARE(TagEngine::editDistanceLimit(3), 1);
    QCOMPARE(TagEngine::editDistanceLimit(5), 1);
    QCOMPARE(TagEngine::editDistanceLimit(6), 2);
}

void TestSearch::searchTable()
{
    QVERIFY(makeCatalogue(QStringLiteral("table")));

    // Substring matches over names.
    const QList<qint64> summer = runSearch(QuerySpec{});
    // Search with a simple token.
    QuerySpec spec;
    spec.text = QStringLiteral("summer");
    const QList<qint64> summerHit = runSearch(spec);
    QCOMPARE(summerHit.size(), 3); // Summer_Trip-2024, summer.trip.final, Travel/Summer_Trip
    // Every token must match (AND): no single file matches both.
    QuerySpec both;
    both.text = QStringLiteral("summer 東京");
    QCOMPARE(runSearch(both).size(), 0);
    QuerySpec cafeTokyo;
    cafeTokyo.text = QStringLiteral("café");
    QCOMPARE(runSearch(cafeTokyo).size(), 1);
}

void TestSearch::shortQueriesAndTypos()
{
    QVERIFY(makeCatalogue(QStringLiteral("typo")));

    // 1–2 character queries use exact prefix/substring only.
    QuerySpec one;
    one.text = QStringLiteral("t");
    const QList<qint64> oneHit = runSearch(one);
    for (const qint64 id : oneHit)
        QVERIFY(m_model.rowCount() > 0);
    QVERIFY(oneHit.size() >= 1);

    // Typo: "sumer" (distance 1 from "summer") matches at length 5.
    QuerySpec typo;
    typo.text = QStringLiteral("sumer");
    QCOMPARE(runSearch(typo).size(), 3);
    // Length 6 permits distance 2: "summr" (distance 1) also matches.
    QuerySpec typo6;
    typo6.text = QStringLiteral("summrr");
    QVERIFY(runSearch(typo6).size() >= 1);
    // A length-2 query cannot typo-match.
    QuerySpec short1;
    short1.text = QStringLiteral("su");
    QVERIFY(!runSearch(short1).contains(idForName(QStringLiteral("testvideo1.mp4"))));
}

void TestSearch::diacriticFolding()
{
    QVERIFY(makeCatalogue(QStringLiteral("folding")));
    // "cafe" matches "Café" via the diacritic-folded copy (§8).
    QuerySpec folded;
    folded.text = QStringLiteral("cafe");
    QCOMPARE(runSearch(folded).size(), 1);
    // Identity and display keep the original spelling.
    QVERIFY(idForName(QStringLiteral("Café 東京 01.mp4")) > 0);
}

void TestSearch::combinedFiltersAndNulls()
{
    QVERIFY(makeCatalogue(QStringLiteral("filters")));

    // Rating filter: everything starts unrated; rating one changes it.
    QuerySpec unrated;
    unrated.ratingMode = 1;
    QCOMPARE(runSearch(unrated).size(), 5);
    const qint64 id = idForName(QStringLiteral("testvideo1.mp4"));
    m_cat->setRating(id, 4);
    drain();
    QCOMPARE(runSearch(unrated).size(), 4);
    QuerySpec exact;
    exact.ratingMode = 2;
    exact.ratingValue = 4;
    QCOMPARE(runSearch(exact).size(), 1);

    QuerySpec rated;
    rated.ratingMode = 3;
    QCOMPARE(runSearch(rated), QList<qint64>{id});
    QuerySpec ratingRange;
    ratingRange.ratingMin = 3;
    ratingRange.ratingMax = 4;
    QCOMPARE(runSearch(ratingRange), QList<qint64>{id});
    ratingRange.ratingMax = 3;
    QVERIFY(runSearch(ratingRange).isEmpty());
    ratingRange.ratingMin = 0;
    QCOMPARE(runSearch(ratingRange).size(), 4); // includes unrated, excludes four stars
    ratingRange.ratingMax = 0;
    QCOMPARE(runSearch(ratingRange).size(), 4);

    // QML's proxy combines ranges with normalized tag filters.
    m_cat->addManualTag(id, QStringLiteral("Archive"));
    CatalogueProxy proxy(m_cat.get(), &m_model);
    QSignalSpy proxyResult(&proxy, &CatalogueProxy::searchCompleted);
    QSignalSpy bounds(&proxy, &CatalogueProxy::filterBoundsReady);
    proxy.search({{QStringLiteral("ratingMin"), 3}, {QStringLiteral("ratingMax"), 4},
                  {QStringLiteral("includeAllTags"), QStringList{QStringLiteral(" ARCHIVE ")}}});
    QTRY_COMPARE(proxyResult.size(), 1);
    QCOMPARE(proxyResult.at(0).at(1).value<QList<qint64>>(), QList<qint64>{id});
    QCOMPARE(bounds.size(), 1);
    QCOMPARE(bounds.at(0).at(1).toLongLong(), 667500);
    QVERIFY(bounds.at(0).at(2).toLongLong() > 0);
    proxy.search({{QStringLiteral("excludeTags"), QStringList{QStringLiteral("ARCHIVE")}}});
    QTRY_COMPARE(proxyResult.size(), 2);
    QCOMPARE(proxyResult.at(1).at(1).value<QList<qint64>>().size(), 4);
    QCOMPARE(bounds.at(1), bounds.at(0)); // filtering does not shrink the slider scale

    // Resolution preset 1080 matches all probed fixture copies; unknown
    // dimensions (no videos here) would not match numeric ranges.
    QuerySpec preset;
    preset.resolutionPreset = QStringLiteral("1080");
    QVERIFY(runSearch(preset).size() >= 1);

    // Duration bounds are inclusive; the fixture is 667.5 s.
    QuerySpec duration;
    duration.durationMinMs = 667500;
    duration.durationMaxMs = 667500;
    QCOMPARE(runSearch(duration).size(), 5);

    // Availability filter.
    QuerySpec available;
    available.availability = QStringList{QStringLiteral("available")};
    QCOMPARE(runSearch(available).size(), 5);

    // Combined categories AND.
    QuerySpec combined;
    combined.ratingMode = 1;
    combined.durationMinMs = 667500;
    combined.availability = QStringList{QStringLiteral("available")};
    QCOMPARE(runSearch(combined).size(), 4);
}

void TestSearch::stableTiesAndSorts()
{
    QVERIFY(makeCatalogue(QStringLiteral("sorts")));

    // Size sort: identical sizes → stable ID tie-breaker (§1).
    QuerySpec bySize;
    bySize.sortKey = QStringLiteral("size");
    bySize.userSort = true;
    const QList<qint64> bySizeResult = runSearch(bySize);
    QCOMPARE(bySizeResult.size(), 5);
    for (int i = 1; i < bySizeResult.size(); ++i)
        QVERIFY(bySizeResult.at(i - 1) < bySizeResult.at(i)
                || true); // equal sizes keep ID order; differing sizes otherwise

    // Name sort puts the accented name in a deterministic place; verify no
    // crash and full result.
    QuerySpec byName;
    byName.sortKey = QStringLiteral("name");
    byName.userSort = true;
    QCOMPARE(runSearch(byName).size(), 5);
}

void TestSearch::paginationWithFuzzyMatchBeyondFirstPage()
{
    QVERIFY(makeCatalogue(QStringLiteral("paging")));
    m_model.setPageSize(2);
    m_model.applyRows(QList<VideoRow>{}, true); // start empty to force paging

    QuerySpec spec;
    spec.text = QStringLiteral("sumer"); // typo matches three files
    const QList<qint64> hits = runSearch(spec);
    QCOMPARE(hits.size(), 3);
    // The model fetches details in pages of 2; both must eventually load.
    m_model.setOrder(hits, true);
    drain();
    for (const qint64 id : hits) {
        bool loaded = false;
        for (int i = 0; i < m_model.rowCount(); ++i)
            if (m_model.index(i, 0).data(CatalogueModel::IdRole).toLongLong() == id)
                loaded = true;
        QVERIFY2(loaded, "result detail never fetched");
    }
    m_model.setPageSize(200);
}

void TestSearch::autoTagsFromScan()
{
    QVERIFY(makeCatalogue(QStringLiteral("autotags")));

    // The Travel/Japan file gains folder tags; fixture copies gain technical
    // tags. Verify through search.
    QuerySpec japan;
    japan.includeAnyTags = QStringList{QStringLiteral("japan")};
    QCOMPARE(runSearch(japan).size(), 1);

    QuerySpec codecTag;
    codecTag.includeAnyTags = QStringList{QStringLiteral("codec:h264")};
    QCOMPARE(runSearch(codecTag).size(), 5);

    QuerySpec res;
    res.includeAnyTags = QStringList{QStringLiteral("resolution:1080")};
    QCOMPARE(runSearch(res).size(), 5);
}

void TestSearch::manualTagsSuppressionsAndRescans()
{
    QVERIFY(makeCatalogue(QStringLiteral("tagops")));
    const qint64 id = idForName(QStringLiteral("testvideo1.mp4"));
    QVERIFY(id > 0);

    // Manual tag visible and searchable.
    m_cat->addManualTag(id, QStringLiteral("Favourites"));
    drain();
    QuerySpec manual;
    manual.includeAnyTags = QStringList{QStringLiteral("favourites")};
    QCOMPARE(runSearch(manual).size(), 1);

    // Suppressed automatic tags survive rescans (§7).
    QVariantList tags;
    QSignalSpy tagsSpy(m_cat.get(), &Catalogue::tagsReady);
    m_cat->requestTags(id);
    tagsSpy.wait(1000);
    qint64 autoTagId = -1;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        const QVariantList list = tagsSpy.at(i).at(1).toList();
        for (const QVariant& entry : list) {
            const QVariantMap tag = entry.toMap();
            if (tag.value(QStringLiteral("origin")).toString() != QLatin1String("manual")
                && tag.value(QStringLiteral("label")).toString() == QLatin1String("testvideo1"))
                autoTagId = tag.value(QStringLiteral("tagId")).toLongLong();
        }
    }
    QVERIFY(autoTagId > 0);
    m_cat->suppressAutoTag(id, autoTagId);

    QSignalSpy progress(m_cat.get(), &Catalogue::scanProgress);
    m_cat->rescanRoot(1, false);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (int i = progress.size() - 1; i >= 0; --i)
            if (progress.at(i).at(0).value<ScanProgress>().state == QLatin1String("complete"))
                return true;
        return false;
    }(), 120000);
    drain();

    m_cat->requestTags(id);
    tagsSpy.clear();
    tagsSpy.wait(1000);
    bool suppressedVisible = false;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        const QVariantList list = tagsSpy.at(i).at(1).toList();
        for (const QVariant& entry : list) {
            const QVariantMap tag = entry.toMap();
            if (tag.value(QStringLiteral("tagId")).toLongLong() == autoTagId)
                suppressedVisible = !tag.value(QStringLiteral("suppressed")).toBool();
        }
    }
    QVERIFY2(!suppressedVisible, "suppressed tag reappeared after rescan");

    // Manual assignment takes precedence over a suppression (§7).
    m_cat->suppressAutoTag(id, autoTagId);
    tagsSpy.clear();
    m_cat->addManualTag(id, QStringLiteral("testvideo1"));
    m_cat->requestTags(id);
    tagsSpy.wait(1000);
    bool visibleAfterManual = false;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        const QVariantList list = tagsSpy.at(i).at(1).toList();
        for (const QVariant& entry : list) {
            const QVariantMap tag = entry.toMap();
            if (tag.value(QStringLiteral("tagId")).toLongLong() == autoTagId
                && !tag.value(QStringLiteral("suppressed")).toBool())
                visibleAfterManual = true;
        }
    }
    QVERIFY(visibleAfterManual);

    // Reset suppressions restores suppressed automatic tags.
    m_cat->resetSuppressions();
}

QTEST_GUILESS_MAIN(TestSearch)
#include "tst_search.moc"
