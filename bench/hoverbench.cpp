// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Milestone-0 benchmark: one shared QMediaPlayer + QVideoSink performing
// paused seeks on a supplied media file (TECH_SPEC.md sections 6 and 11).
//
// The benchmark answers the milestone-0 feasibility question: can the native
// paused hover session deliver a suitably timed frame within the latency and
// accuracy gates? It records requested time versus the delivered frame's
// presentation timestamp (QVideoFrame::startTime/endTime, microseconds) and
// wall-clock latency, plus a pointer-like coalescing burst and an idle check.
//
// Exit code 0 = gates met on this run, 2 = gates missed, 1 = setup error.
// Results are aggregate rows for the supplied fixture only; no catalogue or
// private-library data is involved.
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

#include <QThread>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

struct FrameRecord {
    qint64 arrivalUs = 0;   // steady-clock microseconds at delivery
    qint64 startUs = -1;    // QVideoFrame::startTime (PTS, microseconds)
    qint64 endUs = -1;      // QVideoFrame::endTime
    QSize size;
    bool valid = false;
};

class FrameSink : public QObject {
public:
    explicit FrameSink(QVideoSink* sink) : QObject(sink)
    {
        connect(sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
            records.push_back({nowUs(), frame.startTime(), frame.endTime(), frame.size(),
                               frame.isValid()});
        });
    }
    std::vector<FrameRecord> records;
    void clear() { records.clear(); }
    static qint64 nowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

// Waits for predicate() to become true, processing events, bounded by timeoutMs.
bool waitFor(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty())
        return -1.0;
    std::sort(values.begin(), values.end());
    const double pos = fraction * (values.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(pos));
    const size_t upper = static_cast<size_t>(std::ceil(pos));
    const double t = pos - static_cast<double>(lower);
    return values[lower] * (1.0 - t) + values[upper] * t;
}

qint64 residentMemoryKb()
{
#if defined(Q_OS_UNIX)
    QFile stat(QStringLiteral("/proc/self/statm"));
    if (stat.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> fields = stat.readAll().split(' ');
        if (fields.size() >= 2)
            return fields.at(1).toLongLong() * 4; // pages of 4 KiB (x86_64)
    }
#endif
    return -1;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("itub-hoverbench"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Paused-player seek benchmark: one QMediaPlayer/QVideoSink, "
                       "requested time vs delivered frame PTS and latency."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("source"),
                                 QStringLiteral("Absolute path to a local video file"));
    QCommandLineOption targetsOpt(
        QStringLiteral("targets"),
        QStringLiteral("Comma-separated seek targets in seconds (default 10,300,650,123.456,500)"),
        QStringLiteral("list"));
    QCommandLineOption repeatsOpt(QStringLiteral("repeats"),
                                  QStringLiteral("Repetitions of the target set"), QStringLiteral("n"),
                                  QStringLiteral("1"));
    QCommandLineOption dwellOpt(QStringLiteral("dwell-ms"),
                                QStringLiteral("Hover dwell before the first seek (default 0)"),
                                QStringLiteral("ms"), QStringLiteral("0"));
    QCommandLineOption coalesceOpt(QStringLiteral("coalesce-burst"),
                                   QStringLiteral("Number of pointer-like seeks in the coalescing burst"),
                                   QStringLiteral("n"), QStringLiteral("12"));
    QCommandLineOption burstIntervalOpt(QStringLiteral("burst-interval-ms"),
                                        QStringLiteral("Interval between burst seeks"),
                                        QStringLiteral("ms"), QStringLiteral("60"));
    QCommandLineOption outOpt(QStringLiteral("out"), QStringLiteral("Write JSON results to this path"),
                              QStringLiteral("file"));
    QCommandLineOption accuracyOpt(QStringLiteral("accuracy-ms"),
                                   QStringLiteral("Seek accuracy gate in milliseconds"),
                                   QStringLiteral("ms"), QStringLiteral("100"));
    QCommandLineOption latencyOpt(QStringLiteral("latency-ms"),
                                  QStringLiteral("p95 latency gate in milliseconds"),
                                  QStringLiteral("ms"), QStringLiteral("200"));
    parser.addOption(targetsOpt);
    parser.addOption(repeatsOpt);
    parser.addOption(dwellOpt);
    parser.addOption(coalesceOpt);
    parser.addOption(burstIntervalOpt);
    parser.addOption(outOpt);
    parser.addOption(accuracyOpt);
    parser.addOption(latencyOpt);
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 1) {
        qCritical("Usage: hoverbench SOURCE [--targets seconds,...] [options]");
        return 1;
    }
    const QString sourcePath = positional.first();
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile() || !sourceInfo.isReadable()) {
        qCritical("Source is not a readable file: %s", qUtf8Printable(sourcePath));
        return 1;
    }

    std::vector<double> targets;
    {
        const QStringList raw = parser.value(targetsOpt).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& token : raw) {
            bool ok = false;
            const double seconds = token.toDouble(&ok);
            if (!ok || seconds < 0) {
                qCritical("Invalid target: %s", qUtf8Printable(token));
                return 1;
            }
            targets.push_back(seconds);
        }
        if (targets.empty())
            targets = {10.0, 300.0, 650.0, 123.456, 500.0};
    }
    const int repeats = qMax(1, parser.value(repeatsOpt).toInt());
    const int dwellMs = parser.value(dwellOpt).toInt();
    const int burstCount = qMax(2, parser.value(coalesceOpt).toInt());
    const int burstIntervalMs = qMax(10, parser.value(burstIntervalOpt).toInt());
    const double accuracyGateMs = parser.value(accuracyOpt).toDouble();
    const double latencyGateMs = parser.value(latencyOpt).toDouble();

    QJsonObject report;
    report[QStringLiteral("tool")] = QStringLiteral("itub-hoverbench");
    report[QStringLiteral("version")] = app.applicationVersion();
    report[QStringLiteral("qt")] = QT_VERSION_STR;
    report[QStringLiteral("generated")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    // Record only the file name; result sharing must stay free of private paths.
    report[QStringLiteral("source_file")] = sourceInfo.fileName();
    report[QStringLiteral("source_bytes")] = static_cast<qint64>(sourceInfo.size());

    QMediaPlayer player;
    QVideoSink sink;
    player.setVideoSink(&sink);
    // No QAudioOutput is attached: the paused session must be silent, and
    // audio/subtitle tracks stay disabled (TECH_SPEC.md section 6).
    FrameSink frames(&sink);

    player.setSource(QUrl::fromLocalFile(sourceInfo.absoluteFilePath()));
    if (!waitFor([&] { return player.mediaStatus() != QMediaPlayer::LoadingMedia; }, 30000)) {
        qCritical("Timed out loading media");
        return 1;
    }
    if (player.mediaStatus() == QMediaPlayer::InvalidMedia) {
        qCritical("Media is invalid: %s", qUtf8Printable(player.errorString()));
        return 1;
    }

    QJsonObject media;
    media[QStringLiteral("durationMs")] = player.duration();
    media[QStringLiteral("seekable")] = player.isSeekable();
    media[QStringLiteral("videoTracks")] = static_cast<int>(player.videoTracks().size());
    media[QStringLiteral("audioTracks")] = static_cast<int>(player.audioTracks().size());
    media[QStringLiteral("subtitleTracks")] = static_cast<int>(player.subtitleTracks().size());
    media[QStringLiteral("activeVideoTrack")] = player.activeVideoTrack();
    report[QStringLiteral("media")] = media;
    printf("Loaded %s: duration=%lld ms seekable=%d videoTracks=%d audioTracks=%d\n",
           qUtf8Printable(sourceInfo.fileName()),
           static_cast<long long>(player.duration()), player.isSeekable(),
           static_cast<int>(player.videoTracks().size()),
           static_cast<int>(player.audioTracks().size()));

    if (!player.isSeekable()) {
        qCritical("Media is not seekable; the paused-hover gate cannot be evaluated");
        report[QStringLiteral("error")] = QStringLiteral("not seekable");
        return 1;
    }

    // Enter paused playback so the backend holds a decoder and delivers frames.
    frames.clear();
    player.pause();
    if (!waitFor([&] {
            return player.playbackState() == QMediaPlayer::PlaybackState::PausedState
                && !frames.records.empty();
        }, 30000)) {
        qCritical("Player did not reach paused state with a first frame");
        return 1;
    }
    const qint64 initialPts = frames.records.back().startUs;

    QJsonArray seekRows;
    std::vector<double> settledLatencies;
    std::vector<double> firstLatencies;
    std::vector<double> deltas;
    int ptsMissing = 0;

    QThread::msleep(static_cast<unsigned long>(qMax(0, dwellMs)));

    for (int rep = 0; rep < repeats; ++rep) {
        for (const double targetSeconds : targets) {
            const qint64 targetUs = static_cast<qint64>(targetSeconds * 1e6);
            frames.clear();
            const qint64 t0 = FrameSink::nowUs();
            player.setPosition(static_cast<qint64>(targetSeconds * 1000.0));

            // Settled frame: the last frame delivered within a bounded window
            // after the seek, when delivery has stopped for 120 ms.
            const bool anyDelivery = waitFor([&] { return !frames.records.empty(); }, 30000);
            qint64 tFirst = -1;
            if (anyDelivery)
                tFirst = frames.records.front().arrivalUs;
            waitFor([&] {
                return !frames.records.empty()
                    && FrameSink::nowUs() - frames.records.back().arrivalUs > 120000;
            }, 30000);

            QJsonObject row;
            row[QStringLiteral("target_s")] = targetSeconds;
            row[QStringLiteral("requested_us")] = targetUs;
            row[QStringLiteral("deliveries")] = static_cast<int>(frames.records.size());
            if (!anyDelivery || frames.records.empty()) {
                row[QStringLiteral("timeout")] = true;
                seekRows.append(row);
                continue;
            }
            const FrameRecord& first = frames.records.front();
            const FrameRecord& settled = frames.records.back();
            if (tFirst > 0)
                firstLatencies.push_back((tFirst - t0) / 1000.0);
            settledLatencies.push_back((settled.arrivalUs - t0) / 1000.0);
            if (settled.startUs < 0) {
                ++ptsMissing;
            } else {
                const double deltaMs = (settled.startUs - targetUs) / 1000.0;
                deltas.push_back(deltaMs);
                row[QStringLiteral("settled_pts_us")] = settled.startUs;
                row[QStringLiteral("settled_end_us")] = settled.endUs;
                row[QStringLiteral("delta_ms")] = deltaMs;
                row[QStringLiteral("delta_abs_ms")] = std::abs(deltaMs);
            }
            if (first.startUs >= 0) {
                row[QStringLiteral("first_pts_us")] = first.startUs;
                row[QStringLiteral("first_delta_ms")] = (first.startUs - targetUs) / 1000.0;
            }
            row[QStringLiteral("first_latency_ms")] =
                tFirst > 0 ? (tFirst - t0) / 1000.0 : -1.0;
            row[QStringLiteral("settled_latency_ms")] =
                settledLatencies.back();
            row[QStringLiteral("frame_size")] =
                QStringLiteral("%1x%2").arg(settled.size.width()).arg(settled.size.height());
            row[QStringLiteral("pts_missing")] = settled.startUs < 0;
            seekRows.append(row);
        }
    }

    // Coalescing burst: pointer-like rapid seeks; one seek should be in flight
    // plus a pending target, not one decoder per event (TECH_SPEC.md section 6).
    frames.clear();
    const qint64 burstT0 = FrameSink::nowUs();
    for (int i = 0; i < burstCount; ++i) {
        const qint64 positionMs =
            qBound<qint64>(0, static_cast<qint64>(player.duration() * (i + 1.0) / (burstCount + 1.0)),
                           player.duration() - 1);
        player.setPosition(positionMs);
        QThread::msleep(static_cast<unsigned long>(burstIntervalMs));
    }
    waitFor([&] {
        return !frames.records.empty()
            && FrameSink::nowUs() - frames.records.back().arrivalUs > 250000;
    }, 30000);
    QJsonObject burst;
    burst[QStringLiteral("requests")] = burstCount;
    burst[QStringLiteral("deliveries")] = static_cast<int>(frames.records.size());
    burst[QStringLiteral("wall_ms")] = (FrameSink::nowUs() - burstT0) / 1000.0;
    report[QStringLiteral("coalescing_burst")] = burst;

    // Idle check: a paused session must not keep delivering frames or work.
    frames.clear();
    QThread::msleep(2000);
    QJsonObject idle;
    idle[QStringLiteral("frames_in_2s")] = static_cast<int>(frames.records.size());
    idle[QStringLiteral("rss_kb")] = residentMemoryKb();
    report[QStringLiteral("idle")] = idle;

    QJsonObject summary;
    summary[QStringLiteral("seeks")] = static_cast<int>(settledLatencies.size());
    summary[QStringLiteral("first_latency_p50_ms")] = percentile(firstLatencies, 0.50);
    summary[QStringLiteral("first_latency_p95_ms")] = percentile(firstLatencies, 0.95);
    summary[QStringLiteral("settled_latency_p50_ms")] = percentile(settledLatencies, 0.50);
    summary[QStringLiteral("settled_latency_p95_ms")] = percentile(settledLatencies, 0.95);
    double maxAbsDelta = -1.0;
    for (const double d : deltas)
        maxAbsDelta = qMax(maxAbsDelta, std::abs(d));
    summary[QStringLiteral("max_abs_delta_ms")] = maxAbsDelta;
    summary[QStringLiteral("pts_missing")] = ptsMissing;
    summary[QStringLiteral("initial_pts_us")] = initialPts;
    const bool latencyGate = percentile(settledLatencies, 0.95) <= latencyGateMs;
    const bool accuracyGate = maxAbsDelta >= 0 && maxAbsDelta <= accuracyGateMs;
    summary[QStringLiteral("latency_gate_ms")] = latencyGateMs;
    summary[QStringLiteral("accuracy_gate_ms")] = accuracyGateMs;
    summary[QStringLiteral("latency_gate_pass")] = latencyGate;
    summary[QStringLiteral("accuracy_gate_pass")] = accuracyGate;
    summary[QStringLiteral("gates_pass")] = latencyGate && accuracyGate && ptsMissing == 0;
    report[QStringLiteral("summary")] = summary;
    report[QStringLiteral("seeks")] = seekRows;

    const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    const QString outPath = parser.value(outOpt);
    if (outPath.isEmpty()) {
        fputs(json.constData(), stdout);
        fputc('\n', stdout);
    } else {
        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qCritical("Cannot write results to %s", qUtf8Printable(outPath));
            return 1;
        }
        out.write(json);
        printf("Results written to %s\n", qUtf8Printable(outPath));
    }

    printf("Summary: settled p50=%.1f ms p95=%.1f ms max|delta|=%.1f ms ptsMissing=%d gates=%s\n",
           summary[QStringLiteral("settled_latency_p50_ms")].toDouble(),
           summary[QStringLiteral("settled_latency_p95_ms")].toDouble(), maxAbsDelta,
           ptsMissing, summary[QStringLiteral("gates_pass")].toBool() ? "PASS" : "FAIL");

    return summary[QStringLiteral("gates_pass")].toBool() ? 0 : 2;
}
