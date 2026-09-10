// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// File-safety contract checks (TECH_SPEC.md sections 3 and 12, milestone 0).
//
// The scanner never decodes media, so the controlled-name cases build their
// own disposable corpora from trivial files; sizes and mtimes are still the
// real stat values. The repository's vids/ corpus is scanned directly, as-is
// (AGENTS.md), and proven byte-identical afterwards with vidsFingerprint().
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "app/ProfilePaths.h"
#include "catalogue/SourceScanner.h"
#include "testsupport/FixtureCorpus.h"
#include "testsupport/MediaFixtures.h"
#include "testsupport/TreeSnapshot.h"

using namespace scrubtub;

class TestFileSafety : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void profileDirsAreAppOwned();
    void renamedAppReusesLegacyProfile();
    void scannerFindsAndSkipsCorrectly();
    void scannerRejectsSymlinkedRoot();
    void noWritesDuringScan();
    void snapshotHarnessDetectsChanges();
    void scansRealCorpusAsIs();

private:
    QTemporaryDir m_profileBase;
    QTemporaryDir m_corpusBase;
    QString m_vidsFingerprint;
};

void TestFileSafety::initTestCase()
{
    QVERIFY(m_profileBase.isValid());
    QVERIFY(m_corpusBase.isValid());

    // Fresh disposable application profile: redirect XDG paths into the test's
    // temporary directory before anything consults QStandardPaths.
    qputenv("XDG_DATA_HOME", m_profileBase.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_profileBase.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_profileBase.filePath(QStringLiteral("config")).toUtf8());

    // Fingerprint the read-only corpus up front; cleanupTestCase proves the
    // whole suite left it untouched.
    m_vidsFingerprint = testsupport::vidsFingerprint();
    if (!testsupport::vidsAvailable())
        qInfo("Real video corpus unavailable; the as-is corpus scan will skip. "
              "Set SCRUBTUB_TEST_VIDS_DIR to enable it.");
}

void TestFileSafety::cleanupTestCase()
{
    // The whole run, including the as-is corpus scan, must leave vids/ exactly
    // as it was (names, sizes, mtimes).
    QCOMPARE(testsupport::vidsFingerprint(), m_vidsFingerprint);
}

void TestFileSafety::profileDirsAreAppOwned()
{
    QString error;
    QVERIFY2(ProfilePaths::ensureDirs(QStringLiteral("tests-default"), &error),
             qUtf8Printable(error));
    const QString dataDir = ProfilePaths::profileDataDir(QStringLiteral("tests-default"));
    const QString cacheDir = ProfilePaths::profileCacheDir(QStringLiteral("tests-default"));
    QVERIFY(QDir(dataDir).exists());
    QVERIFY(QDir(cacheDir).exists());
    QVERIFY(QFile::exists(dataDir));
    // Directories live under the redirected test profile, never in a media
    // tree; the corpus base is a different temporary subtree.
    QVERIFY(!dataDir.startsWith(m_corpusBase.path()));
    QVERIFY(!cacheDir.startsWith(m_corpusBase.path()));
    // Profile IDs are sanitized against traversal.
    QVERIFY(ProfilePaths::profileDataDir(QStringLiteral("../escape")).endsWith(
        QStringLiteral("profiles/escape")));
}

void TestFileSafety::renamedAppReusesLegacyProfile()
{
    const QString oldOrg = QCoreApplication::organizationName();
    const QString oldName = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("scrubtub-project"));
    QCoreApplication::setApplicationName(QStringLiteral("scrubtub"));
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString legacy = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/itub-project/itub");
    const QString suffix = QStringLiteral("/profiles/default");
    const QString fresh = ProfilePaths::profileDataDir();
    const bool madeLegacy = QDir().mkpath(legacy + suffix);
    const QString reused = ProfilePaths::profileDataDir();
    const bool madeNew = QDir().mkpath(base + suffix);
    const QString preferred = ProfilePaths::profileDataDir();
    QCoreApplication::setOrganizationName(oldOrg);
    QCoreApplication::setApplicationName(oldName);
    QVERIFY(madeLegacy && madeNew);
    QCOMPARE(fresh, base + suffix);
    QCOMPARE(reused, legacy + suffix);
    QCOMPARE(preferred, base + suffix);
}

void TestFileSafety::scannerFindsAndSkipsCorrectly()
{
    // Controlled names, hidden directory and odd characters. The scanner never
    // decodes media, so trivial files are enough; sizes and mtimes are real.
    testsupport::FixtureCorpus corpus(m_corpusBase.filePath(QStringLiteral("corpus-names")));
    QString error;
    QVERIFY2(corpus.addTextFile(QStringLiteral("Summer_Trip-2024_1080p.mp4"),
                                QByteArray(4001, 'a'), &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral("summer.trip.final.mp4"),
                                QByteArray(1234, 'b'), &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral("Café 東京 01.mp4"), QByteArray(777, 'c'),
                                &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral("odd'name;&|$()!.mp4"), QByteArray(99, 'd'),
                                &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral("Travel/Japan/Summer_Trip.mp4"),
                                QByteArray(3210, 'e'), &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral("notes.txt"), QByteArrayLiteral("not a video\n"),
                                &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral(".hiddendir/secret.mp4"), QByteArray(555, 'f'),
                                &error),
             qUtf8Printable(error));

    DiscoveryOptions options;
    options.rootPath = corpus.root();
    const std::atomic_bool cancel = false;

    const DiscoveryResult result = SourceScanner::enumerateRoot(options, cancel);
    QVERIFY(result.rootAccepted);
    QVERIFY(result.completed);
    QCOMPARE(int(result.files.size()), 5); // hidden directory excluded by default
    QVERIFY(result.issues.isEmpty());

    QStringList found;
    for (const DiscoveredFile& file : result.files) {
        const QString relative = QDir(corpus.root()).relativeFilePath(file.absolutePath);
        found << relative;
        QVERIFY(file.absolutePath.startsWith(corpus.root()));
        const QFileInfo info(file.absolutePath);
        QVERIFY(info.isFile());
        // Real stat values survive the scan.
        QCOMPARE(qint64(file.sizeBytes), info.size());
        QVERIFY(file.mtimeMs > 0);
        QVERIFY(qAbs(file.mtimeMs - info.lastModified().toMSecsSinceEpoch()) <= 1);
    }
    found.sort();
    QCOMPARE(found, (QStringList{QStringLiteral("Café 東京 01.mp4"),
                                 QStringLiteral("Summer_Trip-2024_1080p.mp4"),
                                 QStringLiteral("Travel/Japan/Summer_Trip.mp4"),
                                 QStringLiteral("odd'name;&|$()!.mp4"),
                                 QStringLiteral("summer.trip.final.mp4")}));

    DiscoveryOptions includeHidden = options;
    includeHidden.includeHidden = true;
    const DiscoveryResult hiddenResult = SourceScanner::enumerateRoot(includeHidden, cancel);
    QCOMPARE(int(hiddenResult.files.size()), 6);
    QStringList hiddenFound;
    for (const DiscoveredFile& file : hiddenResult.files)
        hiddenFound << QDir(corpus.root()).relativeFilePath(file.absolutePath);
    QVERIFY2(hiddenFound.contains(QStringLiteral(".hiddendir/secret.mp4")),
             "includeHidden must discover the hidden directory");
}

void TestFileSafety::scannerRejectsSymlinkedRoot()
{
    testsupport::FixtureCorpus corpus(m_corpusBase.filePath(QStringLiteral("corpus-symlink")));
    QString error;
    QVERIFY2(corpus.addTextFile(QStringLiteral("video.mp4"), QByteArray(2048, 'v'), &error),
             qUtf8Printable(error));

    const QString link = m_corpusBase.filePath(QStringLiteral("root-link"));
    QFile::remove(link);
    QVERIFY2(QFile::link(corpus.root(), link), "Could not create test symlink");
    QVERIFY(QFileInfo(link).isSymLink());

    DiscoveryOptions options;
    options.rootPath = link;
    const std::atomic_bool cancel = false;
    const DiscoveryResult result = SourceScanner::enumerateRoot(options, cancel);
    QVERIFY2(!result.rootAccepted, "Symlinked root must be rejected");
    QVERIFY(result.files.isEmpty());
    QVERIFY(!result.issues.isEmpty());
    QVERIFY(result.issues.first().message.contains(QStringLiteral("symlink"),
                                                    Qt::CaseInsensitive));
}

void TestFileSafety::noWritesDuringScan()
{
    testsupport::FixtureCorpus corpus(m_corpusBase.filePath(QStringLiteral("corpus-writes")));
    QString error;
    QVERIFY2(corpus.addTextFile(QStringLiteral("clip-one.mp4"), QByteArray(1500, '1'), &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral("nested/clip-two.mkv"), QByteArray(2500, '2'),
                                &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addTextFile(QStringLiteral("notes.txt"),
                                QByteArrayLiteral("metadata probe\n"), &error),
             qUtf8Printable(error));

    const std::atomic_bool cancel = false;

    QVector<DiscoveredFile> sinkFiles;
    QVector<DiscoveredFile> cancelledFiles;
    std::atomic_bool cancelAfterFirstBatch = false;

    for (int pass = 0; pass < 2; ++pass) {
        DiscoveryOptions options;
        options.rootPath = corpus.root();
        options.includeHidden = pass == 1;
        QString snapshotError;
        testsupport::TreeSnapshot before;
        QVERIFY2(testsupport::snapshotTree(corpus.root(), &before, &snapshotError),
                 qUtf8Printable(snapshotError));

        const DiscoveryResult result = SourceScanner::enumerateRoot(options, cancel);
        QVERIFY(result.rootAccepted);

        const DiscoveryResult batched = SourceScanner::enumerateRoot(
            options, cancel,
            [&sinkFiles](QVector<DiscoveredFile>&& batch) {
                sinkFiles += batch;
            });
        QVERIFY(batched.rootAccepted);

        cancelAfterFirstBatch.store(false);
        const DiscoveryResult stopped = SourceScanner::enumerateRoot(
            options, cancelAfterFirstBatch,
            [&cancelledFiles, &cancelAfterFirstBatch](QVector<DiscoveredFile>&& batch) {
                cancelledFiles += batch;
                cancelAfterFirstBatch.store(true);
            });
        QVERIFY(!stopped.completed); // cancelled mid-run must not claim completion

        testsupport::TreeSnapshot after;
        QVERIFY2(testsupport::snapshotTree(corpus.root(), &after, &snapshotError),
                 qUtf8Printable(snapshotError));
        const QStringList diffs = testsupport::diffTrees(before, after);
        if (!diffs.isEmpty()) {
            const QString detail = QStringLiteral("Scan wrote to the source tree:\n  %1")
                                       .arg(diffs.join(QStringLiteral("\n  ")));
            QVERIFY2(diffs.isEmpty(), qUtf8Printable(detail));
        }
    }

    QVERIFY(!sinkFiles.isEmpty());
    QVERIFY(!cancelledFiles.isEmpty());
}

void TestFileSafety::snapshotHarnessDetectsChanges()
{
    testsupport::FixtureCorpus corpus(m_corpusBase.filePath(QStringLiteral("corpus-harness")));
    QString error;
    QVERIFY2(corpus.addTextFile(QStringLiteral("mutable.txt"), QByteArrayLiteral("before\n"), &error),
             qUtf8Printable(error));

    testsupport::TreeSnapshot before;
    QVERIFY(testsupport::snapshotTree(corpus.root(), &before, &error));

    QVERIFY2(corpus.addTextFile(QStringLiteral("mutable.txt"), QByteArrayLiteral("after!\n"), &error),
             qUtf8Printable(error));

    testsupport::TreeSnapshot after;
    QVERIFY(testsupport::snapshotTree(corpus.root(), &after, &error));
    const QStringList diffs = testsupport::diffTrees(before, after);
    QVERIFY2(!diffs.isEmpty(), "Harness must detect a deliberate change");
    QVERIFY(diffs.join(QLatin1Char(';')).contains(QStringLiteral("mutable.txt")));
}

// The real corpus is scanned directly and only read. Every supported file the
// helper reports must be discovered, and the folder must stay byte-identical.
void TestFileSafety::scansRealCorpusAsIs()
{
    if (!testsupport::vidsAvailable())
        QSKIP("Real video corpus unavailable; set SCRUBTUB_TEST_VIDS_DIR to scan it as-is");

    const QString rootPath = testsupport::vidsDir();
    QVERIFY(!rootPath.isEmpty());
    const QString fingerprintBefore = testsupport::vidsFingerprint();
    QVERIFY2(fingerprintBefore != QStringLiteral("unavailable"),
             "corpus fingerprint must be computable");

    DiscoveryOptions options;
    options.rootPath = rootPath;
    const std::atomic_bool cancel = false;
    const DiscoveryResult result = SourceScanner::enumerateRoot(options, cancel);
    QVERIFY2(result.rootAccepted, "Real corpus root must be accepted");
    QVERIFY2(result.completed, "Real corpus scan must complete");

    // Expected set: the helper's top-level listing filtered by the scanner's
    // own extension list. vidsFiles() is non-recursive by contract.
    QStringList expected;
    for (const QString& name : testsupport::vidsFiles()) {
        if (options.extensions.contains(QFileInfo(name).suffix().toLower()))
            expected << name;
    }
    expected.sort();
    QVERIFY2(!expected.isEmpty(), "Real corpus contains no supported video files");

    QStringList discoveredTopLevel;
    for (const DiscoveredFile& file : result.files) {
        QVERIFY2(file.absolutePath.startsWith(rootPath + QLatin1Char('/')),
                 qUtf8Printable(QStringLiteral("Discovered path outside the corpus: %1")
                                    .arg(file.absolutePath)));
        QVERIFY2(options.extensions.contains(QFileInfo(file.absolutePath).suffix().toLower()),
                 qUtf8Printable(QStringLiteral("Scanner returned an unsupported file: %1")
                                    .arg(file.absolutePath)));
        QCOMPARE(qint64(file.sizeBytes), QFileInfo(file.absolutePath).size());
        const QString relative = file.absolutePath.mid(rootPath.size() + 1);
        if (!relative.contains(QLatin1Char('/')))
            discoveredTopLevel << relative;
    }
    discoveredTopLevel.sort();
    QCOMPARE(discoveredTopLevel, expected);

    // Read-only proof around the scan itself.
    QCOMPARE(testsupport::vidsFingerprint(), fingerprintBefore);
}

QTEST_GUILESS_MAIN(TestFileSafety)
#include "tst_filesafety.moc"
