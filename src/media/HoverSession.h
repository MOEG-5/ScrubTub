// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Original-file paused scrubbing session (TECH_SPEC.md section 6).
//
// One shared QMediaPlayer + QVideoSink attached to the active hovered card.
// At most one seek in flight plus one replaceable latest target; pointer
// updates coalesced; delivered frames validated by their presentation
// timestamp; sources unloaded on exit. No audio output is ever attached and
// subtitle tracks stay disabled. Milestone-0 evidence: seek accuracy is one
// frame; p95 latency misses the 200 ms gate on the reference machine, so the
// UI keeps cached storyboard feedback in parallel (Auto mode).
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
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
    void setStatus(const QString& status);
    void issueSeek();
    void attachSinkConnections();

    QMediaPlayer* m_player = nullptr;
    QVideoSink* m_sink = nullptr;
    QTimer* m_coalesceTimer = nullptr;

    bool m_enabled = true;
    QString m_status = QStringLiteral("idle");
    qint64 m_videoId = 0;
    qint64 m_revision = 0;
    qint64 m_durationMs = -1;
    qint64 m_latestTargetMs = -1;   // replaceable latest target
    qint64 m_deliveredPtsMs = -1;   // most recent delivered PTS
    bool m_seekInFlight = false;
    int m_overBudgetSeeks = 0;
    QImage m_lastFrame;
    QElapsedTimer m_seekTimer;
};

} // namespace itub
