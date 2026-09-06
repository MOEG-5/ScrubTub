// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "HoverSession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QStandardPaths>
#include <QVideoFrame>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

namespace itub {

namespace {
constexpr int kCoalesceIntervalMs = 60;   // §6 proposed default
constexpr int kSettleMs = 220;            // pointer idle → exact seek
constexpr int kFrameScaleWidth = 320;     // card content width bound
constexpr int kSeekBudgetMs = 1500;

QString findMpv()
{
    // mpv is optional; when absent the QMediaPlayer backend is used.
    const QByteArray overridePath = qgetenv("ITUB_MPV_PATH");
    if (!overridePath.isEmpty())
        return QString::fromLocal8Bit(overridePath);
    return QStandardPaths::findExecutable(QStringLiteral("mpv"));
}
} // namespace

HoverSession::HoverSession(QObject* parent)
    : QObject(parent)
{
    m_mpvPath = findMpv();

    // Qt player fallback machinery.
    m_player = new QMediaPlayer(this);
    m_sink = new QVideoSink(this);
    m_player->setVideoSink(m_sink);
    m_coalesceTimer = new QTimer(this);
    m_coalesceTimer->setSingleShot(true);
    m_coalesceTimer->setInterval(kCoalesceIntervalMs);
    connect(m_coalesceTimer, &QTimer::timeout, this, &HoverSession::issueSeek);
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame& frame) {
                if (m_backend != Backend::QtPlayer || m_videoId == 0 || !frame.isValid())
                    return;
                const qint64 startUs = frame.startTime();
                const qint64 deliveredMs =
                    startUs >= 0 ? startUs / 1000 : m_player->position();
                if (m_seekInFlight) {
                    m_seekInFlight = false;
                    if (m_seekTimer.isValid() && m_seekTimer.elapsed() > kSeekBudgetMs) {
                        ++m_overBudgetSeeks;
                        if (m_overBudgetSeeks >= 2)
                            setStatus(QStringLiteral("imprecise"));
                    } else {
                        m_overBudgetSeeks = qMax(0, m_overBudgetSeeks - 1);
                        setStatus(QStringLiteral("ready"));
                    }
                    if (m_latestTargetMs >= 0
                        && qAbs(m_latestTargetMs - deliveredMs) > 100
                        && m_overBudgetSeeks < 2)
                        m_coalesceTimer->start();
                }
                m_deliveredPtsMs = deliveredMs;
                QImage image = frame.toImage();
                if (!image.isNull() && image.width() > kFrameScaleWidth)
                    image = image.scaledToWidth(kFrameScaleWidth, Qt::SmoothTransformation);
                m_lastFrame = image;
                emit frameChanged();
                emit frameReady(m_videoId, m_revision, deliveredMs, image);
            });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (m_backend != Backend::QtPlayer)
                    return;
                if (status == QMediaPlayer::LoadedMedia && m_latestTargetMs >= 0)
                    m_coalesceTimer->start();
            });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& message) {
                if (m_backend == Backend::QtPlayer && m_videoId != 0)
                    setStatus(QStringLiteral("unavailable"));
                qWarning("Qt hover backend error: %s", qUtf8Printable(message));
            });
}

HoverSession::~HoverSession()
{
    cleanupMpv();
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
    m_latestTargetMs = 0;      // the paused first frame
    m_lastSeekedMs = -1;
    m_deliveredPtsMs = -1;
    m_seekInFlight = false;
    m_overBudgetSeeks = 0;

    if (!m_mpvPath.isEmpty())
        engageMpv(absolutePath);
    else
        engageQtPlayer(absolutePath);
}

// ------------------------------- mpv backend --------------------------------

void HoverSession::engageMpv(const QString& absolutePath)
{
    m_backend = Backend::Mpv;
    setStatus(QStringLiteral("loading"));

    const QString sessionDir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/itub-hover");
    QDir().mkpath(sessionDir);
    const QString sessionTag = QStringLiteral("%1-%2-%3")
                                   .arg(m_videoId)
                                   .arg(m_revision)
                                   .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
    m_frameFile = sessionDir + QStringLiteral("/frame-%1.raw").arg(sessionTag);
    m_socketPath = sessionDir + QStringLiteral("/ipc-%1.sock").arg(sessionTag);
    QFile::remove(m_frameFile);
    QFile::remove(m_socketPath);

    // thumbfast-derived invocation: paused, idle, silent, no subtitles or
    // audio, fast software scaling, frame handed over as a raw BGRA file.
    QStringList args{
        QStringLiteral("--no-config"),      QStringLiteral("--really-quiet"),
        QStringLiteral("--no-terminal"),    QStringLiteral("--idle"),
        QStringLiteral("--pause"),          QStringLiteral("--keep-open=always"),
        QStringLiteral("--no-audio"),       QStringLiteral("--no-sub"),
        QStringLiteral("--hr-seek=yes"),    QStringLiteral("--input-ipc-server=") + m_socketPath,
        QStringLiteral("--o=") + m_frameFile,
        QStringLiteral("--ovc=rawvideo"),   QStringLiteral("--of=image2"),
        QStringLiteral("--ofopts=update=1"),
        QStringLiteral("--vf=scale=%1:-4,format=bgra").arg(kFrameScaleWidth),
        QStringLiteral("--sws-scaler=fast-bilinear"),
        QStringLiteral("--vd-lavc-fast"),   QStringLiteral("--vd-lavc-threads=2"),
        absolutePath};

    m_mpv = new QProcess(this);
#ifdef Q_OS_UNIX
    m_mpv->setChildProcessModifier([] { ::setsid(); });
#endif
    m_mpv->start(m_mpvPath, args);
    if (!m_mpv->waitForStarted(10000)) {
        qWarning("mpv hover backend failed to start; falling back to Qt player");
        cleanupMpv();
        engageQtPlayer(absolutePath);
        return;
    }
    connect(m_mpv, &QProcess::finished, this,
            [this, proc = m_mpv](int code, QProcess::ExitStatus status) {
        qWarning("mpv hover backend exited: code=%d crash=%d stderr=%s", code,
                 (int)(status == QProcess::CrashExit),
                 qUtf8Printable(QString::fromUtf8(proc->readAllStandardError().left(300))));
        if (m_backend == Backend::Mpv && m_videoId != 0)
            setStatus(QStringLiteral("unavailable"));
    });

    if (!m_frameTimer) {
        m_frameTimer = new QTimer(this);
        m_frameTimer->setInterval(m_framePollMs);
        connect(m_frameTimer, &QTimer::timeout, this, &HoverSession::pollFrameFile);
    }
    m_frameTimer->start();
    if (!m_settleTimer) {
        m_settleTimer = new QTimer(this);
        m_settleTimer->setSingleShot(true);
        m_settleTimer->setInterval(kSettleMs);
        connect(m_settleTimer, &QTimer::timeout, this, [this] {
            if (m_backend != Backend::Mpv)
                return;
            if (m_latestTargetMs >= 0 && m_latestTargetMs != m_lastSeekedMs) {
                startMpvSeek(m_latestTargetMs, true);
                m_lastSeekedMs = m_latestTargetMs;
            }
        });
    }
}

void HoverSession::startMpvSeek(qint64 targetMs, bool exact)
{
    if (!m_ipc) {
        m_ipc = new QLocalSocket(this);
        m_ipc->connectToServer(m_socketPath);
    }
    if (m_ipc->state() != QLocalSocket::ConnectedState) {
        if (!m_ipc->waitForConnected(2000)) {
            // mpv creates the IPC socket asynchronously after start; a scrub
            // that races creation is retried by the next pointer event or
            // settle tick, but one early lost seek is otherwise invisible.
            qWarning("mpv IPC not ready for seek (%s)",
                     qUtf8Printable(m_ipc->errorString()));
            m_ipc->deleteLater();
            m_ipc = nullptr;
            return;
        }
    }
    // absolute+keyframes while moving (near-instant); absolute+exact on
    // settle. Both honor the paused state.
    // absolute+keyframes while the pointer moves (near-instant);
    // absolute+exact when the pointer settles. Both honor the paused state.
    const QJsonArray args{QStringLiteral("seek"), targetMs / 1000.0,
                          QLatin1String(exact ? "absolute+exact"
                                              : "absolute+keyframes")};
    QByteArray payload = QJsonDocument(
        QJsonObject{{QStringLiteral("command"), args}})
        .toJson(QJsonDocument::Compact);
    payload.append('\n'); // mpv IPC is line-delimited JSON
    const qint64 written = m_ipc->write(payload);
    m_ipc->flush();
}

void HoverSession::pollFrameFile()
{
    if (m_backend != Backend::Mpv || m_videoId == 0)
        return;
    QFileInfo info(m_frameFile);
    if (!info.isFile())
        return;
    const qint64 size = info.size();
    // BGRA frames: size must be a multiple of width*4.
    if (size < kFrameScaleWidth * 4 || size % (kFrameScaleWidth * 4) != 0)
        return;
    const qint64 stamp = info.lastModified().toMSecsSinceEpoch();
    if (stamp == m_lastFileStamp)
        return;
    m_lastFileStamp = stamp;

    QFile file(m_frameFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = file.readAll();
    file.close();
    const int height = static_cast<int>(bytes.size() / (kFrameScaleWidth * 4));
    QImage frame(reinterpret_cast<const uchar*>(bytes.constData()), kFrameScaleWidth,
                 height, kFrameScaleWidth * 4, QImage::Format_ARGB32);
    if (frame.isNull())
        return;
    m_lastFrame = frame.copy(); // detach from the raw buffer
    // The file only changes after a seek completes, so the last seeked
    // target is the delivered frame's time.
    m_deliveredPtsMs = m_lastSeekedMs >= 0 ? m_lastSeekedMs : 0;
    setStatus(QStringLiteral("ready"));
    emit frameChanged();
    emit frameReady(m_videoId, m_revision, m_deliveredPtsMs, m_lastFrame);
}

void HoverSession::cleanupMpv()
{
    if (m_frameTimer)
        m_frameTimer->stop();
    if (m_settleTimer)
        m_settleTimer->stop();
    if (m_ipc) {
        m_ipc->abort();
        m_ipc->deleteLater();
        m_ipc = nullptr;
    }
    if (m_mpv) {
        QProcess* proc = m_mpv;
        m_mpv = nullptr; // the finished() handler must see the new state
        // Intentional shutdown: suppress the unexpected-exit warning.
        disconnect(proc, &QProcess::finished, this, nullptr);
#ifdef Q_OS_UNIX
        ::kill(-proc->processId(), SIGKILL);
#else
        proc->kill();
#endif
        proc->waitForFinished(2000);
        proc->deleteLater();
    }
    if (!m_frameFile.isEmpty())
        QFile::remove(m_frameFile);
    if (!m_socketPath.isEmpty())
        QFile::remove(m_socketPath);
    m_frameFile.clear();
    m_socketPath.clear();
}

// --------------------------- Qt player fallback -----------------------------

void HoverSession::engageQtPlayer(const QString& absolutePath)
{
    m_backend = Backend::QtPlayer;
    setStatus(QStringLiteral("loading"));
    m_player->setActiveAudioTrack(-1);
    m_player->setActiveSubtitleTrack(-1);
    m_player->setSource(QUrl::fromLocalFile(absolutePath));
    m_player->pause();
}

void HoverSession::issueSeek()
{
    if (m_backend != Backend::QtPlayer || m_latestTargetMs < 0 || m_videoId == 0)
        return;
    if (m_seekInFlight)
        return;
    if (m_overBudgetSeeks >= 2) {
        setStatus(QStringLiteral("imprecise"));
        return;
    }
    m_seekInFlight = true;
    m_seekTimer.restart();
    setStatus(QStringLiteral("seeking"));
    m_player->setPosition(m_latestTargetMs);
}

// ------------------------------- common paths -------------------------------

void HoverSession::scrub(qint64 timeMs)
{
    if (!m_enabled || m_videoId == 0 || m_durationMs <= 0)
        return;
    // Clamp into the selected video stream's duration; at EOF request the
    // last representable frame rather than beyond the stream (§6).
    const qint64 clamped = qBound<qint64>(0, timeMs, qMax<qint64>(0, m_durationMs - 40));

    if (m_backend == Backend::Mpv) {
        // While the pointer moves, the cached storyboard tiles carry the
        // feedback; the mpv session delivers the exact frame once the
        // pointer settles. (Keyframe-accurate seeks do not flush mpv's
        // paused encoder — measured, so exact-on-settle is the whole flow.)
        if (m_latestTargetMs != clamped)
            setStatus(QStringLiteral("seeking"));
        m_latestTargetMs = clamped;
        m_settleTimer->start(); // restarts while the pointer keeps moving
        return;
    }
    m_latestTargetMs = clamped;
    if (m_player->mediaStatus() == QMediaPlayer::LoadingMedia) {
        setStatus(QStringLiteral("loading"));
        return;
    }
    if (!m_seekInFlight)
        m_coalesceTimer->start();
}

void HoverSession::disengage()
{
    if (m_backend == Backend::Mpv)
        cleanupMpv();
    m_backend = Backend::None;
    if (m_videoId == 0)
        return;
    m_coalesceTimer->stop();
    m_videoId = 0;
    m_revision = 0;
    m_durationMs = -1;
    m_latestTargetMs = -1;
    m_lastSeekedMs = -1;
    m_seekInFlight = false;
    m_player->setSource(QUrl());
    setStatus(QStringLiteral("idle"));
}

} // namespace itub
