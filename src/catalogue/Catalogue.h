// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Catalogue store, scan coordinator, and media job scheduler
// (TECH_SPEC.md sections 1, 4, 5, 6).
//
// Lives on one dedicated worker thread that owns the SQLite connection.
// Public slots are invoked through queued connections; results come back as
// signals. The UI thread never touches SQLite, the filesystem, or processes.
// Probe/poster/storyboard subprocesses run on a bounded pool — at most two
// active media processes combined (§5).
#pragma once

#include "VideoRow.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <optional>

class QLockFile;
class QThreadPool;
class QTimer;

namespace itub {

class Database;
struct ProbeResult;
struct ExtractResult;

// Search snapshot record (§8): compact pre-normalized text per row.
struct SearchRecord {
    qint64 id = 0;
    QString fileName;
    QString relPath;
    QStringList tokens;       // normalized name/path/tag tokens
    QStringList foldedTokens; // diacritic-folded copies
};

// Structured query (§1, §8): all categories AND-combined; bounds inclusive;
// unknown (NULL) values never match numeric ranges unless the explicit
// unknown option is set on the filter.
struct QuerySpec {
    QString text;

    qlonglong sizeMin = -1;
    qlonglong sizeMax = -1;
    int widthMin = 0;
    int widthMax = 0;
    int heightMin = 0;
    int heightMax = 0;
    QString resolutionPreset;        // shorter-side bucket: 480..2160
    qlonglong durationMinMs = -1;
    qlonglong durationMaxMs = -1;

    int ratingMode = 0;              // 0 any, 1 unrated, 2 exact value
    int ratingValue = 0;

    QStringList includeAllTags;      // normalized; every one required
    QStringList includeAnyTags;      // normalized; at least one
    QStringList excludeTags;         // normalized; never present

    qlonglong rootId = -1;           // -1 = any root
    QString folderPrefix;            // relative path prefix
    qlonglong viewsMin = -1;         // -1 = no filter; 0 includes zero views
    QStringList availability;        // empty = all

    QString sortKey = QStringLiteral("added");
    bool sortDescending = false;
    bool userSort = false;           // explicit user sort overrides relevance
};

class Catalogue : public QObject {
    Q_OBJECT
public:
    explicit Catalogue(QObject* parent = nullptr);
    ~Catalogue() override;

    // Opens/creates the database, applies migrations, resets interrupted jobs,
    // and takes the profile lock. Call on this object's thread.
    bool initialize(const QString& profileDataDir, const QString& ffprobePath,
                    const QString& ffmpegPath, QString* error);

    // Tuning (§5: proposed settings to validate, not immutable constants).
    void setProbeConcurrency(int n) { m_jobConcurrency = n; }
    void setProbeTimeoutMs(int ms) { m_probeTimeoutMs = ms; }
    void setPreviewTimeoutMs(int ms) { m_previewTimeoutMs = ms; }
    void setDiskCacheLimitBytes(qint64 bytes) { m_diskCacheLimit = bytes; }

public slots:
    // Roots. Rejection reasons (duplicate, nested, symlink, nonexistent) are
    // reported via rootRejected().
    void addRoot(const QString& path, bool includeHidden = false);
    void removeRoot(qint64 rootId); // catalogue state only; never touches media

    // Scanning. One scan at a time; requests while active are reported busy.
    void rescanRoot(qint64 rootId, bool force = false);
    void pauseScanning();
    void resumeScanning();
    void cancelScanning();

    // Annotations (catalogue-only operations).
    void setRating(qint64 videoId, int rating); // 0 clears
    void incrementViews(qint64 videoId);
    void refreshRows();

    // Preview artifacts. Poster generation is queued automatically after a
    // successful probe; storyboards are queued on demand (hover or explicit
    // precompute).
    void requestStoryboard(qint64 videoId);
    void requestPoster(qint64 videoId);

    // Open in the system default player (§9). Counts one view per accepted
    // launch request; failed handoffs do not count.
    void openInDefaultPlayer(qint64 videoId);

    // Restores the previous session's roots and rows (§1 persistence).
    // Called after UI connections are in place so nothing is lost.
    void loadExistingState();

    // Hover plumbing: resolve the source for the paused-player session on the
    // catalogue thread; results are delivered queued to the UI.
    void hoverEngage(qint64 videoId);
    void requestSampleTimes(qint64 videoId);

    // Live search (§8): structured filters first, then every query token must
    // match; stale results are discarded by generation. Details are fetched
    // by ID pages on demand.
    void search(const itub::QuerySpec& spec);
    void fetchRowsPage(const QList<qint64>& videoIds);
    void clearSearch();
    void setPageSize(int n) { m_pageSize = n; }

    // Tags (§7): catalogue-only edits; the source filename is never touched.
    void addManualTag(qint64 videoId, const QString& text);
    void removeTag(qint64 videoId, qint64 tagId);
    void suppressAutoTag(qint64 videoId, qint64 tagId);
    void resetSuppressions();
    void regenerateAutoTags(qint64 videoId);
    void requestTags(qint64 videoId);

    // Explicit file operations and recovery (§3, §4, milestone 4).
    // Move videos to the OS Trash/Recycle Bin. Identity is re-checked
    // immediately before each operation; failures keep the catalogue entry
    // and are reported per item; there is no permanent-delete fallback.
    void trashVideos(const QList<qint64>& videoIds);

    // Backup/export and restore (§4): consistent snapshot including all
    // annotations; restore validates schema/integrity on an app-owned copy
    // before replacing the active database and preserves the prior one.
    void exportBackup(const QString& destPath);
    void importBackup(const QString& srcPath);

    // Cache controls (§6): only owned artifacts are ever removed.
    void clearPreviews();
    void requestCacheUsage();

signals:
    void rootAdded(const itub::RootInfo& root);
    void rootRemoved(qint64 rootId);
    void rootRejected(const QString& reason);
    void scanProgress(const itub::ScanProgress& progress);
    void rowsChanged(const QList<itub::VideoRow>& rows, bool reset);
    void ratingCommitted(qint64 videoId, int rating);
    void cacheEntryChanged(qint64 videoId, const QString& profile);
    void hoverSourceReady(qint64 videoId, qint64 revision, const QString& absolutePath,
                          qint64 durationMs);
    void sampleTimesReady(qint64 videoId, const QVariantList& timesMs);
    void searchCompleted(quint64 generation, const QList<qint64>& orderedIds,
                         const QString& validationError);
    void tagsReady(qint64 videoId, const QVariantList& tags);
    void tagListChanged();
    void trashResult(qint64 videoId, bool ok, const QString& reason);
    void backupExported(const QString& destPath);
    void backupImported();
    void cacheUsageReady(qint64 bytes);
    void settingsReady(const QVariantMap& settings);
    void operationFailed(const QString& message);

private:
    struct ActiveJob {
        qint64 jobId = 0;
        qint64 videoId = 0;
        qint64 revision = 0;
        QString kind;                 // probe|poster|storyboard
        qint64 durationMs = -1;       // from the probe result
        int streamIndex = 0;          // selected video stream
        std::atomic_bool cancelled{false};
        // PID of the running child process, 0 when none. Registration lets
        // cancellation kill process trees promptly (§11).
        std::atomic<qint64> pid{0};
    };

    void startScan(qint64 rootId, bool force);
    void runEnumerationLocked(qint64 rootId, bool force);
    void ensureDispatchTimer();
    void dispatchJobs();
    void runJob(ActiveJob* job, const QString& absPath);
    void finishJob(ActiveJob* job, std::optional<ProbeResult> probe,
                   std::optional<ExtractResult> extract);
    void killActiveJobs();
    bool applyProbeResult(qint64 videoId, qint64 revision, const ProbeResult& result);
    void applyExtractResult(ActiveJob* job, const ExtractResult& result);
    void enqueuePreviewJobs(qint64 videoId, bool includeStoryboard);
    void enforceCacheLimit();
    void emitProgress(const QString& state);
    void emitRows(const QList<VideoRow>& rows);
    VideoRow readRow(qint64 videoId);
    QString artifactPath(qint64 videoId, qint64 revision, const QString& profile) const;
    qint64 settingInt(const char* key, qint64 fallback) const;
    void setSettingInt(const char* key, qint64 value);
    QString settingText(const char* key, const QString& fallback) const;
    void setSettingText(const char* key, const QString& value);
public:
    void saveUiSettingsMap(const QVariantMap& settings);
    void restoreSettings();

private:
    void refreshSearchRecord(qint64 videoId);
    void applyAutoTags(qint64 videoId, const ProbeResult& probe);

    QString whereForFilters(const QuerySpec& spec, QStringList* wheres) const;

    QHash<qint64, SearchRecord> m_searchRecords;
    quint64 m_searchGeneration = 0;
    int m_pageSize = 200;         // §8 card-detail page size

    std::unique_ptr<Database> m_db;
    QString m_profileDataDir;
    QString m_cacheDir;
    QString m_ffprobePath;
    QString m_ffmpegPath;
    std::unique_ptr<QLockFile> m_profileLock;

    std::atomic_bool m_pauseRequested{false};
    std::atomic_bool m_cancelRequested{false};
    std::atomic_bool m_scanActive{false};
    qint64 m_scannedRootId = 0;
    bool m_scanForce = false;
    QString m_scanState = QStringLiteral("idle");
    quint64 m_discovered = 0;
    quint64 m_errors = 0;
    quint64 m_probed = 0;
    QTimer* m_dispatchTimer = nullptr;
    QThreadPool* m_pool = nullptr;
    QList<ActiveJob*> m_activeJobs;
    int m_jobConcurrency = 2;      // §5: at most two active media processes
    int m_probeTimeoutMs = 30000;  // §5: 30 s probe timeout
    int m_previewTimeoutMs = 60000; // §5: 60 s per-preview-job timeout
    qint64 m_diskCacheLimit = 5LL * 1024 * 1024 * 1024; // §6: 5 GiB default
};

} // namespace itub
