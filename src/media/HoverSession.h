// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Original-file paused scrubbing session (TECH_SPEC.md section 6).
//
// Preferred backend: one persistent mpv subprocess per engaged card
// (technique after the owner's thumbfast reference), driven over its JSON
// IPC socket, writing the paused frame to an app-owned file that we poll.
// Seeks use keyframe accuracy while the pointer moves and exact accuracy on
// settle; audio and subtitles are disabled; the process group is killed on
// exit. Fallback backend: the shared QMediaPlayer/QVideoSink session.
//
// Measured on the reference machine (docs/BENCHMARKS.md): mpv exact seeks
// update the frame in ~185-260 ms; keyframe seeks are near-instant. The
// QMediaPlayer path measured 250-360 ms per seek (p95 358-377 ms).
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QVideoSink>

class QMediaPlayer;

namespace itub {

class HoverSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)
    Q_PROPERTY(QImage lastFrame READ lastFrame NOTIFY frameChanged FINAL)
    Q_PROPERTY(qint64 lastFramePtsMs READ lastFramePtsMs NOTIFY frameChanged FINAL)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged FINAL)

public:
    explicit HoverSession(QObject* parent = nullptr);
    ~HoverSession() override;

    QString status() const { return m_status; }
    QImage lastFrame() const { return m_lastFrame; }
    qint64 lastFramePtsMs() const { return m_deliveredPtsMs; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled); // Auto (true) or Cached-only (false)

    // Test hooks.
    void setMpvPath(const QString& path) { m_mpvPath = path; }
    void setFramePollMs(int ms) { m_framePollMs = ms; }

public slots:
    // Called after the hover dwell; loads the source paused with no output.
    void engage(qint64 videoId, qint64 revision, const QString& absolutePath,
                qint64 durationMs);
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
    enum class Backend { None, Mpv, QtPlayer };

    void setStatus(const QString& status);
    void engageMpv(const QString& absolutePath);
    void engageQtPlayer(const QString& absolutePath);
    void issueSeek();
    void startMpvSeek(qint64 targetMs, bool exact);
    void pollFrameFile();
    void cleanupMpv();

    QString m_mpvPath;            // resolved once; empty = Qt player backend
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
    QTimer* m_frameTimer = nullptr;     // polls the frame file
    QTimer* m_settleTimer = nullptr;    // exact seek after the pointer settles
    QString m_frameFile;
    QString m_socketPath;
    qint64 m_lastFileStamp = 0;
    qint64 m_lastSeekedMs = -1;         // last target actually sent to mpv

    // Qt player backend state (fallback).
    QMediaPlayer* m_player = nullptr;
    QVideoSink* m_sink = nullptr;
    QTimer* m_coalesceTimer = nullptr;
    bool m_seekInFlight = false;
    int m_overBudgetSeeks = 0;
    QElapsedTimer m_seekTimer;

    int m_framePollMs = 25;
};

} // namespace itub
