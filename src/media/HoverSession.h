// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Original-file paused scrubbing session (TECH_SPEC.md section 6).
//
// One idle mpv process, private IPC, keyframe seeks during motion and exact
// refinement on settle. Native image output survives stop/loadfile; sources
// unload on leave while the process stays warm. Cached previews cover missing mpv.
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QTemporaryDir>
#include <memory>


namespace scrubtub {

class HoverSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)
    Q_PROPERTY(QImage lastFrame READ lastFrame NOTIFY frameChanged FINAL)
    Q_PROPERTY(qint64 lastFramePtsMs READ lastFramePtsMs NOTIFY frameChanged FINAL)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged FINAL)
    Q_PROPERTY(qint64 videoId READ videoId NOTIFY frameChanged FINAL)

public:
    explicit HoverSession(QObject* parent = nullptr);
    ~HoverSession() override;

    QString status() const { return m_status; }
    QImage lastFrame() const { return m_lastFrame; }
    qint64 lastFramePtsMs() const { return m_deliveredPtsMs; }
    bool enabled() const { return m_enabled; }
    qint64 videoId() const { return m_videoId; }
    void setEnabled(bool enabled); // Auto (true) or Cached-only (false)

    // Test hooks.
    void setMpvPath(const QString& path) { m_mpvPath = path; }

public slots:
    // Called after the hover dwell; loads the source paused with no output.
    void engage(qint64 videoId, qint64 revision, const QString& absolutePath,
                qint64 durationMs, qint64 initialTimeMs = 0);
    // Pointer target in milliseconds (video-stream duration space).
    void scrub(qint64 timeMs);
    // Pointer exit: show the poster, stop, unload, release handles (§6).
    void disengage();

signals:
    void frameReady(qint64 videoId, qint64 revision, qint64 deliveredPtsMs,
                    const QImage& frame);
    void frameChanged();
    void statusChanged(const QString& status);
    void enabledChanged();

private:
    enum class Backend { None, Mpv };

    void setStatus(const QString& status);
    void engageMpv(const QString& absolutePath);
    void startMpv();
    void loadMpvSource();
    qint64 sendMpvCommand(const QJsonArray& command);
    void readMpvMessages();
    void failMpv();
    void issueSeek();
    void startMpvSeek(qint64 targetMs, bool exact);
    void readMpvFrame(qint64 ptsMs);
    void clearMpvFrames();
    void cleanupMpv();

    QString m_mpvPath;            // resolved once; empty = cached previews only
    Backend m_backend = Backend::None;
    bool m_enabled = true;
    QString m_status = QStringLiteral("idle");

    qint64 m_videoId = 0;
    qint64 m_revision = 0;
    qint64 m_durationMs = -1;
    qint64 m_latestTargetMs = -1;
    qint64 m_deliveredPtsMs = -1;
    QImage m_lastFrame;

    // mpv backend state.
    QProcess* m_mpv = nullptr;
    QLocalSocket* m_ipc = nullptr;
    QTimer* m_settleTimer = nullptr;    // exact seek after the pointer settles
    QTimer* m_connectTimer = nullptr;
    QTimer* m_mpvTimeout = nullptr;
    std::unique_ptr<QTemporaryDir> m_mpvDir;
    QString m_socketPath;
    QString m_pendingPath;
    QByteArray m_ipcBuffer;
    qint64 m_commandId = 0;
    qint64 m_stopRequest = -1;
    qint64 m_loadRequest = -1;
    qint64 m_frameRequest = -1;
    qint64 m_seekRequest = -1;
    qint64 m_expectedFileId = -1;
    qint64 m_playingFileId = -1;
    bool m_mpvLoaded = false;
    bool m_wantExact = false;
    bool m_lastSeekExact = false;
    qint64 m_lastSeekedMs = -1;         // last target actually sent to mpv
    QElapsedTimer m_mpvSeekClock;

    QTimer* m_coalesceTimer = nullptr;
    bool m_seekInFlight = false;

};

} // namespace scrubtub
