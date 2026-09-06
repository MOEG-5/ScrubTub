// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "Catalogue.h"

#include "CatalogueM3.cpp"
#include "Database.h"
#include "SourceScanner.h"
#include "TagEngine.h"
#include "media/Extract.h"
#include "media/Probe.h"

#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <sqlite3.h>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

namespace itub {

namespace {

// Availability semantics (recorded for the UI and filters):
//   unprobed    — file seen on disk, metadata not extracted yet
//   available   — probed successfully
//   missing     — absent at the end of a complete enumeration
//   unavailable — present but probe failed (unreadable/corrupt/unsupported)

QString fileNameOf(const QString& path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

// Cache profile identifiers: thumbnail quality changes create new versioned
// keys and retire old entries (§6).
const char* kPosterProfile = "poster-320-v1";
const char* kStoryboardProfile = "sb-320-v1";

} // namespace

Catalogue::Catalogue(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<VideoRow>();
    qRegisterMetaType<QList<VideoRow>>();
    qRegisterMetaType<RootInfo>();
    qRegisterMetaType<ScanProgress>();
}

Catalogue::~Catalogue()
{
    if (m_pool) {
        m_cancelRequested.store(true);
        killActiveJobs();
        m_pool->waitForDone();
        m_pool->clear();
    }
}

bool Catalogue::initialize(const QString& profileDataDir, const QString& ffprobePath,
                           const QString& ffmpegPath, QString* error)
{
    m_profileDataDir = profileDataDir;
    m_ffprobePath = ffprobePath;
    m_ffmpegPath = ffmpegPath;
    QDir().mkpath(profileDataDir);
    m_cacheDir = profileDataDir + QStringLiteral("/thumbs");
    QDir().mkpath(m_cacheDir);

    // One application instance owns a profile via a lock (§4).
    m_profileLock = std::make_unique<QLockFile>(profileDataDir + QStringLiteral("/catalogue.lock"));
    m_profileLock->setStaleLockTime(0);
    if (!m_profileLock->tryLock(0)) {
        if (error)
            *error = QStringLiteral("Profile is already in use by another instance");
        return false;
    }

    m_db = std::make_unique<Database>();
    if (!m_db->open(profileDataDir + QStringLiteral("/catalogue.db"), error))
        return false;
    if (!m_db->migrate(error))
        return false;

    // Interrupted jobs return to queued on startup (§5).
    m_db->exec("UPDATE jobs SET state='queued' WHERE state='running'");
    if (error)
        *error = QString();
    return true;
}

void Catalogue::loadExistingState()
{
    {
        Statement st = m_db->prepare(
            "SELECT id, path, status, include_hidden FROM roots ORDER BY id");
        while (st.step()) {
            RootInfo root;
            root.id = st.int64(0);
            root.path = st.text(1);
            root.status = st.text(2);
            root.includeHidden = st.int64(3) != 0;
            emit rootAdded(root);
        }
    }
    refreshRows();
}

void Catalogue::addRoot(const QString& path, bool includeHidden)
{
    const QFileInfo info(path);
    if (!info.isDir()) {
        emit rootRejected(QStringLiteral("Not a directory: %1").arg(path));
        return;
    }
    const QString absolute = info.absoluteFilePath();

    // Roots are real directories; linked paths must be added explicitly (§3).
    if (info.isSymLink()) {
        emit rootRejected(QStringLiteral("%1 is a symlink; add the real directory instead")
                              .arg(absolute));
        return;
    }

    // Reject identical or overlapping roots (§4).
    {
        Statement st = m_db->prepare("SELECT cmp_key FROM roots");
        while (st.step()) {
            const QString existingKey = st.text(0);
            if (existingKey == absolute) {
                emit rootRejected(QStringLiteral("Folder is already in the catalogue"));
                return;
            }
            const bool nested = existingKey.startsWith(absolute + QLatin1Char('/'))
                || absolute.startsWith(existingKey + QLatin1Char('/'));
            if (nested) {
                emit rootRejected(QStringLiteral("Folder overlaps an existing root: %1")
                                      .arg(existingKey));
                return;
            }
        }
    }

    {
        Statement st = m_db->prepare(
            "INSERT INTO roots(path, cmp_key, status, include_hidden) VALUES(?,?,'ok',?)");
        if (!st.isValid()) {
            emit operationFailed(m_db->lastError());
            return;
        }
        st.bind(1, absolute);
        st.bind(2, absolute);
        st.bind(3, includeHidden ? 1 : 0);
        if (!m_db->transaction([&] { return st.run(); })) {
            emit operationFailed(m_db->lastError());
            return;
        }
    }

    RootInfo root;
    root.id = m_db->lastInsertRowId();
    root.path = absolute;
    root.status = QStringLiteral("ok");
    root.includeHidden = includeHidden;
    emit rootAdded(root);

    // Adding a folder starts its first scan; discovered rows appear before
    // metadata extraction finishes (§5).
    startScan(root.id, false);
}

void Catalogue::removeRoot(qint64 rootId)
{
    // Catalogue state only: media files are never touched here (§3).
    if (m_scanActive.load() && m_scannedRootId == rootId)
        m_cancelRequested.store(true);

    Statement find = m_db->prepare("SELECT id FROM roots WHERE id=?");
    find.bind(1, rootId);
    if (!find.step()) {
        emit operationFailed(QStringLiteral("Unknown root"));
        return;
    }
    if (!m_db->transaction([this, rootId] {
            Statement del = m_db->prepare("DELETE FROM roots WHERE id=?");
            del.bind(1, rootId);
            return del.run();
        })) {
        emit operationFailed(m_db->lastError());
        return;
    }
    emit rootRemoved(rootId);
    emit rowsChanged(QList<VideoRow>{}, true);
}

void Catalogue::rescanRoot(qint64 rootId, bool force)
{
    if (m_scanActive.load()) {
        emit operationFailed(QStringLiteral("A scan is already running"));
        return;
    }
    Statement find =
        m_db->prepare("SELECT path, include_hidden FROM roots WHERE id=?");
    find.bind(1, rootId);
    if (!find.step()) {
        emit operationFailed(QStringLiteral("Unknown root"));
        return;
    }
    const QString path = find.text(0);

    // Offline roots are an incomplete-scan condition: no missing-marking.
    QFileInfo dirInfo(path);
    if (!dirInfo.isDir() || dirInfo.isSymLink()) {
        Statement st = m_db->prepare("UPDATE roots SET status='missing' WHERE id=?");
        st.bind(1, rootId);
        st.run();
        m_scannedRootId = rootId;
        emitProgress(QStringLiteral("missing"));
        return;
    }

    startScan(rootId, force);
}

void Catalogue::startScan(qint64 rootId, bool force)
{
    m_scanActive.store(true);
    m_cancelRequested.store(false);
    m_pauseRequested.store(false);
    m_scannedRootId = rootId;
    m_scanForce = force;
    m_discovered = 0;
    m_probed = 0;
    m_errors = 0;

    // Force refresh retries failed/timeout probes of unchanged files (§5).
    if (force) {
        m_db->exec(
            "UPDATE jobs SET state='queued', retries=0, error=NULL WHERE kind='probe' "
            "AND state='error' AND video_id IN (SELECT id FROM videos WHERE root_id="
            + QString::number(rootId).toUtf8() + ")");
    }

    Statement bump = m_db->prepare(
        "UPDATE roots SET scan_generation=scan_generation+1, status='ok' WHERE id=?");
    bump.bind(1, rootId);
    bump.run();

    runEnumerationLocked(rootId, force);
}

void Catalogue::runEnumerationLocked(qint64 rootId, bool force)
{
    emitProgress(QStringLiteral("enumerating"));

    Statement rootSt = m_db->prepare("SELECT path, include_hidden FROM roots WHERE id=?");
    rootSt.bind(1, rootId);
    if (!rootSt.step()) {
        m_scanActive.store(false);
        return;
    }
    const QString rootPath = rootSt.text(0);
    const bool includeHidden = rootSt.int64(1) != 0;
    const qint64 generation =
        m_db->scalarInt(("SELECT scan_generation FROM roots WHERE id="
                         + QString::number(rootId)).toUtf8().constData())
            .value_or(0);

    DiscoveryOptions options;
    options.rootPath = rootPath;
    options.includeHidden = includeHidden;

    QElapsedTimer sinceCommit;
    sinceCommit.start();
    int rowsSinceCommit = 0;
    QList<VideoRow> pendingRows;

    auto flushRows = [this, &pendingRows] {
        if (!pendingRows.isEmpty()) {
            emit rowsChanged(pendingRows, false);
            pendingRows.clear();
        }
    };
    // Commits batches at 250 rows or 100 ms, whichever comes first (§5);
    // the transaction stays open between batches and is finished after
    // enumeration via sqlite3_get_autocommit.
    auto commitIfDue = [this, &rowsSinceCommit, &sinceCommit, &flushRows] {
        if (rowsSinceCommit >= 250 || sinceCommit.elapsed() >= 100) {
            m_db->exec("COMMIT");
            rowsSinceCommit = 0;
            sinceCommit.restart();
            flushRows();
        }
    };

    SourceScanner::BatchSink sink =
        [this, rootId, rootPath, generation, force, &rowsSinceCommit, &sinceCommit,
         &pendingRows, &flushRows, &commitIfDue](QVector<DiscoveredFile>&& batch) {
            // Pause takes effect between batches; queued edits keep running.
            while (m_pauseRequested.load() && !m_cancelRequested.load()) {
                QEventLoop loop;
                QTimer timer;
                timer.setInterval(50);
                QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
                timer.start();
                loop.exec();
            }
            if (m_cancelRequested.load())
                return;

            m_discovered += static_cast<quint64>(batch.size());
            if (!m_db->exec("BEGIN IMMEDIATE"))
                return;

            for (const DiscoveredFile& file : batch) {
                const QString rel = file.absolutePath.mid(rootPath.size() + 1);
                const QString cmpKey = rel;
                const qint64 now = QDateTime::currentMSecsSinceEpoch();

                Statement find = m_db->prepare(
                    "SELECT id, size_bytes, mtime_ms, revision, availability, probe_status "
                    "FROM videos WHERE root_id=? AND cmp_key=?");
                find.bind(1, rootId);
                find.bind(2, cmpKey);
                if (find.step()) {
                    const qint64 id = find.int64(0);
                    const qint64 oldSize = find.int64(1);
                    const qint64 oldMtime = find.int64(2);
                    const qint64 revision = find.int64(3);
                    const QString availability = find.text(4);
                    const QString probeStatus = find.text(5);
                    const bool unchanged = oldSize == static_cast<qint64>(file.sizeBytes)
                        && oldMtime == file.mtimeMs;

                    if (unchanged && !force && availability != QLatin1String("missing")) {
                        Statement seen = m_db->prepare(
                            "UPDATE videos SET seen_generation=? WHERE id=?");
                        seen.bind(1, generation);
                        seen.bind(2, id);
                        seen.run();
                        if (probeStatus != QLatin1String("ok")
                            && availability != QLatin1String("missing")) {
                            // Present again after a probe failure: retry once
                            // through a fresh queued job.
                            Statement requeue = m_db->prepare(
                                "UPDATE jobs SET state='queued', retries=0, error=NULL "
                                "WHERE video_id=? AND kind='probe' AND revision=?");
                            requeue.bind(1, id);
                            requeue.bind(2, revision);
                            requeue.run();
                        }
                        continue;
                    }
                    if (unchanged && !force && availability == QLatin1String("missing")) {
                        // The file is back with identical identity: restore it.
                        Statement seen = m_db->prepare(
                            "UPDATE videos SET seen_generation=?, availability=? WHERE id=?");
                        seen.bind(1, generation);
                        seen.bind(2, probeStatus == QLatin1String("ok")
                                         ? QStringLiteral("available")
                                         : QStringLiteral("unprobed"));
                        seen.bind(3, id);
                        seen.run();
                        continue;
                    }
                    if (unchanged && force && probeStatus == QLatin1String("ok")
                        && availability == QLatin1String("missing")) {
                        Statement seen = m_db->prepare(
                            "UPDATE videos SET seen_generation=?, availability='available' "
                            "WHERE id=?");
                        seen.bind(1, generation);
                        seen.bind(2, id);
                        seen.run();
                        continue;
                    }

                    // Changed content at the same path: new revision, extracted
                    // data invalidated, annotations preserved (§4).
                    Statement upd = m_db->prepare(
                        "UPDATE videos SET size_bytes=?, mtime_ms=?, revision=revision+1, "
                        "duration_ms=NULL, coded_width=NULL, coded_height=NULL, "
                        "display_width=NULL, display_height=NULL, rotation_deg=NULL, "
                        "codec=NULL, availability='unprobed', probe_status='pending', "
                        "probe_error=NULL, seen_generation=? WHERE id=?");
                    upd.bind(1, static_cast<qint64>(file.sizeBytes));
                    upd.bind(2, file.mtimeMs);
                    upd.bind(3, generation);
                    upd.bind(4, id);
                    upd.run();
                    m_db->exec("DELETE FROM jobs WHERE video_id="
                               + QString::number(id).toUtf8());
                    // Invalidate extracted data: rows and their owned
                    // artifact files go together (§4, §6).
                    {
                        Statement artifacts = m_db->prepare(
                            "SELECT rel_path FROM cache_entries WHERE video_id=?");
                        artifacts.bind(1, id);
                        while (artifacts.step())
                            QFile::remove(m_cacheDir + QLatin1Char('/')
                                          + artifacts.text(0));
                    }
                    m_db->exec("DELETE FROM cache_entries WHERE video_id="
                               + QString::number(id).toUtf8());
                    Statement job = m_db->prepare(
                        "INSERT OR IGNORE INTO jobs(video_id, revision, kind, state) "
                        "VALUES(?,?,'probe','queued')");
                    job.bind(1, id);
                    job.bind(2, revision + 1);
                    job.run();
                    ++rowsSinceCommit;
                    pendingRows.append(readRow(id));
                    continue;
                }

                // New file: insert and queue its probe.
                Statement ins = m_db->prepare(
                    "INSERT INTO videos(root_id, rel_path, cmp_key, file_name, size_bytes, "
                    "mtime_ms, revision, views, added_ms, availability, probe_status, "
                    "seen_generation) VALUES(?,?,?,?,?,?,1,0,?,'unprobed','pending',?)");
                ins.bind(1, rootId);
                ins.bind(2, rel);
                ins.bind(3, cmpKey);
                ins.bind(4, fileNameOf(file.absolutePath));
                ins.bind(5, static_cast<qint64>(file.sizeBytes));
                ins.bind(6, file.mtimeMs);
                ins.bind(7, now);
                ins.bind(8, generation);
                if (!ins.run())
                    continue;
                const qint64 id = m_db->lastInsertRowId();
                Statement job = m_db->prepare(
                    "INSERT OR IGNORE INTO jobs(video_id, revision, kind, state) "
                    "VALUES(?,1,'probe','queued')");
                job.bind(1, id);
                job.run();
                ++rowsSinceCommit;
                pendingRows.append(readRow(id));
            }

            commitIfDue();
        };

    const DiscoveryResult result =
        SourceScanner::enumerateRoot(options, m_cancelRequested, sink);

    // Finish any open batched transaction before probing begins.
    if (!sqlite3_get_autocommit(m_db->handle()))
        m_db->exec("COMMIT");
    flushRows();

    if (!result.completed) {
        // Cancelled scan: no missing-marking, prior availability retained (§5).
        killActiveJobs();
        m_scanActive.store(false);
        emitProgress(QStringLiteral("cancelled"));
        return;
    }
    m_errors += static_cast<quint64>(result.issues.size());

    // Complete enumeration: unseen entries become missing; their queued
    // probes are pointless and are removed. Annotations are preserved (§5).
    m_db->transaction([this, rootId, generation] {
        Statement missing = m_db->prepare(
            "UPDATE videos SET availability='missing' WHERE root_id=? AND "
            "(seen_generation IS NULL OR seen_generation < ?) AND availability != 'missing'");
        missing.bind(1, rootId);
        missing.bind(2, generation);
        missing.run();
        Statement delJobs = m_db->prepare(
            "DELETE FROM jobs WHERE state='queued' AND video_id IN "
            "(SELECT id FROM videos WHERE root_id=? AND availability='missing')");
        delJobs.bind(1, rootId);
        delJobs.run();
        Statement touch = m_db->prepare("UPDATE roots SET last_complete_scan_ms=? WHERE id=?");
        touch.bind(1, QDateTime::currentMSecsSinceEpoch());
        touch.bind(2, rootId);
        touch.run();
        return true;
    });

    emitProgress(QStringLiteral("probing"));
    ensureDispatchTimer();
    m_dispatchTimer->start(0);
}

void Catalogue::ensureDispatchTimer()
{
    if (!m_dispatchTimer) {
        m_dispatchTimer = new QTimer(this);
        m_dispatchTimer->setSingleShot(true);
        connect(m_dispatchTimer, &QTimer::timeout, this, &Catalogue::dispatchJobs);
    }
}

void Catalogue::dispatchJobs()
{
    if (m_cancelRequested.load()) {
        killActiveJobs();
        m_scanActive.store(false);
        emitProgress(QStringLiteral("cancelled"));
        return;
    }
    if (m_pauseRequested.load()) {
        // Paused background work starts no new jobs (§5); retry shortly.
        if (m_scanActive.load()) {
            ensureDispatchTimer();
            m_dispatchTimer->start(200);
        }
        return;
    }
    while (m_activeJobs.size() < m_jobConcurrency) {
        // Priority: posters (visible cards) before remaining probes, and
        // background storyboards last (§5).
        Statement job = m_db->prepare(
            "SELECT j.id, j.video_id, j.revision, j.kind, v.cmp_key, r.path FROM jobs j "
            "JOIN videos v ON v.id = j.video_id JOIN roots r ON r.id = v.root_id "
            "WHERE j.state='queued' "
            "ORDER BY CASE j.kind WHEN 'poster' THEN 0 WHEN 'probe' THEN 1 ELSE 2 END, "
            "j.id LIMIT 1");
        if (!job.step())
            break;
        const qint64 jobId = job.int64(0);
        const qint64 videoId = job.int64(1);
        const qint64 revision = job.int64(2);
        const QString kind = job.text(3);
        const QString rel = job.text(4);
        const QString rootPath = job.text(5);

        // A queued job for an older revision is obsolete; drop it (§5).
        Statement rev = m_db->prepare(
            "SELECT revision, duration_ms, stream_index FROM videos WHERE id=?");
        rev.bind(1, videoId);
        if (!rev.step() || rev.int64(0) != revision) {
            Statement del = m_db->prepare("DELETE FROM jobs WHERE id=?");
            del.bind(1, jobId);
            del.run();
            continue;
        }
        const qint64 durationMs = rev.isNull(1) ? -1 : rev.int64(1);
        const int streamIndex = rev.isNull(2) ? 0 : static_cast<int>(rev.int64(2));

        Statement mark = m_db->prepare("UPDATE jobs SET state='running' WHERE id=?");
        mark.bind(1, jobId);
        mark.run();

        auto* active = new ActiveJob;
        active->jobId = jobId;
        active->videoId = videoId;
        active->revision = revision;
        active->kind = kind;
        active->durationMs = durationMs;
        active->streamIndex = streamIndex;
        m_activeJobs.append(active);

        const QString absPath = rootPath + QLatin1Char('/') + rel;
        if (!m_pool)
            m_pool = new QThreadPool(this);
        m_pool->setMaxThreadCount(m_jobConcurrency);
        m_pool->start([this, active, absPath] { runJob(active, absPath); });
    }

    if (m_activeJobs.isEmpty() && m_scanActive.load()) {
        Statement queued =
            m_db->prepare("SELECT COUNT(*) FROM jobs WHERE state='queued'");
        queued.step();
        if (queued.int64(0) == 0) {
            m_scanActive.store(false);
            emitProgress(QStringLiteral("complete"));
        }
    }
}

void Catalogue::runJob(ActiveJob* job, const QString& absPath)
{
    // Pool thread: never touches the DB; results are queued back to the
    // catalogue thread (§2).
    const PidSink sink = [job](qint64 pid) { job->pid.store(pid); };
    if (job->kind == QLatin1String("probe")) {
        const ProbeResult result = Probe::run(m_ffprobePath, absPath, m_probeTimeoutMs,
                                              sink, &job->cancelled);
        QMetaObject::invokeMethod(this,
                                  [this, job, result] { finishJob(job, result, {}); },
                                  Qt::QueuedConnection);
        return;
    }

    ExtractResult result;
    if (job->kind == QLatin1String("poster")) {
        PosterRequest request;
        request.ffmpegPath = m_ffmpegPath;
        request.sourcePath = absPath;
        request.outputPath = artifactPath(job->videoId, job->revision,
                                          QLatin1String(kPosterProfile));
        request.durationMs = job->durationMs;
        request.selectedStreamIndex = job->streamIndex;
        request.timeoutMs = m_previewTimeoutMs;
        result = Extract::poster(request, sink, &job->cancelled);
    } else if (job->kind == QLatin1String("storyboard")) {
        StoryboardRequest request;
        request.ffmpegPath = m_ffmpegPath;
        request.sourcePath = absPath;
        request.outputDir = m_cacheDir + QStringLiteral("/tmp-%1").arg(job->videoId);
        request.outputPath = artifactPath(job->videoId, job->revision,
                                          QLatin1String(kStoryboardProfile));
        request.durationMs = job->durationMs;
        request.selectedStreamIndex = job->streamIndex;
        request.timeoutMs = m_previewTimeoutMs;
        result = Extract::storyboard(request, sink, &job->cancelled);
    } else {
        result.error = QStringLiteral("unknown job kind");
    }
    QMetaObject::invokeMethod(this,
                              [this, job, result] { finishJob(job, {}, result); },
                              Qt::QueuedConnection);
}

void Catalogue::finishJob(ActiveJob* job, std::optional<ProbeResult> probe,
                          std::optional<ExtractResult> extract)
{
    if (!m_activeJobs.removeOne(job))
        return;
    job->pid.store(0);

    if (m_cancelRequested.load()) {
        // Cancellation is not a failure: the job returns to queued (§5).
        Statement requeue = m_db->prepare("UPDATE jobs SET state='queued' WHERE id=?");
        requeue.bind(1, job->jobId);
        requeue.run();
        delete job;
        dispatchJobs();
        return;
    }

    // Stale result for a superseded revision: discard it entirely (§5).
    Statement rev = m_db->prepare("SELECT revision FROM videos WHERE id=?");
    rev.bind(1, job->videoId);
    const bool stale = !rev.step() || rev.int64(0) != job->revision;
    if (stale) {
        Statement del = m_db->prepare("DELETE FROM jobs WHERE id=?");
        del.bind(1, job->jobId);
        del.run();
        delete job;
        dispatchJobs();
        return;
    }

    if (probe.has_value()) {
        const bool finished = applyProbeResult(job->videoId, job->revision, probe.value());
        if (finished) {
            Statement del = m_db->prepare("DELETE FROM jobs WHERE id=?");
            del.bind(1, job->jobId);
            del.run();
        }
    } else if (extract.has_value()) {
        applyExtractResult(job, extract.value());
    }
    delete job;
    dispatchJobs();
}

// Returns true when the job is done (success or exhausted); false when it was
// re-queued for its one automatic retry (§5).
bool Catalogue::applyProbeResult(qint64 videoId, qint64 revision, const ProbeResult& result)
{
    m_probed += 1;
    if (result.ok) {
        m_db->transaction([this, videoId, revision, &result] {
            Statement upd = m_db->prepare(
                "UPDATE videos SET probe_status='ok', availability='available', "
                "duration_ms=?, codec=?, coded_width=?, coded_height=?, "
                "display_width=?, display_height=?, rotation_deg=?, stream_index=?, "
                "probe_error=NULL WHERE id=? AND revision=?");
            if (result.durationMs > 0)
                upd.bind(1, result.durationMs);
            else
                upd.bindNull(1);
            upd.bind(2, result.codec);
            if (result.codedWidth > 0) {
                upd.bind(3, result.codedWidth);
                upd.bind(4, result.codedHeight);
                upd.bind(5, result.displayWidth);
                upd.bind(6, result.displayHeight);
            } else {
                upd.bindNull(3);
                upd.bindNull(4);
                upd.bindNull(5);
                upd.bindNull(6);
            }
            upd.bind(7, result.rotationDeg);
            upd.bind(8, result.selectedStreamIndex);
            upd.bind(9, videoId);
            upd.bind(10, revision);
            return upd.run();
        });
        refreshSearchRecord(videoId);
        applyAutoTags(videoId, result);
        emitRows(QList<VideoRow>{readRow(videoId)});
        enqueuePreviewJobs(videoId, false);
        return true;
    }

    m_errors += 1;
    const bool timeout = result.failKind == ProbeResult::FailKind::Timeout;
    const bool transient = result.failKind == ProbeResult::FailKind::TransientIo;

    bool retried = false;
    m_db->transaction([this, videoId, revision, timeout, transient, &result, &retried] {
        Q_UNUSED(timeout);
        if (transient) {
            // One automatic retry for transient I/O failures, then explicit (§5).
            Statement retry = m_db->prepare(
                "UPDATE jobs SET retries=retries+1, state='queued', error=? WHERE "
                "video_id=? AND state='running' AND retries < 1");
            retry.bind(1, result.error);
            retry.bind(2, videoId);
            retry.run();
            if (m_db->lastChangeCount() > 0) {
                retried = true;
                return true;
            }
        }
        Statement fail = m_db->prepare(
            "UPDATE videos SET probe_status=?, availability='unavailable', probe_error=? "
            "WHERE id=? AND revision=?");
        fail.bind(1, timeout ? QStringLiteral("timeout") : QStringLiteral("error"));
        fail.bind(2, result.error.left(500));
        fail.bind(3, videoId);
        fail.bind(4, revision);
        fail.run();
        Statement job = m_db->prepare(
            "UPDATE jobs SET state='error', error=? WHERE video_id=? AND revision=?");
        job.bind(1, result.error.left(500));
        job.bind(2, videoId);
        job.bind(3, revision);
        job.run();
        return true;
    });
    emitRows(QList<VideoRow>{readRow(videoId)});
    return !retried;
}

void Catalogue::enqueuePreviewJobs(qint64 videoId, bool includeStoryboard)
{
    // Posters are generated before storyboards (§6); storyboards are queued
    // on demand (hover or explicit precompute).
    Statement info = m_db->prepare(
        "SELECT revision, duration_ms, stream_index, probe_status FROM videos WHERE id=?");
    info.bind(1, videoId);
    if (!info.step())
        return;
    const qint64 revision = info.int64(0);
    const qint64 durationMs = info.isNull(1) ? -1 : info.int64(1);
    const int streamIndex = info.isNull(2) ? 0 : static_cast<int>(info.int64(2));
    if (info.text(3) != QLatin1String("ok"))
        return;

    Statement posterJob = m_db->prepare(
        "INSERT OR IGNORE INTO jobs(video_id, revision, kind, state) "
        "VALUES(?,?,'poster','queued')");
    posterJob.bind(1, videoId);
    posterJob.bind(2, revision);
    posterJob.run();
    if (includeStoryboard && durationMs > 0) {
        Statement sbJob = m_db->prepare(
            "INSERT OR IGNORE INTO jobs(video_id, revision, kind, state) "
            "VALUES(?,?,'storyboard','queued')");
        sbJob.bind(1, videoId);
        sbJob.bind(2, revision);
        sbJob.run();
    }
    ensureDispatchTimer();
    m_dispatchTimer->start(0);
}

void Catalogue::requestStoryboard(qint64 videoId)
{
    // Lazy generation: a missing fallback storyboard is produced through
    // low-priority background work on first need (§6).
    enqueuePreviewJobs(videoId, true);
}

void Catalogue::requestPoster(qint64 videoId)
{
    enqueuePreviewJobs(videoId, false);
}

void Catalogue::applyExtractResult(ActiveJob* job, const ExtractResult& result)
{
    const QString profile = job->kind == QLatin1String("poster")
        ? QLatin1String(kPosterProfile) : QLatin1String(kStoryboardProfile);

    if (!result.ok) {
        m_errors += 1;
        m_db->transaction([this, job, &result]() -> bool {
            // One automatic retry for transient extraction failures (§5).
            Statement retry = m_db->prepare(
                "UPDATE jobs SET retries=retries+1, state='queued', error=? WHERE "
                "id=? AND state='running' AND retries < 1");
            retry.bind(1, result.error);
            retry.bind(2, job->jobId);
            retry.run();
            if (m_db->lastChangeCount() > 0)
                return true;
            Statement fail = m_db->prepare(
                "UPDATE jobs SET state='error', error=? WHERE id=?");
            fail.bind(1, result.error.left(500));
            fail.bind(2, job->jobId);
            fail.run();
            return true;
        });
        emit operationFailed(QStringLiteral("Preview generation failed: %1")
                                 .arg(result.error.left(200)));
        dispatchJobs();
        return;
    }

    const qint64 bytes = QFileInfo(result.outputPath).size();
    const QString relArtifact = QDir(m_cacheDir).relativeFilePath(result.outputPath);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_db->transaction([this, job, &result, &relArtifact, bytes, now] {
        Statement entry = m_db->prepare(
            "INSERT OR REPLACE INTO cache_entries(video_id, revision, profile, kind, "
            "rel_path, bytes, sample_times, last_access_ms) VALUES(?,?,?,?,?,?,?,?)");
        entry.bind(1, job->videoId);
        entry.bind(2, job->revision);
        entry.bind(3, job->kind == QLatin1String("poster")
                          ? QLatin1String(kPosterProfile) : QLatin1String(kStoryboardProfile));
        entry.bind(4, job->kind);
        entry.bind(5, relArtifact);
        entry.bind(6, bytes);
        if (result.sampleTimesMs.isEmpty()) {
            entry.bindNull(7);
        } else {
            QJsonArray times;
            for (const qint64 t : result.sampleTimesMs)
                times.append(t);
            entry.bind(7, QString::fromUtf8(
                              QJsonDocument(times).toJson(QJsonDocument::Compact)));
        }
        entry.bind(8, now);
        return entry.run();
    });
    Statement del = m_db->prepare("DELETE FROM jobs WHERE id=?");
    del.bind(1, job->jobId);
    del.run();
    emit cacheEntryChanged(job->videoId, profile);
    emitRows(QList<VideoRow>{readRow(job->videoId)});
    enforceCacheLimit();
    dispatchJobs();
}

void Catalogue::enforceCacheLimit()
{
    // Evict least-recently-used owned artifacts only; ratings and tags are
    // never touched (§6). LRU access times are per entry, not per mouse move.
    const qint64 limit = settingInt("disk_cache_bytes", m_diskCacheLimit);
    for (;;) {
        Statement sum = m_db->prepare("SELECT COALESCE(SUM(bytes),0) FROM cache_entries");
        sum.step();
        const qint64 total = sum.int64(0);
        if (total <= limit)
            return;
        Statement oldest = m_db->prepare(
            "SELECT video_id, profile, rel_path FROM cache_entries "
            "ORDER BY last_access_ms ASC LIMIT 1");
        if (!oldest.step())
            return;
        QFile::remove(m_cacheDir + QLatin1Char('/') + oldest.text(2));
        Statement del = m_db->prepare(
            "DELETE FROM cache_entries WHERE video_id=? AND profile=?");
        del.bind(1, oldest.int64(0));
        del.bind(2, oldest.text(1));
        del.run();
    }
}

void Catalogue::openInDefaultPlayer(qint64 videoId)
{
    // §9: only catalogue videos that currently resolve to regular files.
    Statement info = m_db->prepare(
        "SELECT v.rel_path, r.path FROM videos v JOIN roots r ON r.id = v.root_id "
        "WHERE v.id=?");
    info.bind(1, videoId);
    if (!info.step()) {
        emit operationFailed(QStringLiteral("Unknown video"));
        return;
    }
    const QString absPath = info.text(1) + QLatin1Char('/') + info.text(0);
    const QFileInfo fileInfo(absPath);
    if (!fileInfo.isFile() || fileInfo.isSymLink()) {
        emit operationFailed(QStringLiteral("File is not available: %1").arg(absPath));
        return;
    }
    // §9: the return value means the request was handed to the system, not
    // that playback succeeded. One view per accepted launch action; failed
    // handoffs do not count.
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(absPath)))
        incrementViews(videoId);
}

void Catalogue::hoverEngage(qint64 videoId)
{
    Statement info = m_db->prepare(
        "SELECT v.revision, v.rel_path, r.path, v.duration_ms, v.availability "
        "FROM videos v JOIN roots r ON r.id = v.root_id WHERE v.id=?");
    info.bind(1, videoId);
    if (!info.step()) {
        emit hoverSourceReady(videoId, 0, QString(), -1);
        return;
    }
    const qint64 revision = info.int64(0);
    const QString absPath = info.text(2) + QLatin1Char('/') + info.text(1);
    const qint64 durationMs = info.isNull(3) ? -1 : info.int64(3);
    const QString availability = info.text(4);
    // The original-file session reads only available files (§6).
    if (availability != QLatin1String("available"))
        emit hoverSourceReady(videoId, revision, QString(), durationMs);
    else
        emit hoverSourceReady(videoId, revision, absPath, durationMs);
}

void Catalogue::requestSampleTimes(qint64 videoId)
{
    Statement info = m_db->prepare(
        "SELECT sample_times FROM cache_entries WHERE video_id=? AND kind='storyboard'");
    info.bind(1, videoId);
    if (!info.step()) {
        emit sampleTimesReady(videoId, {});
        return;
    }
    const QJsonDocument doc =
        QJsonDocument::fromJson(info.text(0).toUtf8());
    QVariantList times;
    for (const QJsonValue& v : doc.array())
        times.append(v.toVariant());
    emit sampleTimesReady(videoId, times);
}

void Catalogue::killActiveJobs()
{
    for (ActiveJob* job : m_activeJobs) {
        job->cancelled.store(true);
        const qint64 pid = job->pid.load();
        if (pid > 0) {
#ifdef Q_OS_UNIX
            ::kill(-pid, SIGTERM);
#else
            QProcess process;
            process.start(QStringLiteral("taskkill"),
                          {QStringLiteral("/PID"), QString::number(pid),
                           QStringLiteral("/T"), QStringLiteral("/F")});
            process.waitForFinished(1000);
#endif
        }
    }
    // Survivors are SIGKILLed after 2 s (§11 cancel gate: stopped ≤ 2 s).
    // Capture PID values, never job pointers: jobs may be finished and freed
    // before the timer fires.
    QList<qint64> killedPids;
    for (ActiveJob* job : m_activeJobs) {
        if (job->pid.load() > 0)
            killedPids.append(job->pid.load());
    }
    QTimer::singleShot(2000, this, [this, killedPids] {
        for (ActiveJob* job : m_activeJobs) {
            const qint64 pid = job->pid.load();
            if (pid > 0 && killedPids.contains(pid)) {
#ifdef Q_OS_UNIX
                ::kill(-pid, SIGKILL);
#endif
            }
        }
    });
}

void Catalogue::pauseScanning()
{
    m_pauseRequested.store(true);
    if (m_scanActive.load())
        emitProgress(QStringLiteral("paused"));
}

void Catalogue::resumeScanning()
{
    m_pauseRequested.store(false);
    if (m_scanActive.load())
        emitProgress(QStringLiteral("probing"));
}

void Catalogue::cancelScanning()
{
    if (!m_scanActive.load())
        return;
    m_cancelRequested.store(true);
    m_pauseRequested.store(false);
    killActiveJobs();
    emitProgress(QStringLiteral("cancelled"));
}

void Catalogue::setRating(qint64 videoId, int rating)
{
    if (rating < 0 || rating > 5)
        return;
    m_db->transaction([this, videoId, rating] {
        Statement st = m_db->prepare("UPDATE videos SET rating=? WHERE id=?");
        if (rating == 0)
            st.bindNull(1);
        else
            st.bind(1, rating);
        st.bind(2, videoId);
        return st.run();
    });
    emit ratingCommitted(videoId, rating);
    emitRows(QList<VideoRow>{readRow(videoId)});
}

void Catalogue::incrementViews(qint64 videoId)
{
    m_db->transaction([this, videoId] {
        Statement st = m_db->prepare(
            "UPDATE videos SET views=views+1, last_opened_ms=? WHERE id=?");
        st.bind(1, QDateTime::currentMSecsSinceEpoch());
        st.bind(2, videoId);
        return st.run();
    });
    emitRows(QList<VideoRow>{readRow(videoId)});
}

void Catalogue::refreshRows()
{
    QList<VideoRow> rows;
    Statement st = m_db->prepare("SELECT id FROM videos ORDER BY root_id, file_name, id");
    while (st.step())
        rows.append(readRow(st.int64(0)));
    emit rowsChanged(rows, true);
}

VideoRow Catalogue::readRow(qint64 videoId)
{
    VideoRow row;
    Statement st = m_db->prepare(
        "SELECT id, root_id, rel_path, file_name, size_bytes, mtime_ms, revision, "
        "duration_ms, display_width, display_height, codec, rating, views, added_ms, "
        "availability, probe_status, "
        "EXISTS(SELECT 1 FROM cache_entries c WHERE c.video_id=v.id AND c.kind='poster'), "
        "EXISTS(SELECT 1 FROM cache_entries c WHERE c.video_id=v.id AND c.kind='storyboard') "
        "FROM videos v WHERE v.id=?");
    st.bind(1, videoId);
    if (!st.step())
        return row;
    row.id = st.int64(0);
    row.rootId = st.int64(1);
    row.relPath = st.text(2);
    row.fileName = st.text(3);
    row.sizeBytes = st.isNull(4) ? -1 : st.int64(4);
    row.mtimeMs = st.isNull(5) ? -1 : st.int64(5);
    row.revision = st.int64(6);
    row.durationMs = st.isNull(7) ? -1 : st.int64(7);
    row.displayWidth = st.isNull(8) ? 0 : static_cast<int>(st.int64(8));
    row.displayHeight = st.isNull(9) ? 0 : static_cast<int>(st.int64(9));
    row.codec = st.text(10);
    row.rating = st.isNull(11) ? 0 : static_cast<int>(st.int64(11));
    row.views = st.int64(12);
    row.addedMs = st.int64(13);
    row.availability = st.text(14);
    row.probeStatus = st.text(15);
    row.posterReady = st.int64(16) != 0;
    row.atlasReady = st.int64(17) != 0;
    return row;
}

void Catalogue::emitProgress(const QString& state)
{
    m_scanState = state;
    ScanProgress progress;
    progress.rootId = m_scannedRootId;
    progress.state = state;
    progress.discovered = m_discovered;
    progress.probed = m_probed;
    progress.errors = m_errors;
    Statement st = m_db->prepare("SELECT path FROM roots WHERE id=?");
    st.bind(1, m_scannedRootId);
    progress.rootPath = st.step() ? st.text(0) : QString();
    emit scanProgress(progress);
}

void Catalogue::emitRows(const QList<VideoRow>& rows)
{
    emit rowsChanged(rows, false);
}

QString Catalogue::artifactPath(qint64 videoId, qint64 revision,
                                const QString& profile) const
{
    return m_cacheDir + QStringLiteral("/%1-%2-%3.jpg").arg(videoId).arg(revision).arg(profile);
}

qint64 Catalogue::settingInt(const char* key, qint64 fallback) const
{
    Statement st = m_db->prepare("SELECT value FROM settings WHERE key=?");
    st.bind(1, QString::fromLatin1(key));
    if (!st.step())
        return fallback;
    bool ok = false;
    const qint64 value = st.text(0).toLongLong(&ok);
    return ok ? value : fallback;
}

void Catalogue::setSettingInt(const char* key, qint64 value)
{
    Statement st = m_db->prepare(
        "INSERT OR REPLACE INTO settings(key, value, version) VALUES(?,?,1)");
    st.bind(1, QString::fromLatin1(key));
    st.bind(2, QString::number(value));
    st.run();
}

} // namespace itub
