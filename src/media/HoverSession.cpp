// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "HoverSession.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <cmath>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

namespace scrubtub {

namespace {
constexpr int kSettleMs = 100;            // pointer idle → exact seek
constexpr int kMpvCoalesceMs = 50;
constexpr int kSeekBudgetMs = 1500;

QString findMpv()
{
    // Cached previews remain available when mpv is missing.
    const QByteArray overridePath = qgetenv("SCRUBTUB_MPV_PATH");
    if (!overridePath.isEmpty())
        return QString::fromLocal8Bit(overridePath);
    return QStandardPaths::findExecutable(QStringLiteral("mpv"));
}
} // namespace

HoverSession::HoverSession(QObject* parent)
    : QObject(parent)
{
    m_mpvPath = findMpv();

    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(kSettleMs);
    connect(m_settleTimer, &QTimer::timeout, this, [this] {
        m_wantExact = true;
        issueSeek();
    });
    m_connectTimer = new QTimer(this);
    m_connectTimer->setInterval(20);
    connect(m_connectTimer, &QTimer::timeout, this, [this] {
        if (m_ipc && m_ipc->state() == QLocalSocket::UnconnectedState)
            m_ipc->connectToServer(m_socketPath);
    });
    m_mpvTimeout = new QTimer(this);
    m_mpvTimeout->setSingleShot(true);
    connect(m_mpvTimeout, &QTimer::timeout, this, &HoverSession::failMpv);

    // Prewarm only the idle process; no source is opened until engagement.
    QTimer::singleShot(0, this, [this] {
        if (m_enabled && !m_mpvPath.isEmpty())
            startMpv();
    });

    m_coalesceTimer = new QTimer(this);
    m_coalesceTimer->setSingleShot(true);
    connect(m_coalesceTimer, &QTimer::timeout, this, &HoverSession::issueSeek);
}

HoverSession::~HoverSession()
{
    cleanupMpv();
}

void HoverSession::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled) {
        disengage();
        cleanupMpv();
    }
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
                          qint64 durationMs, qint64 initialTimeMs)
{
    disengage(); // invalidate old frames before switching source (§6)
    if (!m_enabled || absolutePath.isEmpty()) {
        // Cached-only mode (or no available source): cached feedback carries
        // on; the session never loads the original file (§6).
        setStatus(QStringLiteral("unavailable"));
        return;
    }
    m_videoId = videoId;
    m_revision = revision;
    m_durationMs = durationMs;
    m_latestTargetMs = qBound<qint64>(0, initialTimeMs, qMax<qint64>(0, durationMs - 40));
    m_lastSeekedMs = -1;
    m_deliveredPtsMs = -1;
    m_seekInFlight = false;

    if (!m_mpvPath.isEmpty())
        engageMpv(absolutePath);
    else
        setStatus(QStringLiteral("unavailable"));
}

// ------------------------------- mpv backend --------------------------------

void HoverSession::engageMpv(const QString& absolutePath)
{
    m_backend = Backend::Mpv;
    m_pendingPath = absolutePath;
    m_wantExact = false;
    setStatus(QStringLiteral("loading"));
    m_settleTimer->start();
    startMpv();
    loadMpvSource();
}

void HoverSession::startMpv()
{
    if (m_mpv || m_mpvPath.isEmpty())
        return;
    m_mpvDir = std::make_unique<QTemporaryDir>(
        QDir::tempPath() + QStringLiteral("/scrubtub-hover-XXXXXX"));
    if (!m_mpvDir->isValid()) {
        failMpv();
        return;
    }
#ifdef Q_OS_WIN
    m_socketPath = QStringLiteral("scrubtub-hover-") + QFileInfo(m_mpvDir->path()).fileName();
#else
    m_socketPath = m_mpvDir->filePath(QStringLiteral("ipc.sock"));
#endif
    // Native image VO can unload/reload files in one process. Encoding (--o)
    // cannot reinitialize its video output after stop on mpv 0.41.
    const QString hwdec = qEnvironmentVariable("SCRUBTUB_MPV_HWDEC", QStringLiteral("no"));
    const QStringList args{
        QStringLiteral("--no-config"), QStringLiteral("--really-quiet"),
        QStringLiteral("--no-terminal"), QStringLiteral("--idle=yes"),
        QStringLiteral("--pause=yes"), QStringLiteral("--keep-open=always"),
        QStringLiteral("--load-scripts=no"), QStringLiteral("--osc=no"),
        QStringLiteral("--ytdl=no"), QStringLiteral("--media-controls=no"),
        QStringLiteral("--no-audio"), QStringLiteral("--no-sub"),
        QStringLiteral("--hr-seek=no"),
        QStringLiteral("--input-ipc-server=") + m_socketPath,
        QStringLiteral("--vo=image"), QStringLiteral("--vo-image-format=png"),
        QStringLiteral("--vo-image-png-compression=0"),
        QStringLiteral("--vo-image-png-filter=0"),
        QStringLiteral("--vo-image-outdir=") + m_mpvDir->path(),
        QStringLiteral("--vf=scale=320:320:force_original_aspect_ratio=decrease,format=bgra"),
        QStringLiteral("--sws-scaler=fast-bilinear"), QStringLiteral("--sws-allow-zimg=no"),
        QStringLiteral("--vd-lavc-fast"), QStringLiteral("--vd-lavc-threads=2"),
        QStringLiteral("--vd-lavc-skiploopfilter=all"),
        QStringLiteral("--vd-lavc-software-fallback=1"),
        QStringLiteral("--hwdec=") + hwdec,
        QStringLiteral("--demuxer-readahead-secs=0"),
        QStringLiteral("--demuxer-max-bytes=128KiB")};

    m_ipc = new QLocalSocket(this);
    connect(m_ipc, &QLocalSocket::connected, this, [this] {
        m_connectTimer->stop();
        m_mpvTimeout->stop();
        loadMpvSource();
    });
    connect(m_ipc, &QLocalSocket::readyRead, this, &HoverSession::readMpvMessages);
    connect(m_ipc, &QLocalSocket::disconnected, this, &HoverSession::failMpv);
    m_mpv = new QProcess(this);
#ifdef Q_OS_UNIX
    m_mpv->setChildProcessModifier([] { ::setsid(); });
#endif
    connect(m_mpv, &QProcess::finished, this, [this] { failMpv(); });
    connect(m_mpv, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            failMpv();
    });
    m_connectTimer->start();
    m_mpvTimeout->start(5000);
    m_mpv->start(m_mpvPath, args);
}

qint64 HoverSession::sendMpvCommand(const QJsonArray& command)
{
    if (!m_ipc || m_ipc->state() != QLocalSocket::ConnectedState)
        return -1;
    const qint64 id = ++m_commandId;
    const QByteArray payload = QJsonDocument(QJsonObject{
        {QStringLiteral("command"), command}, {QStringLiteral("request_id"), id}})
        .toJson(QJsonDocument::Compact) + '\n';
    if (m_ipc->write(payload) != payload.size()) {
        QTimer::singleShot(0, this, &HoverSession::failMpv);
        return -1;
    }
    return id;
}

void HoverSession::loadMpvSource()
{
    if (m_backend != Backend::Mpv || m_videoId == 0 || m_stopRequest > 0
        || m_loadRequest > 0 || m_mpvLoaded)
        return;
    // The stop reply is a barrier before starting the newest requested source.
    m_stopRequest = sendMpvCommand({QStringLiteral("stop")});
    if (m_stopRequest > 0)
        m_mpvTimeout->start(5000);
}

void HoverSession::readMpvMessages()
{
    m_ipcBuffer += m_ipc->readAll();
    while (m_ipc && m_ipcBuffer.contains('\n')) {
        const auto newline = m_ipcBuffer.indexOf('\n');
        const auto msg = QJsonDocument::fromJson(m_ipcBuffer.left(newline)).object();
        m_ipcBuffer.remove(0, newline + 1);
        const qint64 id = msg.value(QStringLiteral("request_id")).toInteger(-1);
        const QString event = msg.value(QStringLiteral("event")).toString();
        const bool success = msg.value(QStringLiteral("error")).toString() == QLatin1String("success");
        if (id > 0 && id == m_stopRequest) {
            m_stopRequest = -1;
            m_mpvTimeout->stop();
            if (!success) {
                failMpv();
                return;
            }
            clearMpvFrames();
            if (m_backend != Backend::Mpv || m_videoId == 0)
                continue;
            m_lastSeekedMs = m_latestTargetMs;
            m_lastSeekExact = false;
            m_seekInFlight = true;
            m_mpvSeekClock.restart();
            m_loadRequest = sendMpvCommand({QStringLiteral("loadfile"), m_pendingPath,
                QStringLiteral("replace"), -1,
                QJsonObject{{QStringLiteral("start"), QString::number(m_latestTargetMs / 1000.0, 'f', 3)}}});
            m_mpvTimeout->start(5000);
        } else if (id > 0 && id == m_loadRequest) {
            m_loadRequest = -1;
            if (!success) {
                failMpv();
                return;
            }
            m_expectedFileId = msg.value(QStringLiteral("data")).toObject()
                .value(QStringLiteral("playlist_entry_id")).toInteger(-1);
        } else if (event == QLatin1String("start-file")) {
            m_playingFileId = msg.value(QStringLiteral("playlist_entry_id")).toInteger(-1);
        } else if (event == QLatin1String("file-loaded") && m_expectedFileId >= 0
                   && m_playingFileId == m_expectedFileId) {
            m_mpvLoaded = true;
        } else if (event == QLatin1String("playback-restart") && m_mpvLoaded
                   && m_seekInFlight && m_frameRequest < 0) {
            // Paused, video-only playback has now presented its frame. Hold
            // further seeks until the timestamp and image are consumed together.
            m_frameRequest = sendMpvCommand({QStringLiteral("get_property"), QStringLiteral("time-pos")});
        } else if (id > 0 && id == m_frameRequest) {
            m_frameRequest = -1;
            const auto pts = msg.value(QStringLiteral("data"));
            if (!success || !pts.isDouble() || !std::isfinite(pts.toDouble()) || pts.toDouble() < 0) {
                failMpv();
                return;
            }
            readMpvFrame(qRound64(pts.toDouble() * 1000.0));
        } else if ((id > 0 && id == m_seekRequest && !success)
                   || (event == QLatin1String("end-file") && m_expectedFileId >= 0
                       && msg.value(QStringLiteral("playlist_entry_id")).toInteger(-1) == m_expectedFileId
                       && msg.value(QStringLiteral("reason")).toString() == QLatin1String("error"))) {
            failMpv();
            return;
        }
    }
}

void HoverSession::startMpvSeek(qint64 targetMs, bool exact)
{
    m_seekRequest = sendMpvCommand({QStringLiteral("seek"), targetMs / 1000.0,
        QLatin1String(exact ? "absolute+exact" : "absolute+keyframes")});
    if (m_seekRequest < 0)
        return;
    m_lastSeekedMs = targetMs;
    m_lastSeekExact = exact;
    m_seekInFlight = true;
    m_mpvSeekClock.restart();
    m_mpvTimeout->start(kSeekBudgetMs);
}

void HoverSession::clearMpvFrames()
{
    if (!m_mpvDir)
        return;
    QDir dir(m_mpvDir->path());
    for (const auto& name : dir.entryList({QStringLiteral("*.png")}, QDir::Files))
        dir.remove(name);
}

void HoverSession::readMpvFrame(qint64 ptsMs)
{
    if (m_backend != Backend::Mpv || m_videoId == 0 || !m_mpvDir)
        return;
    const QDir dir(m_mpvDir->path());
    const auto files = dir.entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
    // Image VO finishes writing before playback-restart. Only one decode is
    // in flight, so no subsequent frame can overwrite this image/PTS pair.
    const QImage frame = files.isEmpty() ? QImage() : QImage(dir.filePath(files.last()));
    clearMpvFrames();
    if (frame.isNull() || frame.width() > 320 || frame.height() > 320) {
        failMpv();
        return;
    }
    m_mpvTimeout->stop();
    m_seekInFlight = false;
    // A superseded exact seek must not overwrite feedback for the newer target.
    if (!m_lastSeekExact || m_lastSeekedMs == m_latestTargetMs) {
        m_lastFrame = frame;
        m_deliveredPtsMs = ptsMs;
        setStatus(m_lastSeekedMs == m_latestTargetMs ? QStringLiteral("ready")
                                                   : QStringLiteral("seeking"));
        emit frameChanged();
        emit frameReady(m_videoId, m_revision, ptsMs, frame);
    }
    issueSeek();
}

void HoverSession::failMpv()
{
    const bool active = m_backend == Backend::Mpv && m_videoId != 0;
    const bool startupFailed = m_connectTimer->isActive();
    cleanupMpv();
    if (startupFailed)
        m_mpvPath.clear();
    if (active) {
        m_backend = Backend::None;
        m_lastFrame = QImage();
        m_deliveredPtsMs = -1;
        setStatus(QStringLiteral("unavailable"));
        emit frameChanged();
    }
}

void HoverSession::cleanupMpv()
{
    m_connectTimer->stop();
    m_mpvTimeout->stop();
    m_settleTimer->stop();
    if (m_ipc) {
        disconnect(m_ipc, nullptr, this, nullptr);
        m_ipc->abort();
        m_ipc->deleteLater();
        m_ipc = nullptr;
    }
    if (m_mpv) {
        QProcess* proc = m_mpv;
        m_mpv = nullptr;
        disconnect(proc, nullptr, this, nullptr);
        if (proc->state() != QProcess::NotRunning) {
#ifdef Q_OS_UNIX
            if (proc->processId() > 0)
                ::kill(-proc->processId(), SIGKILL);
#endif
            proc->kill();
            proc->waitForFinished(1000); // shutdown/failure only, never normal hover
        }
        proc->deleteLater();
    }
    m_mpvDir.reset();
    m_ipcBuffer.clear();
    m_socketPath.clear();
    m_stopRequest = m_loadRequest = m_frameRequest = m_seekRequest = -1;
    m_expectedFileId = m_playingFileId = -1;
    m_mpvLoaded = false;
}

void HoverSession::issueSeek()
{
    if (m_backend == Backend::Mpv) {
        if (!m_mpvLoaded || m_seekInFlight || m_videoId == 0)
            return;
        const bool changed = m_latestTargetMs != m_lastSeekedMs;
        if (!changed && (!m_wantExact || m_lastSeekExact))
            return;
        if (m_mpvSeekClock.isValid() && m_mpvSeekClock.elapsed() < kMpvCoalesceMs) {
            if (!m_coalesceTimer->isActive())
                m_coalesceTimer->start(kMpvCoalesceMs - m_mpvSeekClock.elapsed());
            return;
        }
        startMpvSeek(m_latestTargetMs, m_wantExact);
        return;
    }

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
        if (m_latestTargetMs == clamped)
            return;
        m_latestTargetMs = clamped;
        m_wantExact = false;
        setStatus(QStringLiteral("seeking"));
        m_settleTimer->start();
        issueSeek();
        return;
    }

}

void HoverSession::disengage()
{
    m_settleTimer->stop();
    m_coalesceTimer->stop();
    m_backend = Backend::None;
    m_videoId = 0;
    m_revision = 0;
    m_durationMs = -1;
    m_latestTargetMs = -1;
    m_lastSeekedMs = -1;
    m_deliveredPtsMs = -1;
    m_lastFrame = QImage();
    m_seekInFlight = false;
    m_mpvLoaded = false;
    m_expectedFileId = m_playingFileId = -1;
    m_loadRequest = m_frameRequest = m_seekRequest = -1;
    m_pendingPath.clear();
    if (m_ipc && m_ipc->state() == QLocalSocket::ConnectedState) {
        m_stopRequest = sendMpvCommand({QStringLiteral("stop")});
        m_mpvTimeout->start(5000);
    }
    setStatus(QStringLiteral("idle"));
    emit frameChanged();
}

} // namespace scrubtub
