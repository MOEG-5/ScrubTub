// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Catalogue store and scan coordinator (TECH_SPEC.md sections 1, 4, 5).
//
// Lives on one dedicated worker thread that owns the SQLite connection.
// Public slots are invoked through queued connections; results come back as
// signals. The UI thread never touches SQLite, the filesystem, or processes.
#pragma once

#include "VideoRow.h"

#include <QObject>
#include <QProcess>
#include <QStringList>

#include <atomic>
#include <memory>

class QLockFile;
class QTimer;

namespace itub {

class Database;
struct ProbeResult;

class Catalogue : public QObject {
    Q_OBJECT
public:
    explicit Catalogue(QObject* parent = nullptr);
    ~Catalogue() override;

    // Opens/creates the database, applies migrations, resets interrupted jobs,
    // and takes the profile lock. Call on this object's thread.
    bool initialize(const QString& profileDataDir, const QString& ffprobePath,
                    QString* error);

    // Test hooks.
    void setProbeConcurrency(int n) { m_probeConcurrency = n; }
    void setProbeTimeoutMs(int ms) { m_probeTimeoutMs = ms; }

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
    void incrementViews(qint64 videoId);        // M3 wiring; committed transactionally
    void refreshRows();

signals:
    void initialized(bool ok);
    void rootAdded(const itub::RootInfo& root);
    void rootRemoved(qint64 rootId);
    void rootRejected(const QString& reason);
    void scanProgress(const itub::ScanProgress& progress);
    void rowsChanged(const QList<itub::VideoRow>& rows, bool reset);
    void ratingCommitted(qint64 videoId, int rating);
    void operationFailed(const QString& message);

private:
    struct ActiveProbe {
        qint64 videoId = 0;
        qint64 revision = 0;
        qint64 jobId = 0;
        std::unique_ptr<QProcess> process;
        bool timedOut = false;
    };

    void startScan(qint64 rootId, bool force);
    void runEnumerationLocked(qint64 rootId, bool force);
    void dispatchProbeJobs();
    void finishProbeJob(ActiveProbe* job, int exitCode, QProcess::ExitStatus status);
    void killActiveProbes();
    bool applyProbeResult(qint64 videoId, qint64 revision, const ProbeResult& result);
    void emitProgress(const QString& state);
    void emitRows(const QList<VideoRow>& rows);
    VideoRow readRow(qint64 videoId);

    std::unique_ptr<Database> m_db;
    QString m_profileDataDir;
    QString m_ffprobePath;
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
    QTimer* m_probeTimer = nullptr;
    QList<ActiveProbe*> m_activeProbes;
    int m_probeConcurrency = 2;   // §5: at most two active probe/decode processes
    int m_probeTimeoutMs = 30000; // §5: 30 s default probe timeout
};

} // namespace itub
