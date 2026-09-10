// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Search and tag contract checks (TECH_SPEC.md sections 7, 8, 12):
// table-driven filename tag examples (media-free), Unicode normalization,
// short queries, typo cutoffs, combined range/tag filters, nulls, stable ties,
// manual/automatic tag overlap, suppressed tags surviving rescans, and a fuzzy
// match paged beyond the first detail page.
//
// Media policy (AGENTS.md): catalogue-level cases search the repository's
// vids/ corpus directly and as-is; names, tokens, counts, bounds and sort
// orders are derived at run time from the scanned rows, never hard-coded.
// Controlled trees (folder tags, folderPrefix, tag suppression across rescans)
// and the diacritic-folding case use original synthetic clips generated inside
// this test's temporary profile. Nothing here copies, links, renames, removes
// or writes under the corpus, which is fingerprinted before and after the
// suite.
//
// Runtime: the corpus is scanned once for the whole suite (the library) and
// probe completion is the condition tests wait for; poster/storyboard work is
// not asserted and is stopped, so the disposable profile stays cheap.
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <rapidfuzz/distance/Levenshtein.hpp>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/CatalogueProxy.h"
#include "catalogue/Database.h"
#include "catalogue/TagEngine.h"
#include "testsupport/MediaFixtures.h"

#include <algorithm>
#include <functional>
#include <memory>

using namespace scrubtub;

namespace {

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
    return scannerSuffixes().contains(QFileInfo(fileName).suffix().toLower());
}

// Alphanumeric words of a file name (or any text), lowercased. Used to derive
// queries from the real names; the matcher's own tokenizer is TagEngine.
QStringList wordsOf(const QString& text)
{
    QStringList words;
    QString current;
    auto flush = [&words, &current] {
        if (!current.isEmpty()) {
            words.append(current.toLower());
            current.clear();
        }
    };
    for (const QChar c : text) {
        if (c.isLetterOrNumber())
            current.append(c);
        else
            flush();
    }
    flush();
    words.removeDuplicates();
    return words;
}

int editDistance(const QString& a, const QString& b)
{
    return rapidfuzz::levenshtein_distance(a.toStdU32String(), b.toStdU32String());
}

QString withSubstitutions(const QString& word, const QList<QChar>& replacements)
{
    QString out = word;
    for (int i = 0; i < replacements.size() && i < out.size(); ++i)
        out[i] = replacements.at(i);
    return out;
}

// Resolution bucket boundaries of TagEngine::resolutionBucket: the technical
// tag is derived from the shorter display side with >= thresholds.
QString resolutionBucket(int displayWidth, int displayHeight)
{
    if (displayWidth <= 0 || displayHeight <= 0)
        return {};
    const int shorter = qMin(displayWidth, displayHeight);
    if (shorter >= 2160) return QStringLiteral("2160");
    if (shorter >= 1440) return QStringLiteral("1440");
    if (shorter >= 1080) return QStringLiteral("1080");
    if (shorter >= 720) return QStringLiteral("720");
    if (shorter >= 480) return QStringLiteral("480");
    return {};
}

QSet<qint64> idSet(const QList<qint64>& ids)
{
    return QSet<qint64>(ids.cbegin(), ids.cend());
}

} // namespace

class TestSearch : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Media-free unit/table cases (must keep passing without any corpus).
    void tagEngineTable();

    // Real-library cases: expectations are derived from the scanned rows.
    void searchTable();
    void shortQueriesAndTypos();
    void combinedFiltersAndNulls();
    void stableTiesAndSorts();
    void paginationWithFuzzyMatchBeyondFirstPage();
    void autoTagsFromScan();

    // Controlled (synthetic) trees.
    void diacriticFolding();
    void folderTagsFromControlledTree();
    void manualTagsSuppressionsAndRescans();

private:
    // Active catalogue wiring: the shared corpus library or a per-test own one.
    bool useLibrary();
    bool makeOwnCatalogue(const QString& treeName, const QStringList& relativeFiles,
                          int expectedRows, QString* error);
    void bindCatalogue(Catalogue* catalogue, CatalogueModel* model, const QString& profile);
    QString treePath(const QString& name) const;

    bool waitEnumeration(QSignalSpy& progressSpy, int timeoutMs);
    bool waitComplete(QSignalSpy& progressSpy, int timeoutMs);
    bool waitProbesSettled(int timeoutMs, bool dropPreviewBacklog = false);
    bool stopPreviewWork();
    bool rescanOwnCatalogue(bool force);

    QList<qint64> runSearch(const QuerySpec& spec);
    QList<VideoRow> snapshotRows() const;
    QHash<QString, VideoRow> rowsByName() const;
    qint64 idForName(const QString& name) const;
    VideoRow rowFor(const QString& name) const;

    // Derivation helpers over the corpus listing.
    void buildCorpusWords();
    bool pickUniqueWord(int minLen, int maxLen, bool alphabeticOnly,
                        QString* fileName, QString* word,
                        const QString& excludeWord = QString()) const;
    // Like pickUniqueWord, but also requires that no OTHER corpus file could
    // match the word through a substring or a within-limit typo, so a search
    // for it must return exactly one row.
    bool pickIsolatedWord(QString* fileName, QString* word) const;
    QString pickMostCommonWord(int minLen) const;
    bool anyNameContains(const QString& needle) const;
    bool anyNameTokenWithin(const QString& fileName, const QString& query, int limit) const;

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
    QHash<QString, int> m_wordCounts; // corpus-wide word frequency (lowercased)

    // Shared read-mostly corpus catalogue: scanned once for the whole suite.
    std::unique_ptr<Catalogue> m_library;
    CatalogueModel m_libraryModel;
    QString m_libraryProfile;
    bool m_libraryReady = false;

    // Per-test controlled catalogue.
    std::unique_ptr<Catalogue> m_own;
    CatalogueModel m_ownModel;

    // Active catalogue of the current test.
    Catalogue* m_cat = nullptr;
    CatalogueModel* m_model = nullptr;
    QString m_currentProfile;
    QList<qint64> m_lastResult;
};

void TestSearch::initTestCase()
{
    QVERIFY(m_profileBase.isValid());
    qputenv("XDG_DATA_HOME", m_profileBase.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_profileBase.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_profileBase.filePath(QStringLiteral("config")).toUtf8());

    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!m_ffprobe.isEmpty() && !m_ffmpeg.isEmpty(),
             "ffprobe and ffmpeg must be installed for search tests");

    // Corpus-dependent tests skip individually so the unit table below keeps
    // running on a machine without the media corpus.
    if (testsupport::vidsAvailable()) {
        m_corpusDir = testsupport::vidsDir();
        for (const QString& name : testsupport::vidsFiles())
            if (isScannerVideo(name))
                m_corpusFiles.append(name);
        m_corpusFingerprint = testsupport::vidsFingerprint();
        buildCorpusWords();
    }
}

void TestSearch::cleanupTestCase()
{
    // The corpus was unavailable: there is no read-only state to check.
    if (m_corpusFingerprint.isEmpty())
        return;
    // Read-only guard: the suites must leave the corpus byte-identical.
    QCOMPARE(testsupport::vidsFingerprint(), m_corpusFingerprint);
}

QString TestSearch::treePath(const QString& name) const
{
    return m_profileBase.filePath(QStringLiteral("synthetic/") + name);
}

void TestSearch::bindCatalogue(Catalogue* catalogue, CatalogueModel* model,
                               const QString& profile)
{
    m_cat = catalogue;
    m_model = model;
    m_currentProfile = profile;
    connect(catalogue, &Catalogue::rowsChanged, model, &CatalogueModel::applyRows);
    connect(catalogue, &Catalogue::scanProgress, model, &CatalogueModel::applyProgress);
    connect(catalogue, &Catalogue::rowsPageReady, model, &CatalogueModel::applyPage);
    connect(catalogue, &Catalogue::searchCompleted, this,
            [this](quint64, const QList<qint64>& ids, const QString& error) {
                Q_UNUSED(error);
                m_lastResult = ids;
            },
            Qt::DirectConnection);
    // Detail paging: direct in-thread fetch chain (§8 pages of 200).
    model->setFetchCallback([this](const QList<qint64>& ids, quint64 generation) {
        m_cat->fetchRowsPage(ids, generation);
    });
    model->setPageSize(200);
}

bool TestSearch::useLibrary()
{
    if (!m_libraryReady) {
        m_libraryProfile = m_profileBase.filePath(QStringLiteral("profiles/library"));
        m_library = std::make_unique<Catalogue>();
        bindCatalogue(m_library.get(), &m_libraryModel, m_libraryProfile);
        QString error;
        if (!m_library->initialize(m_libraryProfile, m_ffprobe, m_ffmpeg, &error)) {
            qWarning("library initialize failed: %s", qUtf8Printable(error));
            return false;
        }
        QSignalSpy progressSpy(m_library.get(), &Catalogue::scanProgress);
        m_library->addRoot(m_corpusDir);
        if (!waitEnumeration(progressSpy, 150000)
            || !QTest::qWaitFor([this] {
                   return m_libraryModel.rowCount() == m_corpusFiles.size();
               }, 120000)
            || !waitProbesSettled(150000, true)) {
            qWarning("the corpus library did not finish probing");
            return false;
        }
        // Search only needs probed rows and their tags; stop the preview work.
        if (!stopPreviewWork())
            return false;
        m_libraryReady = true;
    }
    m_own.reset();
    m_cat = m_library.get();
    m_model = &m_libraryModel;
    m_currentProfile = m_libraryProfile;
    m_libraryModel.applyRows(QList<VideoRow>{}, true);
    m_library->refreshRows(); // repopulate the model for this test
    m_libraryModel.setPageSize(200);
    return true;
}

bool TestSearch::makeOwnCatalogue(const QString& treeName,
                                  const QStringList& relativeFiles, int expectedRows,
                                  QString* error)
{
    const QString root = treePath(treeName);
    for (const QString& relative : relativeFiles) {
        const QString path = QDir(root).filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            *error = QStringLiteral("cannot create %1").arg(path);
            return false;
        }
        if (!testsupport::makeSyntheticVideo(root, relative, 1000, error))
            return false;
    }
    const QString profile = m_profileBase.filePath(
        QStringLiteral("profiles/%1-%2")
            .arg(treeName)
            .arg(QDateTime::currentMSecsSinceEpoch()));
    m_own = std::make_unique<Catalogue>();
    bindCatalogue(m_own.get(), &m_ownModel, profile);
    if (!m_own->initialize(profile, m_ffprobe, m_ffmpeg, error))
        return false;
    QSignalSpy progressSpy(m_own.get(), &Catalogue::scanProgress);
    m_own->addRoot(root);
    if (!waitEnumeration(progressSpy, 60000)
        || !QTest::qWaitFor([this, expectedRows] {
               return m_ownModel.rowCount() == expectedRows;
           }, 60000)
        || !waitProbesSettled(60000)) {
        *error = QStringLiteral("synthetic scan did not finish");
        return false;
    }
    // Tiny trees: let the whole scan (tags, posters) settle so later rescans
    // are accepted without cancelling anything.
    if (!waitComplete(progressSpy, 120000)) {
        *error = QStringLiteral("synthetic scan never completed");
        return false;
    }
    return true;
}

bool TestSearch::waitEnumeration(QSignalSpy& progressSpy, int timeoutMs)
{
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

bool TestSearch::waitComplete(QSignalSpy& progressSpy, int timeoutMs)
{
    return QTest::qWaitFor([&progressSpy] {
        for (int i = 0; i < progressSpy.size(); ++i)
            if (progressSpy.at(i).at(0).value<ScanProgress>().state
                == QLatin1String("complete"))
                return true;
        return false;
    }, timeoutMs);
}

bool TestSearch::waitProbesSettled(int timeoutMs, bool dropPreviewBacklog)
{
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
            QTest::qWait(50);
            return true;
        }
        QTest::qWait(100);
    }
    qWarning("probes did not settle: pending=%lld probe jobs=%lld",
             static_cast<long long>(queryInt(
                 QStringLiteral("SELECT COUNT(*) FROM videos WHERE probe_status='pending'"))),
             static_cast<long long>(countJobs(
                 QStringLiteral("kind='probe' AND state IN ('queued','running')"))));
    return false;
}

bool TestSearch::stopPreviewWork()
{
    // Previews are not asserted here; cancelling and dropping the backlog
    // leaves the profile quiet (a cancelled job is requeued by the catalogue).
    m_cat->cancelScanning();
    for (int attempt = 0; attempt < 100; ++attempt) {
        dropQueuedPreviewJobs();
        if (countJobs(QStringLiteral("state='running'")) == 0
            && countJobs(QStringLiteral("kind IN ('poster','storyboard')")) == 0)
            return true;
        QTest::qWait(100);
    }
    return false;
}

bool TestSearch::rescanOwnCatalogue(bool force)
{
    QSignalSpy progressSpy(m_cat, &Catalogue::scanProgress);
    QSignalSpy failures(m_cat, &Catalogue::operationFailed);
    m_cat->rescanRoot(1, force);
    if (!QTest::qWaitFor([&progressSpy] { return progressSpy.size() > 0; }, 1000)) {
        qWarning("rescan was refused: %s",
                 failures.isEmpty() ? "no reason"
                                    : qUtf8Printable(failures.last().first().toString()));
        return false;
    }
    return waitComplete(progressSpy, 120000);
}

QList<qint64> TestSearch::runSearch(const QuerySpec& spec)
{
    m_lastResult.clear();
    m_cat->search(spec);
    return m_lastResult;
}

QList<VideoRow> TestSearch::snapshotRows() const
{
    QList<VideoRow> rows;
    rows.reserve(m_model->rowCount());
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const QModelIndex idx = m_model->index(i, 0);
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
        row.availability = idx.data(CatalogueModel::AvailabilityRole).toString();
        row.probeStatus = idx.data(CatalogueModel::ProbeStatusRole).toString();
        rows.append(row);
    }
    return rows;
}

QHash<QString, VideoRow> TestSearch::rowsByName() const
{
    QHash<QString, VideoRow> rows;
    for (const VideoRow& row : snapshotRows())
        rows.insert(row.fileName, row);
    return rows;
}

qint64 TestSearch::idForName(const QString& name) const
{
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const QModelIndex idx = m_model->index(i, 0);
        if (idx.data(CatalogueModel::NameRole).toString() == name)
            return idx.data(CatalogueModel::IdRole).toLongLong();
    }
    return -1;
}

VideoRow TestSearch::rowFor(const QString& name) const
{
    return rowsByName().value(name);
}

void TestSearch::buildCorpusWords()
{
    m_wordCounts.clear();
    for (const QString& name : m_corpusFiles)
        for (const QString& word : wordsOf(QFileInfo(name).completeBaseName()))
            m_wordCounts[word] += 1;
}

bool TestSearch::pickUniqueWord(int minLen, int maxLen, bool alphabeticOnly,
                                QString* fileName, QString* word,
                                const QString& excludeWord) const
{
    for (const QString& name : m_corpusFiles) {
        const QStringList words = wordsOf(QFileInfo(name).completeBaseName());
        for (const QString& candidate : words) {
            if (candidate == excludeWord)
                continue;
            if (candidate.size() < minLen || candidate.size() > maxLen)
                continue;
            if (alphabeticOnly && !candidate.at(0).isLetter())
                continue;
            if (m_wordCounts.value(candidate) != 1)
                continue;
            *fileName = name;
            *word = candidate;
            return true;
        }
    }
    return false;
}

QString TestSearch::pickMostCommonWord(int minLen) const
{
    QString best;
    int bestCount = 0;
    for (auto it = m_wordCounts.cbegin(); it != m_wordCounts.cend(); ++it) {
        if (it.key().size() < minLen)
            continue;
        if (it.value() > bestCount
            || (it.value() == bestCount && it.key() < best)) {
            best = it.key();
            bestCount = it.value();
        }
    }
    return best;
}

bool TestSearch::pickIsolatedWord(QString* fileName, QString* word) const
{
    for (const QString& name : m_corpusFiles) {
        for (const QString& candidate : wordsOf(QFileInfo(name).completeBaseName())) {
            if (candidate.size() < 6 || !candidate.at(0).isLetter())
                continue;
            if (m_wordCounts.value(candidate) != 1)
                continue;
            const QString folded = TagEngine::diacriticFold(TagEngine::normalize(candidate));
            const int limit = TagEngine::editDistanceLimit(folded.size());
            bool isolated = true;
            for (const QString& other : m_corpusFiles) {
                if (other == name)
                    continue;
                if (TagEngine::diacriticFold(other).contains(folded)) {
                    isolated = false;
                    break;
                }
                const QStringList tokens =
                    TagEngine::tokenize(QFileInfo(other).completeBaseName());
                for (const QString& token : tokens) {
                    const QString norm =
                        TagEngine::diacriticFold(TagEngine::normalize(token));
                    if (norm.contains(folded)
                        || (limit > 0 && qAbs(norm.size() - folded.size()) <= limit
                            && editDistance(norm, folded) <= limit)) {
                        isolated = false;
                        break;
                    }
                }
                if (!isolated)
                    break;
            }
            if (!isolated)
                continue;
            *fileName = name;
            *word = candidate;
            return true;
        }
    }
    return false;
}

bool TestSearch::anyNameContains(const QString& needle) const
{
    const QString folded = TagEngine::diacriticFold(TagEngine::normalize(needle));
    for (const QString& name : m_corpusFiles)
        if (TagEngine::diacriticFold(name).contains(folded))
            return true;
    return false;
}

bool TestSearch::anyNameTokenWithin(const QString& fileName, const QString& query, int limit) const
{
    // Exact predicate of the matcher for a single-token query against one
    // file's own name tokens (technical tags are constant and never close to
    // the alphabetic typos used below).
    const QString folded = TagEngine::diacriticFold(TagEngine::normalize(query));
    const QStringList tokens = TagEngine::tokenize(QFileInfo(fileName).completeBaseName());
    for (const QString& token : tokens) {
        const QString norm = TagEngine::diacriticFold(TagEngine::normalize(token));
        if (norm == folded || norm.startsWith(folded) || norm.contains(folded))
            return true;
        if (limit > 0 && qAbs(norm.size() - folded.size()) <= limit
            && editDistance(norm, folded) <= limit)
            return true;
    }
    return false;
}

qint64 TestSearch::queryInt(const QString& sql) const
{
    Database db;
    QString error;
    if (!db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error))
        return -1;
    Statement st = db.prepare(sql.toUtf8().constData());
    if (!st.isValid() || !st.step())
        return -1;
    return st.int64(0);
}

qint64 TestSearch::countJobs(const QString& where) const
{
    return queryInt(QStringLiteral("SELECT COUNT(*) FROM jobs WHERE ") + where);
}

bool TestSearch::execSql(const QString& sql) const
{
    Database db;
    QString error;
    if (!db.open(m_currentProfile + QStringLiteral("/catalogue.db"), &error))
        return false;
    return db.exec(sql.toUtf8().constData());
}

bool TestSearch::dropQueuedPreviewJobs() const
{
    return execSql(QStringLiteral(
        "DELETE FROM jobs WHERE kind IN ('poster','storyboard') AND state='queued'"));
}

// ---------------------------------------------------------------------------
// Media-free unit/table cases (TECH_SPEC.md §7 table, no parent folders).

void TestSearch::tagEngineTable()
{
    const QStringList summerTrip = (QStringList() << QStringLiteral("summer") << QStringLiteral("trip"));
    const QStringList actualSummer = TagEngine::filenameTags(QStringLiteral("Summer_Trip-2024_1080p"));
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

// ---------------------------------------------------------------------------
// Real-library cases.

void TestSearch::searchTable()
{
    if (!testsupport::vidsAvailable())
        QSKIP("test corpus unavailable: set SCRUBTUB_TEST_VIDS_DIR to the ScrubTub vids/ folder");
    QVERIFY(useLibrary());
    const QList<VideoRow> rows = snapshotRows();
    QCOMPARE(rows.size(), m_corpusFiles.size());

    // A real name with a word no other corpus file can match — through
    // substrings or through typo tolerance — so the result is exactly one row.
    QString name;
    QString word;
    QVERIFY2(pickIsolatedWord(&name, &word),
             "no isolated >=6-character word in the corpus names");
    const qint64 id = idForName(name);
    QVERIFY(id > 0);

    // Exact token: exactly that file, no other row shares the word.
    QuerySpec exact;
    exact.text = word;
    QCOMPARE(runSearch(exact), QList<qint64>{id});

    // Case variants fold together (§7/§8 normalization).
    QuerySpec upper;
    upper.text = word.toUpper();
    QCOMPARE(runSearch(upper), QList<qint64>{id});
    QuerySpec mixed;
    mixed.text = word.at(0).toUpper() + word.mid(1);
    QCOMPARE(runSearch(mixed), QList<qint64>{id});

    // A prefix of the token still reaches the file (match class 1).
    QuerySpec prefix;
    prefix.text = word.left(5);
    QVERIFY(runSearch(prefix).contains(id));

    // Every query token must match (AND), so a two-token query returns the
    // intersection of the single-token results, never their union.
    QString otherName;
    QString otherWord;
    QVERIFY2(pickUniqueWord(6, 64, true, &otherName, &otherWord, word),
             "no second unique word in the corpus names");
    QuerySpec first;
    first.text = word;
    QuerySpec second;
    second.text = otherWord;
    QuerySpec both;
    both.text = word + QLatin1Char(' ') + otherWord;
    Q_UNUSED(otherName);
    const QSet<qint64> firstHits = idSet(runSearch(first));
    const QSet<qint64> secondHits = idSet(runSearch(second));
    const QSet<qint64> bothHits = idSet(runSearch(both));
    QVERIFY(!firstHits.isEmpty());
    QVERIFY(!secondHits.isEmpty());
    QCOMPARE(bothHits, firstHits & secondHits);
    // No file that matches only one of the two tokens survives (AND, not OR).
    if ((firstHits & secondHits).isEmpty())
        QVERIFY(bothHits.isEmpty());
}

void TestSearch::shortQueriesAndTypos()
{
    if (!testsupport::vidsAvailable())
        QSKIP("test corpus unavailable: set SCRUBTUB_TEST_VIDS_DIR to the ScrubTub vids/ folder");
    QVERIFY(useLibrary());

    // A real single character that occurs in names: substring matching only,
    // and the file that contains it is reached.
    QString name;
    QString word;
    QVERIFY(pickUniqueWord(6, 64, true, &name, &word));
    const qint64 id = idForName(name);
    QuerySpec oneChar;
    oneChar.text = word.left(1);
    QVERIFY(runSearch(oneChar).contains(id));

    // 1–2 character queries use exact prefix/substring only (§8): a corrupted
    // two-character word cannot reach its file.
    QString shortName;
    QString shortWord;
    QVERIFY2(pickUniqueWord(2, 2, false, &shortName, &shortWord),
             "no unique two-character word in the corpus names");
    const qint64 shortId = idForName(shortName);
    QVERIFY(shortId > 0);
    QuerySpec exactShort;
    exactShort.text = shortWord;
    QVERIFY(runSearch(exactShort).contains(shortId));
    const QString corruptedShort = withSubstitutions(shortWord, {shortWord.at(0), QLatin1Char('q')});
    QVERIFY2(!anyNameContains(corruptedShort) && !anyNameTokenWithin(shortName, corruptedShort, 0),
             "derived short-query typo is not a safe counter-example");
    QuerySpec shortTypo;
    shortTypo.text = corruptedShort;
    QVERIFY2(!runSearch(shortTypo).contains(shortId),
             "a two-character query must not typo-match");

    // A length-5 word allows distance 1: one substitution still finds it.
    QString mediumName;
    QString mediumWord;
    QVERIFY2(pickUniqueWord(5, 5, true, &mediumName, &mediumWord),
             "no unique five-character word in the corpus names");
    const qint64 mediumId = idForName(mediumName);
    const QString oneEdit = withSubstitutions(mediumWord, {mediumWord.at(0), QLatin1Char('z')});
    QCOMPARE(editDistance(oneEdit, mediumWord), 1);
    if (!anyNameContains(oneEdit)) {
        QuerySpec typo1;
        typo1.text = oneEdit;
        QVERIFY2(runSearch(typo1).contains(mediumId),
                 qPrintable(QStringLiteral("%1 should match within distance 1").arg(oneEdit)));
    }
    // Two substitutions are beyond the length-5 limit of 1.
    const QString twoEdits =
        withSubstitutions(mediumWord, {QLatin1Char('q'), mediumWord.at(1), QLatin1Char('z')});
    QCOMPARE(editDistance(twoEdits, mediumWord), 2);
    if (!anyNameContains(twoEdits)
        && !anyNameTokenWithin(mediumName, twoEdits, 1)) {
        QuerySpec typo2;
        typo2.text = twoEdits;
        QVERIFY2(!runSearch(typo2).contains(mediumId),
                 qPrintable(QStringLiteral("%1 must not match at limit 1").arg(twoEdits)));
    }

    // A length >= 6 word allows distance 2.
    QString longName;
    QString longWord;
    QVERIFY2(pickUniqueWord(8, 64, true, &longName, &longWord),
             "no unique >=8-character word in the corpus names");
    const qint64 longId = idForName(longName);
    const QString longTwoEdits = withSubstitutions(
        longWord, {QLatin1Char('q'), longWord.at(1), QLatin1Char('z')});
    QCOMPARE(editDistance(longTwoEdits, longWord), 2);
    if (!anyNameContains(longTwoEdits)) {
        QuerySpec typoLong;
        typoLong.text = longTwoEdits;
        QVERIFY2(runSearch(typoLong).contains(longId),
                 qPrintable(QStringLiteral("%1 should match within distance 2").arg(longTwoEdits)));
    }

    // A deliberately unmatched short pair returns nothing.
    QuerySpec nonsense;
    nonsense.text = QStringLiteral("qz");
    if (!anyNameContains(QStringLiteral("qz")))
        QVERIFY(runSearch(nonsense).isEmpty());
}

void TestSearch::combinedFiltersAndNulls()
{
    if (!testsupport::vidsAvailable())
        QSKIP("test corpus unavailable: set SCRUBTUB_TEST_VIDS_DIR to the ScrubTub vids/ folder");
    QVERIFY(useLibrary());
    const QList<VideoRow> rows = snapshotRows();
    const int total = rows.size();
    QVERIFY(total > 1);

    // Rating filters: everything starts unrated; rating one real row changes
    // it and every count below is derived from that.
    QuerySpec unrated;
    unrated.ratingMode = 1;
    QCOMPARE(runSearch(unrated).size(), total);
    const VideoRow target = rows.first();
    m_cat->setRating(target.id, 4);
    QCOMPARE(runSearch(unrated).size(), total - 1);
    QuerySpec exact;
    exact.ratingMode = 2;
    exact.ratingValue = 4;
    QCOMPARE(runSearch(exact), QList<qint64>{target.id});
    QuerySpec rated;
    rated.ratingMode = 3;
    QCOMPARE(runSearch(rated), QList<qint64>{target.id});
    QuerySpec ratingRange;
    ratingRange.ratingMin = 3;
    ratingRange.ratingMax = 4;
    QCOMPARE(runSearch(ratingRange), QList<qint64>{target.id});
    ratingRange.ratingMax = 3;
    QVERIFY(runSearch(ratingRange).isEmpty());
    ratingRange.ratingMin = 0;
    QCOMPARE(runSearch(ratingRange).size(), total - 1); // includes unrated, excludes four stars
    ratingRange.ratingMax = 0;
    QCOMPARE(runSearch(ratingRange).size(), total - 1);

    // Duration bounds are inclusive; the expected set comes from the rows.
    const VideoRow durationTarget = [&rows] {
        for (const VideoRow& row : rows)
            if (row.durationMs > 0)
                return row;
        return VideoRow{};
    }();
    QVERIFY(durationTarget.id > 0);
    QSet<qint64> expectedDurations;
    for (const VideoRow& row : rows)
        if (row.durationMs == durationTarget.durationMs)
            expectedDurations.insert(row.id);
    QuerySpec duration;
    duration.durationMinMs = durationTarget.durationMs;
    duration.durationMaxMs = durationTarget.durationMs;
    QCOMPARE(idSet(runSearch(duration)), expectedDurations);

    // Size bounds behave the same way.
    QSet<qint64> expectedSizes;
    for (const VideoRow& row : rows)
        if (row.sizeBytes == target.sizeBytes)
            expectedSizes.insert(row.id);
    QuerySpec size;
    size.sizeMin = target.sizeBytes;
    size.sizeMax = target.sizeBytes;
    QCOMPARE(idSet(runSearch(size)), expectedSizes);

    // Resolution presets bucket the shorter side: expected counts are computed
    // from the rows with the same [preset, preset + preset/3) rule.
    bool sawPreset = false;
    for (const int preset : {480, 720, 1080, 1440, 2160}) {
        int expected = 0;
        for (const VideoRow& row : rows) {
            if (row.displayWidth <= 0 || row.displayHeight <= 0)
                continue;
            const int shorter = qMin(row.displayWidth, row.displayHeight);
            if (shorter >= preset && shorter < preset + preset / 3)
                ++expected;
        }
        sawPreset = sawPreset || expected > 0;
        QuerySpec presetSpec;
        presetSpec.resolutionPreset = QString::number(preset);
        QCOMPARE(runSearch(presetSpec).size(), expected);
    }
    QVERIFY(sawPreset);

    // Availability filter.
    int expectedAvailable = 0;
    for (const VideoRow& row : rows)
        if (row.availability == QLatin1String("available"))
            ++expectedAvailable;
    QuerySpec available;
    available.availability = QStringList{QStringLiteral("available")};
    QCOMPARE(runSearch(available).size(), expectedAvailable);

    // Combined categories AND: unrated rows of the target's exact duration.
    QuerySpec combined;
    combined.ratingMode = 1;
    combined.durationMinMs = durationTarget.durationMs;
    combined.durationMaxMs = durationTarget.durationMs;
    combined.availability = QStringList{QStringLiteral("available")};
    QSet<qint64> expectedCombined = expectedDurations;
    expectedCombined.remove(target.id);
    QCOMPARE(idSet(runSearch(combined)), expectedCombined);

    // QML's proxy combines ranges with normalized tag filters, and its
    // slider bounds come from the collection, not from the filtered hits.
    m_cat->addManualTag(target.id, QStringLiteral("Archive"));
    CatalogueProxy proxy(m_cat, m_model);
    QSignalSpy proxyResult(&proxy, &CatalogueProxy::searchCompleted);
    QSignalSpy bounds(&proxy, &CatalogueProxy::filterBoundsReady);
    proxy.search({{QStringLiteral("ratingMin"), 3}, {QStringLiteral("ratingMax"), 4},
                  {QStringLiteral("includeAllTags"), QStringList{QStringLiteral(" ARCHIVE ")}}});
    QTRY_COMPARE(proxyResult.size(), 1);
    QCOMPARE(proxyResult.at(0).at(1).value<QList<qint64>>(), QList<qint64>{target.id});
    QCOMPARE(bounds.size(), 1);
    qint64 maxDuration = 0;
    qint64 maxSize = 0;
    for (const VideoRow& row : rows) {
        maxDuration = qMax(maxDuration, row.durationMs);
        maxSize = qMax(maxSize, row.sizeBytes);
    }
    QCOMPARE(bounds.at(0).at(1).toLongLong(), maxDuration);
    QCOMPARE(bounds.at(0).at(2).toLongLong(), maxSize);
    proxy.search({{QStringLiteral("excludeTags"), QStringList{QStringLiteral("ARCHIVE")}}});
    QTRY_COMPARE(proxyResult.size(), 2);
    QCOMPARE(proxyResult.at(1).at(1).value<QList<qint64>>().size(), total - 1);
    QCOMPARE(bounds.at(1), bounds.at(0)); // filtering does not shrink the slider scale

    // Restore the library row so later cases still see an unrated collection.
    m_cat->setRating(target.id, 0);
    QCOMPARE(runSearch(unrated).size(), total);
}

void TestSearch::stableTiesAndSorts()
{
    if (!testsupport::vidsAvailable())
        QSKIP("test corpus unavailable: set SCRUBTUB_TEST_VIDS_DIR to the ScrubTub vids/ folder");
    QVERIFY(useLibrary());
    const QList<VideoRow> rows = snapshotRows();
    QCOMPARE(rows.size(), m_corpusFiles.size());

    // Expected orders replicate the documented SQL: unknowns last, key
    // ascending, video ID as the stable tie-breaker (§1).
    const auto orderedIds = [](QList<VideoRow> sorted) {
        QList<qint64> ids;
        ids.reserve(sorted.size());
        for (const VideoRow& row : sorted)
            ids.append(row.id);
        return ids;
    };
    const auto byKeyThenId = [](QList<VideoRow> list,
                                const std::function<qint64(const VideoRow&)>& key) {
        std::sort(list.begin(), list.end(),
                  [&key](const VideoRow& a, const VideoRow& b) {
                      const qint64 ka = key(a);
                      const qint64 kb = key(b);
                      if ((ka < 0) != (kb < 0))
                          return kb < 0; // unknown values sort last
                      if (ka != kb)
                          return ka < kb;
                      return a.id < b.id;
                  });
        return list;
    };

    QuerySpec bySize;
    bySize.sortKey = QStringLiteral("size");
    bySize.userSort = true;
    QCOMPARE(runSearch(bySize),
             orderedIds(byKeyThenId(rows, [](const VideoRow& r) { return r.sizeBytes; })));

    QuerySpec byDuration;
    byDuration.sortKey = QStringLiteral("duration");
    byDuration.userSort = true;
    QCOMPARE(runSearch(byDuration),
             orderedIds(byKeyThenId(rows, [](const VideoRow& r) { return r.durationMs; })));

    // Name sort uses SQLite's BINARY collation: compare UTF-8 bytes, the same
    // order the database applies.
    QList<VideoRow> byNameExpected = rows;
    std::sort(byNameExpected.begin(), byNameExpected.end(),
              [](const VideoRow& a, const VideoRow& b) {
                  const int c = a.fileName.toUtf8().compare(b.fileName.toUtf8());
                  if (c != 0)
                      return c < 0;
                  return a.id < b.id;
              });
    QuerySpec byName;
    byName.sortKey = QStringLiteral("name");
    byName.userSort = true;
    QCOMPARE(runSearch(byName), orderedIds(byNameExpected));
}

void TestSearch::paginationWithFuzzyMatchBeyondFirstPage()
{
    if (!testsupport::vidsAvailable())
        QSKIP("test corpus unavailable: set SCRUBTUB_TEST_VIDS_DIR to the ScrubTub vids/ folder");
    QVERIFY(useLibrary());

    // A real, frequent token large enough for typo tolerance (limit 2 at
    // length >= 6), then a distance-1 typo of it as the live query.
    const QString common = pickMostCommonWord(6);
    QVERIFY(!common.isEmpty());
    QVERIFY(m_wordCounts.value(common) > 4);
    const QString typo = withSubstitutions(common, {common.at(0), QLatin1Char('x')});
    QCOMPARE(editDistance(typo, common), 1);
    QuerySpec spec;
    spec.text = typo;
    const QList<qint64> hits = runSearch(spec);
    QVERIFY2(hits.size() > 4, "the fuzzy query needs more than one detail page");

    m_model->setPageSize(2);
    m_model->applyRows(QList<VideoRow>{}, true); // start empty to force paging
    m_model->setOrder(hits, true);
    // Details are fetched in pages of 2; IDs beyond the first page must load.
    QVERIFY2(QTest::qWaitFor([this, hits] {
                 for (const qint64 id : hits) {
                     bool loaded = false;
                     for (int i = 0; i < m_model->rowCount(); ++i)
                         if (m_model->index(i, 0).data(CatalogueModel::IdRole).toLongLong() == id)
                             loaded = true;
                     if (!loaded)
                         return false;
                 }
                 return true;
             }, 60000),
             "result details beyond the first detail page were never fetched");
    QCOMPARE(m_model->rowCount(), hits.size());
    m_model->setPageSize(200);
}

void TestSearch::autoTagsFromScan()
{
    if (!testsupport::vidsAvailable())
        QSKIP("test corpus unavailable: set SCRUBTUB_TEST_VIDS_DIR to the ScrubTub vids/ folder");
    QVERIFY(useLibrary());
    const QList<VideoRow> rows = snapshotRows();

    // Technical tags are derived from the actual probe results: codec per row
    // and resolution from the shorter display side.
    QHash<QString, int> codecCounts;
    QHash<QString, int> bucketCounts;
    for (const VideoRow& row : rows) {
        if (!row.codec.isEmpty() && row.codec != QLatin1String("unknown"))
            codecCounts[row.codec] += 1;
        const QString bucket = resolutionBucket(row.displayWidth, row.displayHeight);
        if (!bucket.isEmpty())
            bucketCounts[bucket] += 1;
    }
    QVERIFY(!codecCounts.isEmpty());
    QVERIFY(!bucketCounts.isEmpty());
    for (auto it = codecCounts.cbegin(); it != codecCounts.cend(); ++it) {
        QuerySpec spec;
        spec.includeAnyTags = QStringList{QStringLiteral("codec:") + it.key()};
        QCOMPARE(runSearch(spec).size(), it.value());
    }
    for (auto it = bucketCounts.cbegin(); it != bucketCounts.cend(); ++it) {
        QuerySpec spec;
        spec.includeAnyTags = QStringList{QStringLiteral("resolution:") + it.key()};
        QCOMPARE(runSearch(spec).size(), it.value());
    }

    // The tag list of one row carries exactly the technical tags its probe
    // data implies, with distinct provenance (§7).
    const VideoRow sample = rows.first();
    QVERIFY(!sample.codec.isEmpty());
    QSignalSpy tagsSpy(m_cat, &Catalogue::tagsReady);
    m_cat->requestTags(sample.id);
    QTRY_VERIFY(!tagsSpy.isEmpty());
    QStringList technical;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        if (tagsSpy.at(i).at(0).toLongLong() != sample.id)
            continue;
        for (const QVariant& entry : tagsSpy.at(i).at(1).toList()) {
            const QVariantMap tag = entry.toMap();
            const QString origin = tag.value(QStringLiteral("origin")).toString();
            if (origin == QLatin1String("technical") || origin == QLatin1String("filename")
                || origin == QLatin1String("folder"))
                technical.append(tag.value(QStringLiteral("norm")).toString());
        }
    }
    QVERIFY(technical.contains(QStringLiteral("codec:") + sample.codec));
    const QString bucket = resolutionBucket(sample.displayWidth, sample.displayHeight);
    if (!bucket.isEmpty())
        QVERIFY(technical.contains(QStringLiteral("resolution:") + bucket));
}

// ---------------------------------------------------------------------------
// Controlled (synthetic) trees.

void TestSearch::diacriticFolding()
{
    QString error;
    const QStringList files{QStringLiteral("Café Über 東京 01.mp4"),
                            QStringLiteral("plain.mp4")};
    QVERIFY2(makeOwnCatalogue(QStringLiteral("folding"), files, 2, &error),
             qUtf8Printable(error));

    const qint64 accented = idForName(QStringLiteral("Café Über 東京 01.mp4"));
    QVERIFY(accented > 0);
    // Identity and display keep the original spelling.
    QCOMPARE(rowFor(QStringLiteral("Café Über 東京 01.mp4")).fileName,
             QStringLiteral("Café Über 東京 01.mp4"));

    // The diacritic-folded copy matches the plain spelling (§8).
    QuerySpec folded;
    folded.text = QStringLiteral("cafe");
    QCOMPARE(runSearch(folded), QList<qint64>{accented});
    QuerySpec foldedTwo;
    foldedTwo.text = QStringLiteral("uber");
    QCOMPARE(runSearch(foldedTwo), QList<qint64>{accented});
    QuerySpec foldedBoth;
    foldedBoth.text = QStringLiteral("cafe uber");
    QCOMPARE(runSearch(foldedBoth), QList<qint64>{accented});
    // And the accented spelling itself still matches.
    QuerySpec accentedQuery;
    accentedQuery.text = QStringLiteral("café über");
    QCOMPARE(runSearch(accentedQuery), QList<qint64>{accented});
    // The other row is not dragged in.
    QVERIFY(!runSearch(folded).contains(idForName(QStringLiteral("plain.mp4"))));
}

void TestSearch::folderTagsFromControlledTree()
{
    QString error;
    // A controlled tree: the root name never becomes a tag, only the folders
    // below it do (§7).
    QVERIFY2(makeOwnCatalogue(
                 QStringLiteral("foldertags"),
                 {QStringLiteral("Travel/Japan/Summer_Trip-2024_1080p.mp4"),
                  QStringLiteral("plain.mp4")},
                 2, &error),
             qUtf8Printable(error));
    const qint64 nested = idForName(QStringLiteral("Summer_Trip-2024_1080p.mp4"));
    const qint64 plain = idForName(QStringLiteral("plain.mp4"));
    QVERIFY(nested > 0 && plain > 0);
    QCOMPARE(rowFor(QStringLiteral("Summer_Trip-2024_1080p.mp4")).relPath,
             QStringLiteral("Travel/Japan/Summer_Trip-2024_1080p.mp4"));

    // Folder tags come from the path segments below the root.
    QuerySpec japan;
    japan.includeAnyTags = QStringList{QStringLiteral("japan")};
    QCOMPARE(runSearch(japan), QList<qint64>{nested});
    QuerySpec travel;
    travel.includeAnyTags = QStringList{QStringLiteral("travel")};
    QCOMPARE(runSearch(travel), QList<qint64>{nested});
    // Filename tags of a mixed-name stem: years and container tokens are not
    // tags (§7).
    QuerySpec summer;
    summer.includeAnyTags = QStringList{QStringLiteral("summer")};
    QCOMPARE(runSearch(summer), QList<qint64>{nested});
    QuerySpec trip;
    trip.includeAnyTags = QStringList{QStringLiteral("trip")};
    QCOMPARE(runSearch(trip), QList<qint64>{nested});
    QuerySpec year;
    year.includeAnyTags = QStringList{QStringLiteral("2024")};
    QVERIFY(runSearch(year).isEmpty());

    // folderPrefix filters by the root-relative path prefix.
    QuerySpec deepFolder;
    deepFolder.folderPrefix = QStringLiteral("Travel/Japan/");
    QCOMPARE(runSearch(deepFolder), QList<qint64>{nested});
    QuerySpec shallowFolder;
    shallowFolder.folderPrefix = QStringLiteral("Travel/");
    QCOMPARE(runSearch(shallowFolder), QList<qint64>{nested});
    // The root's own name never becomes a folder tag (§7).
    QuerySpec rootTag;
    rootTag.includeAnyTags = QStringList{QStringLiteral("foldertags")};
    QVERIFY(runSearch(rootTag).isEmpty());
}

void TestSearch::manualTagsSuppressionsAndRescans()
{
    QString error;
    QVERIFY2(makeOwnCatalogue(QStringLiteral("tagops"),
                              {QStringLiteral("Summer_Trip.mp4"), QStringLiteral("plain.mp4")},
                              2, &error),
             qUtf8Printable(error));
    const qint64 id = idForName(QStringLiteral("Summer_Trip.mp4"));
    QVERIFY(id > 0);

    // Manual tag visible and searchable.
    m_cat->addManualTag(id, QStringLiteral("Favourites"));
    QuerySpec manual;
    manual.includeAnyTags = QStringList{QStringLiteral("favourites")};
    QCOMPARE(runSearch(manual), QList<qint64>{id});

    // Find the automatic filename tag of this row.
    QSignalSpy tagsSpy(m_cat, &Catalogue::tagsReady);
    m_cat->requestTags(id);
    QTRY_VERIFY(!tagsSpy.isEmpty());
    qint64 autoTagId = -1;
    QString autoLabel;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        if (tagsSpy.at(i).at(0).toLongLong() != id)
            continue;
        for (const QVariant& entry : tagsSpy.at(i).at(1).toList()) {
            const QVariantMap tag = entry.toMap();
            if (tag.value(QStringLiteral("origin")).toString() != QLatin1String("manual")) {
                autoTagId = tag.value(QStringLiteral("tagId")).toLongLong();
                autoLabel = tag.value(QStringLiteral("label")).toString();
            }
        }
    }
    QVERIFY(autoTagId > 0);
    QVERIFY(!autoLabel.isEmpty());

    // Suppressed automatic tags survive a rescan; the rescan is forced so the
    // automatic tags are regenerated from the probe results.
    m_cat->suppressAutoTag(id, autoTagId);
    QVERIFY2(rescanOwnCatalogue(true), "forced rescan failed");
    tagsSpy.clear();
    m_cat->requestTags(id);
    QTRY_VERIFY(!tagsSpy.isEmpty());
    bool suppressedVisible = false;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        if (tagsSpy.at(i).at(0).toLongLong() != id)
            continue;
        for (const QVariant& entry : tagsSpy.at(i).at(1).toList()) {
            const QVariantMap tag = entry.toMap();
            if (tag.value(QStringLiteral("tagId")).toLongLong() == autoTagId)
                suppressedVisible = !tag.value(QStringLiteral("suppressed")).toBool();
        }
    }
    QVERIFY2(!suppressedVisible, "suppressed tag reappeared after rescan");

    // Manual assignment takes precedence over a suppression (§7).
    m_cat->suppressAutoTag(id, autoTagId);
    m_cat->addManualTag(id, autoLabel);
    tagsSpy.clear();
    m_cat->requestTags(id);
    QTRY_VERIFY(!tagsSpy.isEmpty());
    bool visibleAfterManual = false;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        if (tagsSpy.at(i).at(0).toLongLong() != id)
            continue;
        for (const QVariant& entry : tagsSpy.at(i).at(1).toList()) {
            const QVariantMap tag = entry.toMap();
            if (tag.value(QStringLiteral("tagId")).toLongLong() == autoTagId
                && !tag.value(QStringLiteral("suppressed")).toBool())
                visibleAfterManual = true;
        }
    }
    QVERIFY(visibleAfterManual);

    // Reset suppressions restores suppressed automatic tags.
    m_cat->resetSuppressions();
    tagsSpy.clear();
    m_cat->requestTags(id);
    QTRY_VERIFY(!tagsSpy.isEmpty());
    bool visibleAfterReset = false;
    for (int i = 0; i < tagsSpy.size(); ++i) {
        if (tagsSpy.at(i).at(0).toLongLong() != id)
            continue;
        for (const QVariant& entry : tagsSpy.at(i).at(1).toList()) {
            const QVariantMap tag = entry.toMap();
            if (tag.value(QStringLiteral("tagId")).toLongLong() == autoTagId
                && !tag.value(QStringLiteral("suppressed")).toBool())
                visibleAfterReset = true;
        }
    }
    QVERIFY(visibleAfterReset);
}

QTEST_GUILESS_MAIN(TestSearch)
#include "tst_search.moc"
