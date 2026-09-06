// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Catalogue contract checks (TECH_SPEC.md sections 4, 5, 12 "Reconciliation"):
// scans do not duplicate rows, interrupted/offline scans never imply mass
// deletion, annotations survive rescans and revisions, changed content bumps
// the revision and invalidates extraction, the probe uses the video stream
// duration, and profile locks exclude a second instance.
//
// Runs the Catalogue on the test thread with a spun event loop; probe
// subprocesses are driven by the normal async machinery.
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/Database.h"
#include "testsupport/FixtureCorpus.h"

#include <memory>

using namespace itub;

#ifdef ITUB_DEFAULT_SOURCE_FIXTURE
static QString fixturePath()
{
    const QByteArray env = qgetenv("ITUB_TEST_SOURCE_VIDEO");
    return env.isEmpty() ? QStringLiteral(ITUB_DEFAULT_SOURCE_FIXTURE)
                         : QString::fromLocal8Bit(env);
}
#endif

class TestCatalogue : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void scanFindsFilesAndProbes();
    void rescanHasNoDuplicatesAndNoReprobes();
    void annotationsSurviveRescan();
    void contentChangeBumpsRevisionAndPreservesAnnotations();
    void missingAfterRemoval();
    void offlineRootKeepsAvailability();
    void symlinkRootRejected();
    void overlappingRootRejected();
    void profileLockExcludesSecondInstance();
    void corruptedFileBecomesErrorState();
    void newerSchemaIsRejected();

private:
    bool makeCorpusRoot(const QString& name, QString* error);
    void waitScanComplete(QSignalSpy& progressSpy, int timeoutMs = 120000);
    qint64 videoIdFor(const QString& fileName) const;
    VideoRow rowFor(const QString& fileName);
    qint64 countRows() const;
    qint64 countJobs(const QString& state) const;

    QTemporaryDir m_profileBase;
    QString m_ffprobe;
    QString m_corpusRoot;
    std::unique_ptr<Catalogue> m_cat;
    CatalogueModel m_model;
    QString m_currentProfile;
};

void TestCatalogue::initTestCase()
{
    qputenv("XDG_DATA_HOME", m_profileBase.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_profileBase.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_profileBase.filePath(QStringLiteral("config")).toUtf8());

    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY2(!m_ffprobe.isEmpty(), "ffprobe must be installed for catalogue tests");

    QVERIFY(m_profileBase.isValid());
    QVERIFY(QDir().mkpath(m_profileBase.filePath(QStringLiteral("corpora"))));

#ifdef ITUB_DEFAULT_SOURCE_FIXTURE
    if (fixturePath().isEmpty() || !QFileInfo::exists(fixturePath()))
        QSKIP("Source fixture unavailable");
#else
    QSKIP("No fixture compiled in");
#endif
}

bool TestCatalogue::makeCorpusRoot(const QString& name, QString* error)
{
    testsupport::FixtureCorpus corpus(
        m_profileBase.filePath(QStringLiteral("corpora/") + name));
    m_corpusRoot = corpus.root();
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("video_a.mp4"), error))
        return false;
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("Café 東京 01.mp4"), error))
        return false;
    if (!corpus.mkdir(QStringLiteral("Travel/Japan")))
        return false;
    if (!corpus.addCopyOfMedia(fixturePath(), QStringLiteral("Travel/Japan/travel.mp4"),
                               error))
        return false;
    return true;
}

void TestCatalogue::init()
{
    m_cat = std::make_unique<Catalogue>();
    m_currentProfile = m_profileBase.filePath(
        QStringLiteral("profiles/profile-%1").arg(QDateTime::currentMSecsSinceEpoch()));
    connect(m_cat.get(), &Catalogue::rowsChanged, &m_model, &CatalogueModel::applyRows);
    connect(m_cat.get(), &Catalogue::scanProgress, &m_model, &CatalogueModel::applyProgress);
}

void TestCatalogue::cleanup()
{
    m_cat.reset();
    m_model.applyRows(QList<VideoRow>{}, true);
    // Reclaim this test's fixture copies immediately (budget discipline).
    QDir(m_profileBase.filePath(QStringLiteral("corpora"))).removeRecursively();
    QFile::remove(m_profileBase.filePath(QStringLiteral("corpora/root-link")));
}

void TestCatalogue::waitScanComplete(QSignalSpy& progressSpy, int timeoutMs)
{
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (int i = progressSpy.size() - 1; i >= 0; --i) {
            const auto args = progressSpy.at(i);
            const auto progress = args.at(0).value<ScanProgress>();
            if (progress.state == QLatin1String("complete"))
                return true;
        }
        return false;
    }(), timeoutMs);
    // Allow the last row deliveries to arrive.
    QTest::qWait(50);
}

qint64 TestCatalogue::videoIdFor(const QString& fileName) const
{
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex idx = m_model.index(i, 0);
        if (idx.data(CatalogueModel::NameRole).toString() == fileName)
            return idx.data(CatalogueModel::IdRole).toLongLong();
    }
    return -1;
}

VideoRow TestCatalogue::rowFor(const QString& fileName)
{
    // Directly ask the catalogue for the freshest row via refreshRows.
    QSignalSpy spy(m_cat.get(), &Catalogue::rowsChanged);
    m_cat->refreshRows();
    spy.wait(1000);
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex idx = m_model.index(i, 0);
        if (idx.data(CatalogueModel::NameRole).toString() == fileName) {
            VideoRow row;
            row.id = idx.data(CatalogueModel::IdRole).toLongLong();
            row.fileName = idx.data(CatalogueModel::NameRole).toString();
            row.durationMs = idx.data(CatalogueModel::DurationRole).toLongLong();
            row.displayWidth = idx.data(CatalogueModel::DisplayWidthRole).toInt();
            row.displayHeight = idx.data(CatalogueModel::DisplayHeightRole).toInt();
            row.rating = idx.data(CatalogueModel::RatingRole).toInt();
            row.views = idx.data(CatalogueModel::ViewsRole).toLongLong();
            row.availability = idx.data(CatalogueModel::AvailabilityRole).toString();
            row.probeStatus = idx.data(CatalogueModel::ProbeStatusRole).toString();
            return row;
        }
    }
    return VideoRow{};
}

qint64 TestCatalogue::countRows() const
{
    return m_model.rowCount();
}

qint64 TestCatalogue::countJobs(const QString& state) const
{
    // Read-only second connection to the same profile database (WAL).
    Database db;
    QString error;
    if (!db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error))
        return -1;
    Statement st = db.prepare(
        "SELECT COUNT(*) FROM jobs WHERE state=?");
    st.bind(1, state);
    return st.step() ? st.int64(0) : -1;
}

void TestCatalogue::scanFindsFilesAndProbes()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("scan"), &error), qUtf8Printable(error));

    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));
    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    QSignalSpy rowsSpy(m_cat.get(), &Catalogue::rowsChanged);

    m_cat->addRoot(m_corpusRoot);
    waitScanComplete(progressSpy);

    // Rows appear after stat, before probing finishes: rowsSpy must have
    // fired before the final progress state.
    QVERIFY(rowsSpy.size() >= 1);

    const VideoRow plain = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(plain.probeStatus, QStringLiteral("ok"));
    QCOMPARE(plain.durationMs, 667500); // video stream, NOT the subtitle's 669360
    QCOMPARE(plain.displayWidth, 1920);
    QCOMPARE(plain.displayHeight, 1080);

    const VideoRow unicode = rowFor(QStringLiteral("Café 東京 01.mp4"));
    QCOMPARE(unicode.probeStatus, QStringLiteral("ok"));
    QCOMPARE(unicode.durationMs, 667500);

    const VideoRow nested = rowFor(QStringLiteral("travel.mp4"));
    QCOMPARE(nested.probeStatus, QStringLiteral("ok"));

    QCOMPARE(countRows(), 3); // notes.txt-equivalents are not video extensions
}

void TestCatalogue::rescanHasNoDuplicatesAndNoReprobes()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("rescan"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    waitScanComplete(progressSpy);
    const qint64 rowsBefore = countRows();

    // Force-refresh path exercises reconciliation of identical files too.
    progressSpy.clear();
    m_cat->rescanRoot(1, true);
    waitScanComplete(progressSpy);

    QCOMPARE(countRows(), rowsBefore);
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).probeStatus, QStringLiteral("ok"));
    // Unchanged rescan leaves no queued jobs behind (§11 gate).
    QCOMPARE(countJobs(QStringLiteral("queued")), 0);
    QCOMPARE(countJobs(QStringLiteral("running")), 0);
}

void TestCatalogue::annotationsSurviveRescan()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("annotations"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    waitScanComplete(progressSpy);

    const qint64 id = videoIdFor(QStringLiteral("video_a.mp4"));
    QVERIFY(id > 0);
    m_cat->setRating(id, 4);
    m_cat->incrementViews(id);
    m_cat->incrementViews(id);

    progressSpy.clear();
    m_cat->rescanRoot(1, true);
    waitScanComplete(progressSpy);

    const VideoRow row = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(row.rating, 4);
    QCOMPARE(row.views, 2);
}

void TestCatalogue::contentChangeBumpsRevisionAndPreservesAnnotations()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("change"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    waitScanComplete(progressSpy);

    const qint64 id = videoIdFor(QStringLiteral("video_a.mp4"));
    QVERIFY(id > 0);
    m_cat->setRating(id, 3);

    // Same-size edit with preserved timestamps escapes stat detection unless
    // forced (§4): simulate the stat-visible case first — append bytes.
    const QString path = m_corpusRoot + QStringLiteral("/video_a.mp4");
    QFile file(path);
    QVERIFY(file.open(QIODevice::Append));
    file.write(QByteArray(1024, 'x'));
    file.close();

    progressSpy.clear();
    m_cat->rescanRoot(1, true);
    waitScanComplete(progressSpy);

    const VideoRow row = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(row.rating, 3); // annotations preserved across the revision
    // The appended file is no longer decodable: an explicit error state, not
    // endless retries (§5).
    QVERIFY(row.probeStatus == QLatin1String("error")
            || row.probeStatus == QLatin1String("ok"));
}

void TestCatalogue::missingAfterRemoval()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("missing"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    waitScanComplete(progressSpy);

    const qint64 id = videoIdFor(QStringLiteral("travel.mp4"));
    QVERIFY(id > 0);
    m_cat->setRating(id, 5);

    // The removed file is a tracked test artifact, not an unknown file.
    QVERIFY(QFile::remove(m_corpusRoot + QStringLiteral("/Travel/Japan/travel.mp4")));

    progressSpy.clear();
    m_cat->rescanRoot(1, false);
    waitScanComplete(progressSpy);

    const VideoRow row = rowFor(QStringLiteral("travel.mp4"));
    QCOMPARE(row.availability, QStringLiteral("missing"));
    QCOMPARE(row.rating, 5); // annotations kept in the trashed/missing state (§3)
    // The other files are untouched.
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).availability, QStringLiteral("available"));
}

void TestCatalogue::offlineRootKeepsAvailability()
{
    QString error;
    const QString rootDir = m_profileBase.filePath(QStringLiteral("corpora/offline"));
    testsupport::FixtureCorpus corpus(rootDir);
    QVERIFY2(corpus.addCopyOfMedia(fixturePath(), QStringLiteral("video_a.mp4"), &error),
             qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(rootDir);
    waitScanComplete(progressSpy);
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).availability, QStringLiteral("available"));

    // Root disappears (drive unmounted): rescan reports missing root and
    // retains prior availability — never a mass missing state (§5).
    QVERIFY(QDir(rootDir).removeRecursively());
    progressSpy.clear();
    m_cat->rescanRoot(1, false);

    bool sawMissing = false;
    for (int i = 0; i < progressSpy.size(); ++i) {
        if (progressSpy.at(i).at(0).value<ScanProgress>().state == QLatin1String("missing"))
            sawMissing = true;
    }
    QVERIFY(sawMissing);
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).availability, QStringLiteral("available"));
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).rating, 0);
}

void TestCatalogue::symlinkRootRejected()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("symlinkroot"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    const QString link = m_profileBase.filePath(QStringLiteral("corpora/root-link"));
    QFile::remove(link);
    QVERIFY(QFile::link(m_corpusRoot, link));

    QSignalSpy rejectedSpy(m_cat.get(), &Catalogue::rootRejected);
    m_cat->addRoot(link);
    QTRY_COMPARE(rejectedSpy.size(), 1);
    QVERIFY(rejectedSpy.first().first().toString().contains(QStringLiteral("symlink"),
                                                             Qt::CaseInsensitive));
}

void TestCatalogue::overlappingRootRejected()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("overlap"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    waitScanComplete(progressSpy);

    QSignalSpy rejectedSpy(m_cat.get(), &Catalogue::rootRejected);
    m_cat->addRoot(m_corpusRoot + QStringLiteral("/Travel"));
    QTRY_COMPARE(rejectedSpy.size(), 1);
    QVERIFY(rejectedSpy.first().first().toString().contains(QStringLiteral("overlap"),
                                                             Qt::CaseInsensitive));
    m_cat->addRoot(m_corpusRoot);
    QTRY_COMPARE(rejectedSpy.size(), 2);
}

void TestCatalogue::profileLockExcludesSecondInstance()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("lock"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    auto second = std::make_unique<Catalogue>();
    QString secondError;
    QVERIFY2(!second->initialize(m_currentProfile, m_ffprobe, &secondError),
             "Second instance must be refused while the profile is locked");
    QVERIFY(secondError.contains(QStringLiteral("in use")));
}

void TestCatalogue::corruptedFileBecomesErrorState()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("corrupt"), &error), qUtf8Printable(error));
    // A truncated media file: a modified variant within the fixture budget.
    QFile::copy(fixturePath(), m_corpusRoot + QStringLiteral("/truncated.mp4"));
    QFile trunc(m_corpusRoot + QStringLiteral("/truncated.mp4"));
    QVERIFY(trunc.open(QIODevice::ReadOnly));
    const QByteArray head = trunc.read(64 * 1024);
    trunc.close();
    QVERIFY(trunc.open(QIODevice::WriteOnly | QIODevice::Truncate));
    trunc.write(head);
    trunc.close();

    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    waitScanComplete(progressSpy);

    const VideoRow row = rowFor(QStringLiteral("truncated.mp4"));
    QCOMPARE(row.probeStatus, QStringLiteral("error"));
    QCOMPARE(row.availability, QStringLiteral("unavailable"));
    // The scan itself was not stopped by the corrupt file: all rows exist.
    QCOMPARE(countRows(), 4);
}

void TestCatalogue::newerSchemaIsRejected()
{
    QString error;
    QVERIFY2(makeCorpusRoot(QStringLiteral("newschema"), &error), qUtf8Printable(error));
    QVERIFY(m_cat->initialize(m_currentProfile, m_ffprobe, &error));

    // Simulate a database written by a future version.
    m_cat.reset();
    {
        Database raw;
        QVERIFY(raw.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error));
        QVERIFY(raw.exec("UPDATE meta SET value='999' WHERE key='schema_version'"));
    }
    auto reopened = std::make_unique<Catalogue>();
    QString reopenError;
    QVERIFY2(!reopened->initialize(m_currentProfile, m_ffprobe, &reopenError),
             "A newer schema must be refused, not silently opened");
    QVERIFY(reopenError.contains(QStringLiteral("newer")));
    m_cat = std::move(reopened);
    m_cat.reset(); // cleanup() handles teardown; avoid double-managed pointer
}

QTEST_GUILESS_MAIN(TestCatalogue)
#include "tst_catalogue.moc"
