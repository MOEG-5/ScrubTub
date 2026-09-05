// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// File-safety contract checks (TECH_SPEC.md sections 3 and 12, milestone 0).
//
// Every test operates on a freshly created disposable corpus in a temporary
// directory with a fresh application profile. The supplied original fixture is
// only ever read and verified unchanged at the end of the suite.
#include <QCryptographicHash>
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
#include "testsupport/TreeSnapshot.h"

#ifdef ITUB_DEFAULT_SOURCE_FIXTURE
#include <cstdlib>
#endif

using namespace itub;

class TestFileSafety : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void profileDirsAreAppOwned();
    void scannerFindsAndSkipsCorrectly();
    void scannerRejectsSymlinkedRoot();
    void noWritesDuringScan();
    void snapshotHarnessDetectsChanges();

private:
    bool makeCorpus(testsupport::FixtureCorpus** out, QString* error);
    QString originalFixture() const;
    bool fixtureHash(QString* statDesc, QByteArray* sha256Out) const;

    QTemporaryDir m_profileBase;
    QTemporaryDir m_corpusBase;
    QString m_originalFixture;
    QString m_originalFixtureStat;
    QByteArray m_originalFixtureHash;
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

#ifdef ITUB_DEFAULT_SOURCE_FIXTURE
    const QByteArray env = qgetenv("ITUB_TEST_SOURCE_VIDEO");
    m_originalFixture = env.isEmpty() ? QStringLiteral(ITUB_DEFAULT_SOURCE_FIXTURE)
                                      : QString::fromLocal8Bit(env);
#endif
    if (m_originalFixture.isEmpty() || !QFileInfo::exists(m_originalFixture))
        qInfo("No source fixture available; media-dependent tests will skip. "
              "Set ITUB_TEST_SOURCE_VIDEO to enable them.");
    else
        QVERIFY(fixtureHash(&m_originalFixtureStat, &m_originalFixtureHash));
}

void TestFileSafety::cleanupTestCase()
{
    // The original fixture must be byte- and metadata-identical after the suite.
    if (m_originalFixture.isEmpty())
        return;
    QString statNow;
    QByteArray hash;
    QVERIFY2(fixtureHash(&statNow, &hash), "Original fixture must stay readable");
    QCOMPARE(hash, m_originalFixtureHash);
    QCOMPARE(statNow, m_originalFixtureStat);
}

QString TestFileSafety::originalFixture() const
{
    return m_originalFixture;
}

bool TestFileSafety::fixtureHash(QString* statDesc, QByteArray* sha256Out) const
{
    QFileInfo info(m_originalFixture);
    *statDesc = QStringLiteral("%1|%2|%3")
                    .arg(info.size())
                    .arg(info.lastModified().toMSecsSinceEpoch())
                    .arg(info.metadataChangeTime().toMSecsSinceEpoch());
    if (sha256Out) {
        QFile file(m_originalFixture);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        char buffer[128 * 1024];
        while (!file.atEnd()) {
            const qint64 read = file.read(buffer, sizeof(buffer));
            if (read <= 0)
                return false;
            hash.addData(QByteArrayView(buffer, static_cast<int>(read)));
        }
        *sha256Out = hash.result().toHex();
    }
    return true;
}

bool TestFileSafety::makeCorpus(testsupport::FixtureCorpus** out, QString* error)
{
    static int counter = 0;
    auto corpus = std::make_unique<testsupport::FixtureCorpus>(
        m_corpusBase.filePath(QStringLiteral("corpus%1").arg(++counter)));

    if (originalFixture().isEmpty()) {
        *error = QStringLiteral("source fixture unavailable");
        return false;
    }
    if (!corpus->addCopyOfMedia(originalFixture(), QStringLiteral("Summer_Trip-2024_1080p.mp4"), error))
        return false;
    *out = corpus.release();
    return true;
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

void TestFileSafety::scannerFindsAndSkipsCorrectly()
{
    testsupport::FixtureCorpus corpus(m_corpusBase.filePath(QStringLiteral("corpus-names")));
    QString error;
    QVERIFY2(corpus.addCopyOfMedia(originalFixture(),
                                   QStringLiteral("Summer_Trip-2024_1080p.mp4"), &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addCopyOfMedia(originalFixture(),
                                   QStringLiteral("summer.trip.final.mp4"), &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addCopyOfMedia(originalFixture(),
                                   QStringLiteral("Café 東京 01.mp4"), &error),
             qUtf8Printable(error));
    QVERIFY2(corpus.addCopyOfMedia(originalFixture(),
                                   QStringLiteral("odd'name;&|$()!.mp4"), &error),
             qUtf8Printable(error));
    QVERIFY(corpus.mkdir(QStringLiteral("Travel/Japan")));
    QVERIFY2(corpus.addCopyOfMedia(originalFixture(),
                                   QStringLiteral("Travel/Japan/Summer_Trip.mp4"), &error),
             qUtf8Printable(error));
    QVERIFY(corpus.addTextFile(QStringLiteral("notes.txt"), QByteArrayLiteral("not a video\n"),
                               &error));
    QVERIFY(corpus.mkdir(QStringLiteral(".hiddendir")));
    QVERIFY2(corpus.addCopyOfMedia(originalFixture(),
                                   QStringLiteral(".hiddendir/secret.mp4"), &error),
             qUtf8Printable(error));

    if (originalFixture().isEmpty())
        QSKIP("Source fixture unavailable");

    DiscoveryOptions options;
    options.rootPath = corpus.root();
    const std::atomic_bool cancel = false;

    const DiscoveryResult result = SourceScanner::enumerateRoot(options, cancel);
    QVERIFY(result.rootAccepted);
    QVERIFY(result.completed);
    QCOMPARE(result.files.size(), 5); // hidden directory excluded by default
    QVERIFY(result.issues.isEmpty());

    QStringList found;
    for (const DiscoveredFile& file : result.files) {
        found << QDir(corpus.root()).relativeFilePath(file.absolutePath);
        QVERIFY(file.absolutePath.startsWith(corpus.root()));
        QVERIFY(file.sizeBytes > 0);
        QVERIFY(file.mtimeMs > 0);
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
    QCOMPARE(hiddenResult.files.size(), 6);
}

void TestFileSafety::scannerRejectsSymlinkedRoot()
{
    if (originalFixture().isEmpty())
        QSKIP("Source fixture unavailable");

    testsupport::FixtureCorpus corpus(m_corpusBase.filePath(QStringLiteral("corpus-symlink")));
    QString error;
    QVERIFY2(corpus.addCopyOfMedia(originalFixture(), QStringLiteral("video.mp4"), &error),
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
    testsupport::FixtureCorpus* corpus = nullptr;
    QString error;
    QVERIFY2(makeCorpus(&corpus, &error), qUtf8Printable(error));
    std::unique_ptr<testsupport::FixtureCorpus> guard(corpus);
    QVERIFY(corpus->addTextFile(QStringLiteral("notes.txt"), QByteArrayLiteral("metadata probe\n"), &error));

    const std::atomic_bool cancel = false;

    QVector<DiscoveredFile> sinkFiles;
    QVector<DiscoveredFile> cancelledFiles;
    std::atomic_bool cancelAfterFirstBatch = false;

    for (int pass = 0; pass < 2; ++pass) {
        DiscoveryOptions options;
        options.rootPath = corpus->root();
        options.includeHidden = pass == 1;
        QString snapshotError;
        testsupport::TreeSnapshot before;
        QVERIFY2(testsupport::snapshotTree(corpus->root(), &before, &snapshotError),
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
        QVERIFY2(testsupport::snapshotTree(corpus->root(), &after, &snapshotError),
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

QTEST_GUILESS_MAIN(TestFileSafety)
#include "tst_filesafety.moc"
