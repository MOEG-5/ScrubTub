// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "HoverSession.h"

#include <QMediaPlayer>
#include <QVideoFrame>

namespace itub {

namespace {
constexpr int kDwellHandledInQml = 0;     // QML starts the session after dwell
constexpr int kCoalesceIntervalMs = 60;   // §6 proposed default
constexpr int kSeekBudgetMs = 1500;       // beyond this: stop hammering slow media
constexpr int kMaxOverBudgetSeeks = 2;    // then precise seeking is disabled
constexpr int kFrameScaleWidth = 320;     // card content width bound
} // namespace

HoverSession::HoverSession(QObject* parent)
    : QObject(parent)
{
    m_player = new QMediaPlayer(this);
    m_sink = new QVideoSink(this);
    m_player->setVideoSink(m_sink);
    // No QAudioOutput is attached: the paused session is silent; audio and
    // subtitle tracks are disabled when a source engages (§6).

    m_coalesceTimer = new QTimer(this);
    m_coalesceTimer->setSingleShot(true);
    m_coalesceTimer->setInterval(kCoalesceIntervalMs);
    connect(m_coalesceTimer, &QTimer::timeout, this, &HoverSession::issueSeek);
    attachSinkConnections();
}

HoverSession::~HoverSession()
{
    if (m_player)
        m_player->setSource(QUrl());
}

void HoverSession::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled)
        disengage();
    emit enabledChanged();
}

void HoverSession::setStatus(const QString& status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged(status);
}

void HoverSession::engage(qint64 videoId, qint64 revision, const QString& absolutePath,
                          qint64 durationMs)
{
    if (!m_enabled || absolutePath.isEmpty()) {
        // Cached-only mode (or no available source): cached feedback carries
        // on; the session never loads the original file (§6).
        setStatus(QStringLiteral("unavailable"));
        return;
    }
    disengage(); // invalidate old frames before switching source (§6)
    m_videoId = videoId;
    m_revision = revision;
    m_durationMs = durationMs;
    m_latestTargetMs = -1;
    m_deliveredPtsMs = -1;
    m_seekInFlight = false;
    m_overBudgetSeeks = 0;

    setStatus(QStringLiteral("loading"));
    m_player->setActiveAudioTrack(-1);
    m_player->setActiveSubtitleTrack(-1);
    m_player->setSource(QUrl::fromLocalFile(absolutePath));
    // Enter paused state: the backend initializes decoding with no audible
    // output and no autonomous playback (§6).
    m_player->pause();
}

void HoverSession::scrub(qint64 timeMs)
{
    if (!m_enabled || m_videoId == 0 || m_durationMs <= 0)
        return;
    // Clamp into the selected video stream's duration; at EOF request the
    // last representable frame rather than beyond the stream (§6).
    const qint64 clamped = qBound<qint64>(0, timeMs, qMax<qint64>(0, m_durationMs - 40));
    m_latestTargetMs = clamped;
    if (m_player->mediaStatus() == QMediaPlayer::LoadingMedia) {
        setStatus(QStringLiteral("loading"));
        return;
    }
    if (!m_seekInFlight) {
        m_coalesceTimer->start(); // coalesce rapid pointer updates (§6)
    }
}

void HoverSession::issueSeek()
{
    if (m_latestTargetMs < 0 || m_videoId == 0)
        return;
    if (m_seekInFlight)
        return; // one seek in flight plus one replaceable target (§6)
    if (m_overBudgetSeeks >= kMaxOverBudgetSeeks) {
        setStatus(QStringLiteral("imprecise"));
        return; // cached samples carry on; do not hammer slow media (§6)
    }
    m_seekInFlight = true;
    m_seekTimer.restart();
    setStatus(QStringLiteral("seeking"));
    m_player->setPosition(m_latestTargetMs);
}

void HoverSession::attachSinkConnections()
{
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame& frame) {
                if (m_videoId == 0 || !frame.isValid())
                    return;
                // Validate by the delivered frame's own timestamps, not just
                // the player position (§6, milestone-0 finding).
                const qint64 startUs = frame.startTime();
                const qint64 deliveredMs =
                    startUs >= 0 ? startUs / 1000 : m_player->position();

                if (m_seekInFlight) {
                    m_seekInFlight = false;
                    if (m_seekTimer.isValid()
                        && m_seekTimer.elapsed() > kSeekBudgetMs) {
                        ++m_overBudgetSeeks;
                        if (m_overBudgetSeeks >= kMaxOverBudgetSeeks)
                            setStatus(QStringLiteral("imprecise"));
                    } else {
                        m_overBudgetSeeks = qMax(0, m_overBudgetSeeks - 1);
                        setStatus(QStringLiteral("ready"));
                    }
                    // A completed old seek must not overwrite a newer target:
                    // schedule the pending one immediately.
                    if (m_latestTargetMs >= 0
                        && qAbs(m_latestTargetMs - deliveredMs) > 100
                        && m_overBudgetSeeks < kMaxOverBudgetSeeks) {
                        m_coalesceTimer->start();
                    }
                }

                m_deliveredPtsMs = deliveredMs;
                QImage image = frame.toImage();
                if (!image.isNull() && image.width() > kFrameScaleWidth)
                    image = image.scaledToWidth(kFrameScaleWidth, Qt::SmoothTransformation);
                m_lastFrame = image;
                emit frameChanged();
                emit frameReady(m_videoId, m_revision, deliveredMs, image);
            });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && m_latestTargetMs >= 0)
            m_coalesceTimer->start();
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error,
                                                                 const QString& message) {
        if (m_videoId != 0)
            setStatus(QStringLiteral("unavailable"));
        qWarning("Hover session error: %s", qUtf8Printable(message));
    });
}

void HoverSession::disengage()
{
    if (m_videoId == 0)
        return;
    m_coalesceTimer->stop();
    m_videoId = 0;
    m_revision = 0;
    m_durationMs = -1;
    m_latestTargetMs = -1;
    m_seekInFlight = false;
    // Release file handles promptly (§6): unload the source entirely.
    m_player->setSource(QUrl());
    setStatus(QStringLiteral("idle"));
}

} // namespace itub
