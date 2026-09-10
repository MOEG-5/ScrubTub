// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Milestone-4 contract checks (TECH_SPEC.md sections 3, 4, 12): explicit
// authorized mutation via platform Trash only, identity re-check, no
// permanent-delete fallback, backup/restore preserving all annotations,
// schema and integrity validation, and cache controls limited to owned
// artifacts.
//
// Media rules (AGENTS.md): the mutation cases use synthetic ORIGINAL clips
// under the test profile so nothing here can touch the owner's library, and
// they live on the same filesystem as the redirected XDG data dir so the
// platform Trash facility works. The repository's vids/ corpus is only ever
// read as-is; the read-only case proves it via vidsFingerprint().
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/Database.h"
#include "testsupport/MediaFixtures.h"

#include <memory>

using namespace scrubtub;

namespace {

// Short but long enough for a probe, a poster and a storyboard pass.
constexpr int kSyntheticDurationMs = 1000;

QString syntheticCorpusDir(const QString& name)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/corpora/") + name;
}

} // namespace

class TestFileOps : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void addRootFromFileUrlWithSpecialCharacters();
    void authorizedTrashRemovesOnlyTheSelection();
    void trashAbortsOnIdentityChange();
    void trashFailureKeepsEntry();
    void backupPreservesAnnotationsAcrossRestore();
    void restoreRejectsNewerSchema();
    void restoreRejectsCorruptBackup();
    void clearPreviewsKeepsAnnotations();
    void sizeFormattingUsesIecUnits();
    void realCorpusSmallestVideoStaysReadOnly();

private:
    bool makeCatalogue(const QString& name);
    qint64 idForName(const QString& name) const;
    void drain();
    qint64 jobCount(const char* state) const;
    QString trashDir() const;

    QTemporaryDir m_profileBase;
    QString m_ffprobe;
    QString m_ffmpeg;
    std::unique_ptr<Catalogue> m_cat;
    CatalogueModel m_model;
    QString m_corpusRoot;
    QString m_currentProfile;
    QString m_vidsFingerprint;
};

void TestFileOps::initTestCase()
{
    // Every artifact lives under this temporary profile: the database, the
    // cache, and the synthetic corpora. The owner's real profile is untouched.
    QVERIFY(m_profileBase.isValid());
    qputenv("XDG_DATA_HOME", m_profileBase.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_profileBase.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_profileBase.filePath(QStringLiteral("config")).toUtf8());
    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!m_ffprobe.isEmpty() && !m_ffmpeg.isEmpty(),
             "ffprobe and ffmpeg are required to synthesize test media");
    m_vidsFingerprint = testsupport::vidsFingerprint();
    if (!testsupport::vidsAvailable())
        qInfo("Real video corpus unavailable; the read-only corpus case will skip. "
              "Set SCRUBTUB_TEST_VIDS_DIR to enable it.");
}

void TestFileOps::cleanupTestCase()
{
    // The Trash directory this run created belongs to this run. It lives under
    // the redirected XDG data home, so nothing outside the temp profile goes.
    QDir(trashDir()).removeRecursively();

    // No case may have written to the read-only corpus.
    QCOMPARE(testsupport::vidsFingerprint(), m_vidsFingerprint);
}

QString TestFileOps::trashDir() const
{
    return m_profileBase.filePath(QStringLiteral("data/Trash"));
}

bool TestFileOps::makeCatalogue(const QString& name)
{
    m_cat = std::make_unique<Catalogue>();
    QString error;
    m_currentProfile = m_profileBase.filePath(
        QStringLiteral("profiles/%1-%2").arg(name).arg(QDateTime::currentMSecsSinceEpoch()));
    if (!m_cat->initialize(m_currentProfile, m_ffprobe, m_ffmpeg, &error))
        return false;
    connect(m_cat.get(), &Catalogue::rowsChanged, &m_model, &CatalogueModel::applyRows,
            Qt::DirectConnection);

    // Synthetic ORIGINALS (never derived from vids/) on the same filesystem as
    // the redirected XDG data dir, so the platform Trash facility works for
    // the mutation cases.
    m_corpusRoot = syntheticCorpusDir(name);
    if (!testsupport::makeSyntheticVideos(
            m_corpusRoot,
            {QStringLiteral("video_a.mp4"), QStringLiteral("video_b.mp4")},
            kSyntheticDurationMs, &error))
        return false;

    QSignalSpy progress(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(m_corpusRoot);
    const bool done = QTest::qWaitFor([&] {
        for (int i = progress.size() - 1; i >= 0; --i)
            if (progress.at(i).at(0).value<ScanProgress>().state == QLatin1String("complete"))
                return true;
        return false;
    }, 120000);
    if (!done)
        return false;
    drain();
    return true;
}

qint64 TestFileOps::jobCount(const char* state) const
{
    Database db;
    QString error;
    if (!db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error))
        return -1;
    Statement st = db.prepare("SELECT COUNT(*) FROM jobs WHERE state=?");
    st.bind(1, QString::fromLatin1(state));
    return st.step() ? st.int64(0) : qint64(-1);
}

void TestFileOps::drain()
{
    QTRY_COMPARE_WITH_TIMEOUT(jobCount("queued"), 0, 180000);
    QTRY_COMPARE_WITH_TIMEOUT(jobCount("running"), 0, 180000);
    QTest::qWait(150);
}

qint64 TestFileOps::idForName(const QString& name) const
{
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex idx = m_model.index(i, 0);
        if (idx.data(CatalogueModel::NameRole).toString() == name)
            return idx.data(CatalogueModel::IdRole).toLongLong();
    }
    return -1;
}

// Regression: the QML folder picker hands the catalogue a percent-encoded
// file:// URL; the root must be added with its decoded native path and
// scanned normally (user report: "[SubsPlease] ... (1080p) [Batch]").
void TestFileOps::addRootFromFileUrlWithSpecialCharacters()
{
    QVERIFY(makeCatalogue(QStringLiteral("urlroots")));
    QString error;
    const QString bracketedRoot = syntheticCorpusDir(
        QStringLiteral("[SubsPlease] Kanojo, Okarishimasu (01-12) (1080p) [Batch]"));
    QVERIFY2(testsupport::makeSyntheticVideo(
                 bracketedRoot, QStringLiteral("episode 01.mp4"), kSyntheticDurationMs, &error),
             qUtf8Printable(error));
    const QString seasonDir = bracketedRoot + QStringLiteral("/Season 1/[Group] Name (2024)");
    QVERIFY(QDir().mkpath(seasonDir));
    QVERIFY2(testsupport::makeSyntheticVideo(
                 seasonDir, QStringLiteral("episode 02.mp4"), kSyntheticDurationMs, &error),
             qUtf8Printable(error));

    // Exercise the encoded URL form independently of Qt's display formatting
    // defaults (which differ between supported Qt releases).
    const QString url = QUrl::fromLocalFile(bracketedRoot + QLatin1Char('/'))
                            .toString(QUrl::FullyEncoded)
                            .replace(QLatin1Char('['), QStringLiteral("%5B"))
                            .replace(QLatin1Char(']'), QStringLiteral("%5D"));
    QVERIFY2(url.contains(QStringLiteral("%5BSubsPlease%5D")),
             "test setup expected a percent-encoded URL");
    QVERIFY2(url.startsWith(QStringLiteral("file://")), "expected a file:// URL");

    connect(m_cat.get(), &Catalogue::rootRejected, this, [this](const QString& reason) {
        QFAIL(qUtf8Printable(QStringLiteral("root rejected: %1").arg(reason)));
    });

    QSignalSpy progress(m_cat.get(), &Catalogue::scanProgress);
    m_cat->addRoot(url);
    const bool done = QTest::qWaitFor([&] {
        for (int i = progress.size() - 1; i >= 0; --i)
            if (progress.at(i).at(0).value<ScanProgress>().state == QLatin1String("complete"))
                return true;
        return false;
    }, 120000);
    QVERIFY2(done, "scan of the URL-added root did not complete");
    drain();

    // Two videos discovered under the decoded native path.
    bool found01 = false;
    bool found02 = false;
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex row = m_model.index(i, 0);
        const QString name = row.data(CatalogueModel::NameRole).toString();
        if (name == QLatin1String("episode 01.mp4"))
            found01 = true;
        if (name == QLatin1String("episode 02.mp4"))
            found02 = true;
    }
    QVERIFY2(found01 && found02, "videos under the bracketed folder were not discovered");
}

void TestFileOps::authorizedTrashRemovesOnlyTheSelection()
{
    QVERIFY(makeCatalogue(QStringLiteral("trash")));
    const qint64 idA = idForName(QStringLiteral("video_a.mp4"));
    const qint64 idB = idForName(QStringLiteral("video_b.mp4"));
    QVERIFY(idA > 0 && idB > 0);
    m_cat->setRating(idA, 5);

    // Explicit action for one selection (confirmation happens in the UI).
    QSignalSpy results(m_cat.get(), &Catalogue::trashResult);
    m_cat->trashVideos({idA});

    QCOMPARE(results.size(), 1);
    if (!results.first().at(1).toBool())
        qWarning("trash failed: %s", qUtf8Printable(results.first().at(2).toString()));
    QCOMPARE(results.first().at(1).toBool(), true);
    // The selected file left its directory; the sibling is untouched.
    QVERIFY(!QFile::exists(m_corpusRoot + QStringLiteral("/video_a.mp4")));
    QVERIFY(QFile::exists(m_corpusRoot + QStringLiteral("/video_b.mp4")));
    // The file went to the platform Trash, not to a permanent delete.
    QVERIFY(QDir(trashDir()).exists());
    QVERIFY(!QDir(trashDir() + QStringLiteral("/files")).entryList(QDir::Files).isEmpty());
    // The entry remains with annotations, reported missing (§3).
    QTest::qWait(100);
    bool found = false;
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex row = m_model.index(i, 0);
        if (row.data(CatalogueModel::IdRole).toLongLong() == idA) {
            found = true;
            QCOMPARE(row.data(CatalogueModel::AvailabilityRole).toString(),
                     QStringLiteral("missing"));
            QCOMPARE(row.data(CatalogueModel::RatingRole).toInt(), 5);
        }
    }
    QVERIFY2(found, "trashed entry must remain in the catalogue");
}

void TestFileOps::trashAbortsOnIdentityChange()
{
    QVERIFY(makeCatalogue(QStringLiteral("identity")));
    const qint64 idA = idForName(QStringLiteral("video_a.mp4"));
    QVERIFY(idA > 0);

    // The file changed between confirmation and the operation: that item is
    // aborted and nothing is deleted (§3).
    QFile file(m_corpusRoot + QStringLiteral("/video_a.mp4"));
    QVERIFY(file.open(QIODevice::Append));
    file.write(QByteArray(64, 'x'));
    file.close();

    QSignalSpy results(m_cat.get(), &Catalogue::trashResult);
    m_cat->trashVideos({idA});
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().at(1).toBool(), false);
    QVERIFY2(results.first().at(2).toString().contains(QStringLiteral("changed")),
             "reason must identify the identity mismatch");
    QVERIFY(QFile::exists(m_corpusRoot + QStringLiteral("/video_a.mp4")));
}

void TestFileOps::trashFailureKeepsEntry()
{
    QVERIFY(makeCatalogue(QStringLiteral("trashfail")));
    const qint64 idA = idForName(QStringLiteral("video_a.mp4"));
    QVERIFY(idA > 0);

    // A missing file cannot be trashed; the entry stays with a reason and
    // there is no permanent-delete fallback (§3).
    QVERIFY(QFile::remove(m_corpusRoot + QStringLiteral("/video_a.mp4")));
    QSignalSpy results(m_cat.get(), &Catalogue::trashResult);
    m_cat->trashVideos({idA});
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().at(1).toBool(), false);
    QVERIFY(!results.first().at(2).toString().isEmpty());
    bool entryGone = true;
    for (int i = 0; i < m_model.rowCount(); ++i)
        if (m_model.index(i, 0).data(CatalogueModel::IdRole).toLongLong() == idA)
            entryGone = false;
    QVERIFY2(!entryGone, "failed trash must leave the catalogue entry");
}

void TestFileOps::backupPreservesAnnotationsAcrossRestore()
{
    QVERIFY(makeCatalogue(QStringLiteral("backup")));
    const qint64 idA = idForName(QStringLiteral("video_a.mp4"));
    QVERIFY(idA > 0);
    m_cat->setRating(idA, 3);
    m_cat->addManualTag(idA, QStringLiteral("holiday"));
    drain();

    // Export a consistent snapshot.
    const QString backupPath = m_profileBase.filePath(QStringLiteral("backup.db"));
    QSignalSpy exported(m_cat.get(), &Catalogue::backupExported);
    m_cat->exportBackup(backupPath);
    QCOMPARE(exported.size(), 1);
    QVERIFY(QFile::exists(backupPath));

    // Restore into a fresh profile with the same media: annotations survive.
    auto second = std::make_unique<Catalogue>();
    CatalogueModel secondModel;
    QString error;
    const QString freshProfile = m_profileBase.filePath(
        QStringLiteral("profiles/restored-%1").arg(QDateTime::currentMSecsSinceEpoch()));
    QVERIFY(second->initialize(freshProfile, m_ffprobe, m_ffmpeg, &error));
    connect(second.get(), &Catalogue::rowsChanged, &secondModel,
            &CatalogueModel::applyRows, Qt::DirectConnection);
    second->addRoot(m_corpusRoot);
    const bool done = QTest::qWaitFor([&] {
        return secondModel.rowCount() >= 2;
    }, 120000);
    QVERIFY(done);

    QSignalSpy imported(second.get(), &Catalogue::backupImported);
    second->importBackup(backupPath);
    QCOMPARE(imported.size(), 1);
    QTest::qWait(200);

    bool restored = false;
    for (int i = 0; i < secondModel.rowCount(); ++i) {
        const QModelIndex row = secondModel.index(i, 0);
        if (row.data(CatalogueModel::NameRole).toString() == QLatin1String("video_a.mp4")) {
            QCOMPARE(row.data(CatalogueModel::RatingRole).toInt(), 3);
            restored = true;
        }
    }
    QVERIFY2(restored, "rating did not survive restore");
}

void TestFileOps::restoreRejectsNewerSchema()
{
    QVERIFY(makeCatalogue(QStringLiteral("newbackup")));
    // A backup from a future version must be refused (§4).
    const QString futurePath = m_profileBase.filePath(QStringLiteral("future.db"));
    {
        Database raw;
        QString error;
        QVERIFY(raw.open(futurePath, &error));
        QVERIFY(raw.exec("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)"));
        QVERIFY(raw.exec("INSERT INTO meta VALUES('schema_version','999')"));
    }
    QSignalSpy failed(m_cat.get(), &Catalogue::operationFailed);
    m_cat->importBackup(futurePath);
    QCOMPARE(failed.size(), 1);
    QVERIFY2(failed.first().first().toString().contains(QStringLiteral("newer")),
             qUtf8Printable(failed.first().first().toString()));
}

void TestFileOps::restoreRejectsCorruptBackup()
{
    QVERIFY(makeCatalogue(QStringLiteral("corruptbackup")));
    const QString badPath = m_profileBase.filePath(QStringLiteral("bad.db"));
    QFile bad(badPath);
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("this is not a database, just noise");
    bad.close();

    QSignalSpy failed(m_cat.get(), &Catalogue::operationFailed);
    m_cat->importBackup(badPath);
    QVERIFY(failed.size() >= 1);
    // The active catalogue still works after the refused restore.
    m_cat->refreshRows();
    QTest::qWait(100);
    QCOMPARE(m_model.rowCount(), 2);
}

void TestFileOps::clearPreviewsKeepsAnnotations()
{
    QVERIFY(makeCatalogue(QStringLiteral("clearcache")));
    const qint64 idA = idForName(QStringLiteral("video_a.mp4"));
    m_cat->setRating(idA, 2);
    drain(); // posters generated

    Database db;
    QString error;
    QVERIFY(db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error));
    qint64 entries = -1;
    {
        Statement st = db.prepare("SELECT COUNT(*) FROM cache_entries");
        QVERIFY(st.step());
        entries = st.int64(0);
    }
    QVERIFY(entries > 0);

    QSignalSpy usage(m_cat.get(), &Catalogue::cacheUsageReady);
    m_cat->clearPreviews();

    // clearPreviews() drops every entry synchronously. The poster jobs it
    // re-queues for the next pass must not have run yet, so the check happens
    // before spinning the event loop.
    {
        Statement st = db.prepare("SELECT COUNT(*) FROM cache_entries");
        QVERIFY(st.step());
        QCOMPARE(st.int64(0), 0);
    }
    QVERIFY(usage.size() >= 1);

    // Ratings survive; only owned artifacts were removed (§6).
    m_cat->refreshRows();
    QTest::qWait(100);
    bool ratingKept = false;
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const QModelIndex row = m_model.index(i, 0);
        if (row.data(CatalogueModel::IdRole).toLongLong() == idA)
            ratingKept = row.data(CatalogueModel::RatingRole).toInt() == 2;
    }
    QVERIFY2(ratingKept, "clear previews must not touch ratings");
}

// §1: IEC units with the correct factor (user-reported GiB/MiB mislabel).
void TestFileOps::sizeFormattingUsesIecUnits()
{
    // The user-reported case: 1427.8 MiB is above 1 GiB → shown as GiB with
    // the GiB factor (was mislabeled "1427.60 GiB" using the MiB factor).
    QCOMPARE(formatIecBytes(1427 * 1024 * 1024 + 823 * 1024),
             QStringLiteral("1.39 GiB"));
    QCOMPARE(formatIecBytes(qint64(2) * 1024 * 1024 * 1024),
             QStringLiteral("2.00 GiB"));
    QCOMPARE(formatIecBytes(512 * 1024 * 1024), QStringLiteral("512.0 MiB"));
    QCOMPARE(formatIecBytes(-1), QStringLiteral("?"));
}

// The repository's vids/ corpus is used as-is: a real, already-existing clip
// is decoded read-only and the whole folder must stay byte-identical.
void TestFileOps::realCorpusSmallestVideoStaysReadOnly()
{
    if (!testsupport::vidsAvailable())
        QSKIP("Real video corpus unavailable; set SCRUBTUB_TEST_VIDS_DIR to scan it as-is");

    const QString video = testsupport::smallestVideo();
    QVERIFY2(!video.isEmpty(), "corpus advertises media but has no video file");
    const QString before = testsupport::vidsFingerprint();
    QVERIFY2(before != QStringLiteral("unavailable"), "corpus fingerprint must be computable");

    QProcess decode;
    decode.start(m_ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
                            QStringLiteral("-nostdin"), QStringLiteral("-i"), video,
                            QStringLiteral("-frames:v"), QStringLiteral("1"),
                            QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
    QVERIFY2(decode.waitForStarted(10000), "ffmpeg did not start");
    QVERIFY2(decode.waitForFinished(120000), "decoding the corpus video timed out");
    QCOMPARE(decode.exitStatus(), QProcess::NormalExit);
    QCOMPARE(decode.exitCode(), 0);

    QCOMPARE(testsupport::vidsFingerprint(), before);
}

QTEST_GUILESS_MAIN(TestFileOps)
#include "tst_fileops.moc"
