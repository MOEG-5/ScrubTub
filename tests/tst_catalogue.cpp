// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Catalogue contract checks (TECH_SPEC.md sections 4, 5, 12 "Reconciliation"):
// discovery of every supported file, scans do not duplicate rows,
// interrupted/offline scans never imply mass deletion, annotations survive
// rescans and revisions, changed content bumps the revision and invalidates
// extraction, and profile locks exclude a second instance.
//
// Media policy (AGENTS.md): the repository's vids/ corpus is scanned directly
// and as-is; expectations (file set, duration, resolution, codec) are derived
// at run time from the directory listing and from independent ffprobe runs.
// Cases that must mutate media (appended/removed/truncated files, offline
// roots) and cases that need controlled trees use original synthetic clips
// generated inside this test's temporary profile. Nothing here copies, links,
// renames, removes or writes anything under the corpus, which is fingerprinted
// before and after the suite.
//
// Runtime: a corpus scan probes ~269 files and then starts poster/storyboard
// work that this suite does not assert on. The waiting helpers stop waiting at
// probe completion and drop the queued preview backlog; the disposable profile
// is discarded afterwards.
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/Database.h"
#include "testsupport/MediaFixtures.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace scrubtub;

namespace {

// Scanner-accepted suffixes (SourceScanner.h defaults): the discovery check
// compares the model rows against exactly these files.
const QStringList& scannerSuffixes()
{
    static const QStringList suffixes{
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"),
        QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("mov"),
        QStringLiteral("wmv"), QStringLiteral("flv")};
    return suffixes;
}

bool isScannerVideo(const QString& fileName)
{
    return scannerSuffixes().contains(
        QFileInfo(fileName).suffix().toLower());
}

double jsonNumber(const QJsonValue& value, double fallback)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

int normalizedRotation(double raw)
{
    const int deg = static_cast<int>(std::lround(std::fabs(raw))) % 360;
    if (deg > 45 && deg < 135)
        return 90;
    if (deg >= 135 && deg <= 225)
        return 180;
    if (deg > 225 && deg < 315)
        return 270;
    return 0;
}

// Independent expectation for one file: a fresh ffprobe run parsed here, not a
// constant. Mirrors the documented selection rules (first video stream that is
// not attached cover art, its own duration with container fallback, display
// dimensions after rotation) so a mismatch points at the catalogue's probe.
struct ProbeExpectation {
    bool ok = false;
    qint64 durationMs = -1;
    int width = 0;
    int height = 0;
    QString codec;
    QString reason;
};

ProbeExpectation ffprobeExpectation(const QString& ffprobePath, const QString& filePath)
{
    ProbeExpectation out;
    QProcess process;
    process.start(ffprobePath,
                  {QStringLiteral("-v"), QStringLiteral("error"),
                   QStringLiteral("-print_format"), QStringLiteral("json"),
                   QStringLiteral("-show_streams"), QStringLiteral("-show_format"),
                   filePath});
    if (!process.waitForStarted(10000) || !process.waitForFinished(60000)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        out.reason = QStringLiteral("ffprobe did not succeed");
        return out;
    }
    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        out.reason = QStringLiteral("ffprobe JSON parse failure");
        return out;
    }
    const QJsonObject root = doc.object();
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();

    QJsonObject selected;
    for (const QJsonValue& value : streams) {
        const QJsonObject stream = value.toObject();
        if (stream.value(QStringLiteral("codec_type")).toString()
            != QLatin1String("video"))
            continue;
        if (stream.value(QStringLiteral("disposition")).toObject()
                .value(QStringLiteral("attached_pic")).toInt(0) == 1)
            continue;
        selected = stream;
        break;
    }
    if (selected.isEmpty()) {
        out.reason = QStringLiteral("no video stream");
        return out;
    }

    out.codec = selected.value(QStringLiteral("codec_name")).toString();
    const int width = static_cast<int>(jsonNumber(selected.value(QStringLiteral("width")), 0));
    const int height = static_cast<int>(jsonNumber(selected.value(QStringLiteral("height")), 0));
    if ((width > 0) != (height > 0) || width < 0 || height < 0) {
        out.reason = QStringLiteral("invalid stream dimensions");
        return out;
    }

    double durationSec = jsonNumber(selected.value(QStringLiteral("duration")), -1);
    if (!(durationSec > 0)) {
        durationSec = jsonNumber(
            root.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")),
            -1);
    }
    if (durationSec > 0)
        out.durationMs = static_cast<qint64>(std::llround(durationSec * 1000.0));

    const QJsonArray sideData = selected.value(QStringLiteral("side_data_list")).toArray();
    int rotation = 0;
    for (const QJsonValue& value : sideData) {
        const QJsonObject entry = value.toObject();
        if (entry.contains(QStringLiteral("rotation"))) {
            rotation = normalizedRotation(
                jsonNumber(entry.value(QStringLiteral("rotation")), 0));
            break;
        }
    }
    out.width = width;
    out.height = height;
    if (rotation == 90 || rotation == 270)
        std::swap(out.width, out.height);
    out.ok = true;
    return out;
}

} // namespace

class TestCatalogue : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();

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
    void v1DatabaseMigratesToCurrentSchema();
    void pendingRowsWithoutJobsGetReprobed();

private:
    // Wires a catalogue's signals to the model; every catalogue instance a
    // test ends up using (a replacement after a reopen) goes through this.
    void attachCatalogue(Catalogue* catalogue);

    // Synthetic (original) media in this test's own temporary directory.
    QString treePath(const QString& name) const;
    bool makeTree(const QString& name, const QStringList& relativeNames,
                  int durationMs, QString* error);

    // Scan/probe coordination. These stop at probe completion: poster and
    // storyboard work is not asserted by this suite.
    bool waitEnumeration(QSignalSpy& progressSpy, int timeoutMs);
    bool waitComplete(QSignalSpy& progressSpy, int timeoutMs);
    bool waitProbesSettled(int timeoutMs, bool dropPreviewBacklog = false);
    bool scanRoot(const QString& root, int expectedRows, bool dropPreviewBacklog = false);
    bool rescanRootWhenIdle(qint64 rootId, bool force, int expectedRows);
    bool stopPreviewWork();

    // Row/DB access.
    void refreshModel();
    QHash<QString, VideoRow> modelRowsByName() const;
    VideoRow rowFor(const QString& fileName);
    qint64 videoIdFor(const QString& fileName) const;
    qint64 queryInt(const QString& sql) const;
    qint64 countJobs(const QString& where) const;
    bool execSql(const QString& sql) const;
    bool dropQueuedPreviewJobs() const;

    QTemporaryDir m_profileBase;
    QString m_ffprobe;
    QString m_ffmpeg;
    QString m_corpusDir;
    QStringList m_corpusFiles;
    QString m_corpusFingerprint;
    std::unique_ptr<Catalogue> m_cat;
    CatalogueModel m_model;
    QString m_currentProfile;
};

void TestCatalogue::initTestCase()
{
    QVERIFY(m_profileBase.isValid());
    qputenv("XDG_DATA_HOME", m_profileBase.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_profileBase.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_profileBase.filePath(QStringLiteral("config")).toUtf8());

    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY2(!m_ffprobe.isEmpty(), "ffprobe must be installed for catalogue tests");
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!m_ffmpeg.isEmpty(), "ffmpeg must be installed for catalogue tests");

    if (!testsupport::vidsAvailable())
        QSKIP("test corpus unavailable: set SCRUBTUB_TEST_VIDS_DIR to a readable "
              "folder of videos (ScrubTub vids/); no media is copied for tests");

    m_corpusDir = testsupport::vidsDir();
    for (const QString& name : testsupport::vidsFiles())
        if (isScannerVideo(name))
            m_corpusFiles.append(name);
    QVERIFY2(!m_corpusFiles.isEmpty(),
             "the configured test corpus holds no scanner-supported video file");

    // Read-only guard: every test must leave the corpus byte-identical.
    m_corpusFingerprint = testsupport::vidsFingerprint();
}

void TestCatalogue::cleanupTestCase()
{
    // initTestCase skipped: there is no corpus whose read-only state to check.
    if (m_corpusFingerprint.isEmpty())
        return;
    QCOMPARE(testsupport::vidsFingerprint(), m_corpusFingerprint);
}

void TestCatalogue::init()
{
    m_cat = std::make_unique<Catalogue>();
    m_currentProfile = m_profileBase.filePath(
        QStringLiteral("profiles/profile-%1").arg(QDateTime::currentMSecsSinceEpoch()));
    attachCatalogue(m_cat.get());
}

void TestCatalogue::attachCatalogue(Catalogue* catalogue)
{
    // Direct connections: both objects live on this test thread.
    connect(catalogue, &Catalogue::rowsChanged, &m_model, &CatalogueModel::applyRows);
    connect(catalogue, &Catalogue::scanProgress, &m_model, &CatalogueModel::applyProgress);
}

void TestCatalogue::cleanup()
{
    m_cat.reset();
    m_model.applyRows(QList<VideoRow>{}, true);
}

QString TestCatalogue::treePath(const QString& name) const
{
    return m_profileBase.filePath(QStringLiteral("synthetic/") + name);
}

bool TestCatalogue::makeTree(const QString& name, const QStringList& relativeNames,
                             int durationMs, QString* error)
{
    const QString root = treePath(name);
    for (const QString& relative : relativeNames) {
        const QString path = QDir(root).filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            *error = QStringLiteral("cannot create %1").arg(path);
            return false;
        }
        if (!testsupport::makeSyntheticVideo(root, relative, durationMs, error))
            return false;
    }
    return true;
}

bool TestCatalogue::waitEnumeration(QSignalSpy& progressSpy, int timeoutMs)
{
    // "probing"/"complete"/"missing" all mean enumeration finished; probing
    // is the first one emitted, so this stops as soon as discovery is done.
    return QTest::qWaitFor([&progressSpy] {
        for (int i = 0; i < progressSpy.size(); ++i) {
            const QString state = progressSpy.at(i).at(0).value<ScanProgress>().state;
            if (state == QLatin1String("probing") || state == QLatin1String("complete")
                || state == QLatin1String("missing"))
                return true;
        }
        return false;
    }, timeoutMs);
}

bool TestCatalogue::waitComplete(QSignalSpy& progressSpy, int timeoutMs)
{
    return QTest::qWaitFor([&progressSpy] {
        for (int i = 0; i < progressSpy.size(); ++i)
            if (progressSpy.at(i).at(0).value<ScanProgress>().state
                == QLatin1String("complete"))
                return true;
        return false;
    }, timeoutMs);
}

bool TestCatalogue::waitProbesSettled(int timeoutMs, bool dropPreviewBacklog)
{
    // "Settled" = every discovered row carries a probe outcome and no probe
    // job is queued or running (a failed probe may be requeued once).
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (dropPreviewBacklog)
            dropQueuedPreviewJobs();
        const qint64 pendingRows =
            queryInt(QStringLiteral("SELECT COUNT(*) FROM videos WHERE probe_status='pending'"));
        const qint64 probeJobs =
            countJobs(QStringLiteral("kind='probe' AND state IN ('queued','running')"));
        if (pendingRows == 0 && probeJobs == 0) {
            QTest::qWait(50); // let the last row deliveries arrive
            return true;
        }
        QTest::qWait(100);
    }
    qWarning("probes did not settle: pending rows=%lld probe jobs=%lld",
             static_cast<long long>(queryInt(
                 QStringLiteral("SELECT COUNT(*) FROM videos WHERE probe_status='pending'"))),
             static_cast<long long>(countJobs(
                 QStringLiteral("kind='probe' AND state IN ('queued','running')"))));
    return false;
}

bool TestCatalogue::scanRoot(const QString& root, int expectedRows, bool dropPreviewBacklog)
{
    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(root);
    if (!waitEnumeration(progressSpy, 150000)) {
        qWarning("enumeration never finished for %s", qUtf8Printable(root));
        return false;
    }
    if (!QTest::qWaitFor([this, expectedRows] { return m_model.rowCount() == expectedRows; },
                         120000)) {
        qWarning("discovered %d of %d rows", m_model.rowCount(), expectedRows);
        return false;
    }
    return waitProbesSettled(150000, dropPreviewBacklog);
}

bool TestCatalogue::stopPreviewWork()
{
    // The catalogue refuses a rescan while any media job is active, and the
    // poster/storyboard passes are work this suite never asserts on. Cancel
    // the running previews and drop the queued backlog; a cancelled job is
    // requeued by the catalogue, so delete it again until the queue is empty.
    m_cat->cancelScanning();
    for (int attempt = 0; attempt < 100; ++attempt) {
        dropQueuedPreviewJobs();
        const qint64 running = countJobs(QStringLiteral("state='running'"));
        const qint64 previews = countJobs(QStringLiteral("kind IN ('poster','storyboard')"));
        if (running == 0 && previews == 0)
            return true;
        QTest::qWait(100);
    }
    qWarning("preview jobs did not stop: running=%lld previews=%lld",
             static_cast<long long>(countJobs(QStringLiteral("state='running'"))),
             static_cast<long long>(countJobs(QStringLiteral("kind IN ('poster','storyboard')"))));
    return false;
}

bool TestCatalogue::rescanRootWhenIdle(qint64 rootId, bool force, int expectedRows)
{
    if (!stopPreviewWork())
        return false;
    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    // The rescan is refused while the catalogue still considers a scan active;
    // retry briefly instead of assuming the previous one is fully torn down.
    bool started = false;
    for (int attempt = 0; attempt < 50 && !started; ++attempt) {
        m_cat->rescanRoot(rootId, force);
        started = QTest::qWaitFor([&progressSpy] { return progressSpy.size() > 0; }, 200);
    }
    if (!started) {
        qWarning("rescan was never accepted");
        return false;
    }
    if (!waitEnumeration(progressSpy, 150000))
        return false;
    if (!QTest::qWaitFor([this, expectedRows] { return m_model.rowCount() == expectedRows; },
                         120000))
        return false;
    return waitProbesSettled(150000);
}

void TestCatalogue::refreshModel()
{
    // Direct connection: rowsChanged is delivered synchronously on this thread.
    m_cat->refreshRows();
}

QHash<QString, VideoRow> TestCatalogue::modelRowsByName() const
{
    QHash<QString, VideoRow> rows;
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex idx = m_model.index(i, 0);
        VideoRow row;
        row.id = idx.data(CatalogueModel::IdRole).toLongLong();
        row.fileName = idx.data(CatalogueModel::NameRole).toString();
        row.relPath = idx.data(CatalogueModel::PathRole).toString();
        row.sizeBytes = idx.data(CatalogueModel::SizeRole).toLongLong();
        row.revision = idx.data(CatalogueModel::RevisionRole).toLongLong();
        row.durationMs = idx.data(CatalogueModel::DurationRole).toLongLong();
        row.displayWidth = idx.data(CatalogueModel::DisplayWidthRole).toInt();
        row.displayHeight = idx.data(CatalogueModel::DisplayHeightRole).toInt();
        row.codec = idx.data(CatalogueModel::CodecRole).toString();
        row.rating = idx.data(CatalogueModel::RatingRole).toInt();
        row.views = idx.data(CatalogueModel::ViewsRole).toLongLong();
        row.availability = idx.data(CatalogueModel::AvailabilityRole).toString();
        row.probeStatus = idx.data(CatalogueModel::ProbeStatusRole).toString();
        row.probeError = idx.data(CatalogueModel::ProbeErrorRole).toString();
        rows.insert(row.fileName, row);
    }
    return rows;
}

VideoRow TestCatalogue::rowFor(const QString& fileName)
{
    refreshModel();
    return modelRowsByName().value(fileName);
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

qint64 TestCatalogue::queryInt(const QString& sql) const
{
    // Second connection to this test's own profile database (WAL).
    Database db;
    QString error;
    if (!db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error))
        return -1;
    Statement st = db.prepare(sql.toUtf8().constData());
    if (!st.isValid() || !st.step())
        return -1;
    return st.int64(0);
}

qint64 TestCatalogue::countJobs(const QString& where) const
{
    return queryInt(QStringLiteral("SELECT COUNT(*) FROM jobs WHERE ") + where);
}

bool TestCatalogue::execSql(const QString& sql) const
{
    Database db;
    QString error;
    if (!db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error))
        return false;
    return db.exec(sql.toUtf8().constData());
}

bool TestCatalogue::dropQueuedPreviewJobs() const
{
    return execSql(QStringLiteral(
        "DELETE FROM jobs WHERE kind IN ('poster','storyboard') AND state='queued'"));
}

// ---------------------------------------------------------------------------

void TestCatalogue::scanFindsFilesAndProbes()
{
    QString error;
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    QSignalSpy rowsSpy(m_cat.get(), &Catalogue::rowsChanged);

    const int expected = m_corpusFiles.size();
    m_cat->addRoot(m_corpusDir);
    QVERIFY2(waitEnumeration(progressSpy, 150000), "corpus enumeration did not finish");

    // Rows appear after stat, before probing finishes: the model already holds
    // every discovered file while probe jobs are still running.
    QVERIFY2(QTest::qWaitFor([this, expected] { return m_model.rowCount() == expected; },
                             120000),
             qPrintable(QStringLiteral("discovered %1 of %2 corpus files")
                            .arg(m_model.rowCount()).arg(expected)));
    QVERIFY(rowsSpy.size() >= 1);

    // Poster/storyboard generation for the whole corpus is not asserted here;
    // dropping that backlog keeps the probe pass on the media slots.
    QVERIFY2(waitProbesSettled(150000, true), "corpus probes did not settle");

    refreshModel();
    const QHash<QString, VideoRow> rows = modelRowsByName();

    // Exactly the scanner-supported corpus files, each exactly one row.
    QCOMPARE(rows.size(), expected);
    for (const QString& name : m_corpusFiles)
        QVERIFY2(rows.contains(name), qUtf8Printable(name));
    // Unsupported extensions in the corpus (.swf here) never become rows.
    for (const QString& name : testsupport::vidsFiles())
        if (!isScannerVideo(name))
            QVERIFY2(!rows.contains(name), qUtf8Printable(name));

    // Probe results are cross-checked against an independent ffprobe run of the
    // same file — no constant durations or resolutions.
    int expectedOk = 0;
    for (const QString& name : m_corpusFiles) {
        const VideoRow row = rows.value(name);
        QVERIFY2(row.id > 0, qUtf8Printable(name));
        const ProbeExpectation expectation =
            ffprobeExpectation(m_ffprobe, QDir(m_corpusDir).filePath(name));
        if (!expectation.ok) {
            // A file the reference probe rejects must not be reported as ok.
            QVERIFY2(row.probeStatus == QLatin1String("error")
                         || row.probeStatus == QLatin1String("timeout"),
                     qPrintable(QStringLiteral("%1: catalogue reports %2, ffprobe says %3")
                                    .arg(name, row.probeStatus, expectation.reason)));
            continue;
        }
        ++expectedOk;
        QVERIFY2(row.probeStatus == QLatin1String("ok"),
                 qPrintable(QStringLiteral("%1: %2").arg(name, row.probeError)));
        QCOMPARE(row.availability, QStringLiteral("available"));
        QCOMPARE(row.durationMs, expectation.durationMs);
        QCOMPARE(row.displayWidth, expectation.width);
        QCOMPARE(row.displayHeight, expectation.height);
        QCOMPARE(row.codec, expectation.codec);
    }
    QCOMPARE(expectedOk, expected); // every corpus file is readable media
}

void TestCatalogue::rescanHasNoDuplicatesAndNoReprobes()
{
    QString error;
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    const int expected = m_corpusFiles.size();
    QVERIFY2(scanRoot(m_corpusDir, expected, true), "initial corpus scan failed");

    refreshModel();
    const QHash<QString, VideoRow> before = modelRowsByName();
    QCOMPARE(before.size(), expected);

    // Plain rescan of unchanged files: reconcile without duplicating rows and
    // without re-queuing probes for content that did not change.
    QVERIFY2(rescanRootWhenIdle(1, false, expected), "rescan was refused or failed");
    refreshModel();
    const QHash<QString, VideoRow> after = modelRowsByName();
    QCOMPARE(after.size(), expected);
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        const VideoRow row = after.value(it.key());
        QCOMPARE(row.id, it.value().id); // the same row, not a re-inserted copy
        QCOMPARE(row.availability, QStringLiteral("available"));
        QCOMPARE(row.probeStatus, QStringLiteral("ok"));
    }

    // Unchanged rescan leaves no probe work behind, even a moment later.
    QTest::qWait(500);
    QCOMPARE(countJobs(QStringLiteral("kind='probe' AND state IN ('queued','running')")), 0);
}

void TestCatalogue::annotationsSurviveRescan()
{
    QString error;
    const QString root = treePath(QStringLiteral("annotations"));
    QVERIFY2(makeTree(QStringLiteral("annotations"),
                      {QStringLiteral("video_a.mp4"), QStringLiteral("video_b.mp4")},
                      1000, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    QVERIFY2(scanRoot(root, 2), "synthetic scan failed");

    const qint64 id = videoIdFor(QStringLiteral("video_a.mp4"));
    QVERIFY(id > 0);
    m_cat->setRating(id, 4);
    m_cat->incrementViews(id);
    m_cat->incrementViews(id);

    // An unchanged rescan must not touch annotations or row identity.
    QVERIFY2(rescanRootWhenIdle(1, false, 2), "rescan failed");
    VideoRow row = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(row.id, id);
    QCOMPARE(row.rating, 4);
    QCOMPARE(row.views, 2);

    // A forced rescan re-probes even unchanged files; annotations and row
    // identity still survive, and no duplicate row appears.
    QVERIFY2(rescanRootWhenIdle(1, true, 2), "forced rescan failed");
    QCOMPARE(m_model.rowCount(), 2);
    row = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(row.id, id);
    QCOMPARE(row.rating, 4);
    QCOMPARE(row.views, 2);
    QCOMPARE(row.availability, QStringLiteral("available"));
}

void TestCatalogue::contentChangeBumpsRevisionAndPreservesAnnotations()
{
    QString error;
    const QString root = treePath(QStringLiteral("change"));
    QVERIFY2(makeTree(QStringLiteral("change"), {QStringLiteral("video_a.mp4")}, 1500, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    QVERIFY2(scanRoot(root, 1), "synthetic scan failed");

    const qint64 id = videoIdFor(QStringLiteral("video_a.mp4"));
    QVERIFY(id > 0);
    m_cat->setRating(id, 3);
    const VideoRow before = rowFor(QStringLiteral("video_a.mp4"));
    // Wait for extracted data of the current revision, so the invalidation
    // check below is not vacuous.
    QVERIFY2(QTest::qWaitFor([this, id, before] {
                 return queryInt(QStringLiteral(
                            "SELECT COUNT(*) FROM cache_entries WHERE video_id=%1 "
                            "AND revision=%2").arg(id).arg(before.revision)) > 0;
             }, 120000),
             "no cache entry was produced before the content change");

    // Stat-visible content change: appended bytes (no force needed).
    QFile file(QDir(root).filePath(QStringLiteral("video_a.mp4")));
    QVERIFY(file.open(QIODevice::Append));
    file.write(QByteArray(1024, 'x'));
    file.close();

    QVERIFY2(rescanRootWhenIdle(1, false, 1), "rescan failed");
    const VideoRow row = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(row.id, id);
    QCOMPARE(row.rating, 3); // annotations preserved across the revision
    QCOMPARE(row.revision, before.revision + 1);
    // The appended file may or may not stay decodable; either way the probe
    // reaches a terminal state rather than retrying forever (§5).
    QVERIFY(row.probeStatus == QLatin1String("ok")
            || row.probeStatus == QLatin1String("error")
            || row.probeStatus == QLatin1String("timeout"));
    // Extracted data of the superseded revision is invalidated with the bump.
    QCOMPARE(queryInt(QStringLiteral(
                 "SELECT COUNT(*) FROM cache_entries WHERE video_id=%1 AND revision=%2")
                 .arg(id).arg(before.revision)), 0);
}

void TestCatalogue::missingAfterRemoval()
{
    QString error;
    const QString root = treePath(QStringLiteral("missing"));
    QVERIFY2(makeTree(QStringLiteral("missing"),
                      {QStringLiteral("video_a.mp4"),
                       QStringLiteral("Travel/Japan/travel.mp4")},
                      1000, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    QVERIFY2(scanRoot(root, 2), "synthetic scan failed");

    // Nested files are tracked by their root-relative path.
    const VideoRow nested = rowFor(QStringLiteral("travel.mp4"));
    QVERIFY(nested.id > 0);
    QCOMPARE(nested.relPath, QStringLiteral("Travel/Japan/travel.mp4"));
    m_cat->setRating(nested.id, 5);

    QVERIFY(QFile::remove(QDir(root).filePath(QStringLiteral("Travel/Japan/travel.mp4"))));
    QVERIFY2(rescanRootWhenIdle(1, false, 2), "rescan failed");

    const VideoRow removed = rowFor(QStringLiteral("travel.mp4"));
    QCOMPARE(removed.availability, QStringLiteral("missing"));
    QCOMPARE(removed.rating, 5); // annotations kept in the missing state (§3)
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).availability, QStringLiteral("available"));
}

void TestCatalogue::offlineRootKeepsAvailability()
{
    QString error;
    const QString root = treePath(QStringLiteral("offline"));
    QVERIFY2(makeTree(QStringLiteral("offline"), {QStringLiteral("video_a.mp4")}, 1000, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));

    QSignalSpy progressSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(root);
    QVERIFY(waitEnumeration(progressSpy, 150000));
    QVERIFY(QTest::qWaitFor([this] { return m_model.rowCount() == 1; }, 120000));
    // Wait for the scan to fully finish so the rescan below is accepted.
    QVERIFY2(waitComplete(progressSpy, 120000), "scan never completed");
    const qint64 id = videoIdFor(QStringLiteral("video_a.mp4"));
    QVERIFY(id > 0);
    m_cat->setRating(id, 2);
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).availability, QStringLiteral("available"));

    // Root disappears (drive unmounted): the rescan reports the missing root
    // and retains prior availability — never a mass missing state (§5).
    QVERIFY(QDir(root).removeRecursively());
    QSignalSpy rescanSpy(m_cat.get(), &Catalogue::scanProgress);
    m_cat->rescanRoot(1, false);
    QVERIFY2(QTest::qWaitFor([&rescanSpy] {
                 for (int i = 0; i < rescanSpy.size(); ++i)
                     if (rescanSpy.at(i).at(0).value<ScanProgress>().state
                         == QLatin1String("missing"))
                         return true;
                 return false;
             }, 60000),
             "an offline root must be reported as missing");

    const VideoRow row = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(row.availability, QStringLiteral("available"));
    QCOMPARE(row.rating, 2);
}

void TestCatalogue::symlinkRootRejected()
{
    QString error;
    const QString target = treePath(QStringLiteral("symlink-target"));
    QVERIFY2(makeTree(QStringLiteral("symlink-target"), {QStringLiteral("video_a.mp4")},
                      1000, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));

    const QString link = m_profileBase.filePath(QStringLiteral("symlink-root"));
    QFile::remove(link);
    QVERIFY(QFile::link(target, link));

    QSignalSpy rejectedSpy(m_cat.get(), &Catalogue::rootRejected);
    m_cat->addRoot(link);
    QTRY_COMPARE(rejectedSpy.size(), 1);
    QVERIFY(rejectedSpy.first().first().toString().contains(QStringLiteral("symlink"),
                                                            Qt::CaseInsensitive));
    QCOMPARE(queryInt(QStringLiteral("SELECT COUNT(*) FROM roots")), 0);
}

void TestCatalogue::overlappingRootRejected()
{
    QString error;
    const QString root = treePath(QStringLiteral("overlap"));
    QVERIFY2(makeTree(QStringLiteral("overlap"),
                      {QStringLiteral("video_a.mp4"), QStringLiteral("Sub/video_b.mp4")},
                      1000, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    QVERIFY2(scanRoot(root, 2), "synthetic scan failed");

    QSignalSpy rejectedSpy(m_cat.get(), &Catalogue::rootRejected);
    m_cat->addRoot(QDir(root).filePath(QStringLiteral("Sub")));
    QTRY_COMPARE(rejectedSpy.size(), 1);
    QVERIFY(rejectedSpy.first().first().toString().contains(QStringLiteral("overlap"),
                                                            Qt::CaseInsensitive));
    m_cat->addRoot(root);
    QTRY_COMPARE(rejectedSpy.size(), 2);
    QVERIFY(rejectedSpy.at(1).first().toString().contains(QStringLiteral("already"),
                                                          Qt::CaseInsensitive));
    QCOMPARE(queryInt(QStringLiteral("SELECT COUNT(*) FROM roots")), 1);
    QCOMPARE(m_model.rowCount(), 2); // rejected roots never rescan or re-add rows
}

void TestCatalogue::profileLockExcludesSecondInstance()
{
    QString error;
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));

    auto second = std::make_unique<Catalogue>();
    QString secondError;
    QVERIFY2(!second->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &secondError),
             "Second instance must be refused while the profile is locked");
    QVERIFY(secondError.contains(QStringLiteral("in use")));
}

void TestCatalogue::corruptedFileBecomesErrorState()
{
    QString error;
    const QString root = treePath(QStringLiteral("corrupt"));
    QVERIFY2(makeTree(QStringLiteral("corrupt"), {QStringLiteral("good.mp4")}, 1000, &error),
             qUtf8Printable(error));

    // A truncated copy of this test's own synthetic clip: a decodable header
    // fragment with no complete stream.
    const QString good = QDir(root).filePath(QStringLiteral("good.mp4"));
    QFile source(good);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray head = source.read(1024);
    source.close();
    QVERIFY(!head.isEmpty());
    QFile broken(QDir(root).filePath(QStringLiteral("broken.mp4")));
    QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(broken.write(head) == head.size());
    broken.close();

    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    QVERIFY2(scanRoot(root, 2), "synthetic scan failed");

    const VideoRow row = rowFor(QStringLiteral("broken.mp4"));
    QCOMPARE(row.probeStatus, QStringLiteral("error"));
    QCOMPARE(row.availability, QStringLiteral("unavailable"));
    // The scan itself was not stopped by the corrupt file: all rows exist.
    QCOMPARE(m_model.rowCount(), 2);
    QCOMPARE(rowFor(QStringLiteral("good.mp4")).probeStatus, QStringLiteral("ok"));
}

void TestCatalogue::newerSchemaIsRejected()
{
    QString error;
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));

    // Simulate a database written by a future version.
    m_cat.reset();
    {
        Database raw;
        QVERIFY(raw.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error));
        QVERIFY(raw.exec("UPDATE meta SET value='999' WHERE key='schema_version'"));
    }
    auto reopened = std::make_unique<Catalogue>();
    QString reopenError;
    QVERIFY2(!reopened->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &reopenError),
             "A newer schema must be refused, not silently opened");
    QVERIFY(reopenError.contains(QStringLiteral("newer")));
}

// A database created before the stream_index column and settings table
// existed (stamped v1) must migrate on open — the user-reported stall:
// every probe dispatch failed with "no such column: stream_index".
void TestCatalogue::v1DatabaseMigratesToCurrentSchema()
{
    QString error;
    const QString root = treePath(QStringLiteral("migration"));
    QVERIFY2(makeTree(QStringLiteral("migration"),
                      {QStringLiteral("video_a.mp4"), QStringLiteral("video_b.mp4")},
                      1000, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));

    // Downgrade the just-created database to a genuine v1 layout.
    m_cat.reset();
    {
        Database raw;
        QVERIFY(raw.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error));
        QVERIFY(raw.exec("DROP TABLE settings"));
        QVERIFY(raw.exec("ALTER TABLE videos DROP COLUMN stream_index"));
        QVERIFY(raw.exec("UPDATE meta SET value='1' WHERE key='schema_version'"));
    }

    // Reopening must migrate v1 → v2 transparently.
    auto reopened = std::make_unique<Catalogue>();
    QVERIFY2(reopened->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    m_cat = std::move(reopened);
    attachCatalogue(m_cat.get());

    {
        Database raw;
        QVERIFY(raw.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error));
        const auto version = raw.scalarInt(
            "SELECT value FROM meta WHERE key='schema_version'");
        QVERIFY(version.has_value());
        QCOMPARE(version.value(), 2);
        QCOMPARE(raw.scalarInt("SELECT COUNT(*) FROM pragma_table_info('videos') "
                               "WHERE name='stream_index'").value_or(0), qint64(1));
    }

    // And the migrated database fully works: scan + probe to completion.
    QVERIFY2(scanRoot(root, 2), "scan after migration failed");
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).probeStatus, QStringLiteral("ok"));
    QCOMPARE(rowFor(QStringLiteral("video_b.mp4")).probeStatus, QStringLiteral("ok"));
}

// Recovery for the user-reported state: rows left 'pending' with no job
// rows at all (dispatch failures deleted them) must be re-probed by a
// normal rescan, without needing force refresh.
void TestCatalogue::pendingRowsWithoutJobsGetReprobed()
{
    QString error;
    const QString root = treePath(QStringLiteral("orphanjobs"));
    QVERIFY2(makeTree(QStringLiteral("orphanjobs"),
                      {QStringLiteral("video_a.mp4"), QStringLiteral("video_b.mp4")},
                      1000, &error),
             qUtf8Printable(error));
    QVERIFY2(m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error),
             qUtf8Printable(error));
    QVERIFY2(scanRoot(root, 2), "synthetic scan failed");
    QVERIFY2(stopPreviewWork(), "preview work did not stop");

    // Simulate the pre-migration stall: all jobs gone, rows back to pending.
    QVERIFY(execSql(QStringLiteral("DELETE FROM jobs")));
    QVERIFY(execSql(QStringLiteral(
        "UPDATE videos SET probe_status='pending', availability='unprobed', "
        "duration_ms=NULL, codec=NULL, display_width=NULL, display_height=NULL")));
    refreshModel();
    QCOMPARE(rowFor(QStringLiteral("video_a.mp4")).probeStatus, QStringLiteral("pending"));

    QVERIFY2(rescanRootWhenIdle(1, false, 2), "rescan failed");
    const VideoRow row = rowFor(QStringLiteral("video_a.mp4"));
    QCOMPARE(row.probeStatus, QStringLiteral("ok"));
    QCOMPARE(row.availability, QStringLiteral("available"));
    QVERIFY(row.durationMs > 0);
}

QTEST_GUILESS_MAIN(TestCatalogue)
#include "tst_catalogue.moc"
