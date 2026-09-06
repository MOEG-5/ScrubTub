// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "Probe.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>

#include <cmath>
#include <utility>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace itub {

namespace {
constexpr int kMaxStderrBytes = 64 * 1024; // TECH_SPEC.md section 5
constexpr int kMaxOutputBytes = 8 * 1024 * 1024;

ProbeResult::FailKind failureKindForExit(int exitCode, QProcess::ExitStatus status)
{
    if (status == QProcess::CrashExit)
        return ProbeResult::FailKind::TransientIo; // one automatic retry (§5)
    if (exitCode != 0)
        return ProbeResult::FailKind::BadMedia;
    return ProbeResult::FailKind::None;
}

int normalizedRotation(double raw)
{
    int deg = static_cast<int>(std::lround(std::fabs(raw))) % 360;
    if (deg > 45 && deg < 135)
        return 90;
    if (deg >= 135 && deg <= 225)
        return 180;
    if (deg > 225 && deg < 315)
        return 270;
    return 0;
}

// ffprobe emits most numeric fields as strings ("667.500000"); accept both
// strings and JSON numbers.
double jsonDouble(const QJsonValue& value, double fallback)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}
} // namespace

QStringList Probe::arguments(const QString& filePath)
{
    // Local file only, no logging noise, JSON output. (ffprobe reads no stdin;
    // the -nostdin requirement in TECH_SPEC.md §3 applies to the ffmpeg
    // extraction commands, where the flag is valid.)
    return {QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-print_format"), QStringLiteral("json"),
            QStringLiteral("-show_streams"), QStringLiteral("-show_format"),
            filePath};
}

ProbeResult Probe::run(const QString& ffprobePath, const QString& filePath, int timeoutMs,
                       const PidSink& pidSink, const std::atomic_bool* cancelled)
{
    ProbeResult result;
    if (cancelled && cancelled->load()) {
        result.failKind = ProbeResult::FailKind::TransientIo;
        result.error = QStringLiteral("cancelled before start");
        return result;
    }
    QProcess process;
    const QStringList args = arguments(filePath);

#ifdef Q_OS_UNIX
    // Own process group so a timeout kills the whole tree, not just the child.
    process.setChildProcessModifier([] { ::setsid(); });
#endif
    process.start(ffprobePath, args);
    if (pidSink)
        pidSink(process.processId());
    if (!process.waitForStarted(10000)) {
        if (pidSink)
            pidSink(0);
        result.failKind = ProbeResult::FailKind::TransientIo;
        result.error = QStringLiteral("could not start ffprobe: %1")
                           .arg(process.errorString());
        return result;
    }
    if (!process.waitForFinished(timeoutMs)) {
        if (pidSink)
            pidSink(0);
#ifdef Q_OS_UNIX
        ::kill(-process.processId(), SIGTERM);
#else
        process.terminate();
#endif
        if (!process.waitForFinished(2000)) {
#ifdef Q_OS_UNIX
            ::kill(-process.processId(), SIGKILL);
#else
            process.kill();
#endif
            process.waitForFinished(2000);
        }
        result.failKind = ProbeResult::FailKind::Timeout;
        result.error = QStringLiteral("ffprobe timed out after %1 ms").arg(timeoutMs);
        return result;
    }

    if (pidSink)
        pidSink(0);
    const QByteArray out = process.readAllStandardOutput();
    const QByteArray err = process.readAllStandardError().left(kMaxStderrBytes);
    const auto failKind =
        failureKindForExit(process.exitCode(), process.exitStatus());
    if (failKind != ProbeResult::FailKind::None) {
        result.failKind = failKind;
        result.error = QStringLiteral("ffprobe exit %1: %2")
                           .arg(process.exitCode())
                           .arg(QString::fromUtf8(err.left(2000)));
        return result;
    }
    if (out.size() > kMaxOutputBytes) {
        result.failKind = ProbeResult::FailKind::BadMedia;
        result.error = QStringLiteral("ffprobe output exceeded bound");
        return result;
    }

    result = parse(out);
    if (!result.ok && result.error.isEmpty())
        result.error = QStringLiteral("unparseable ffprobe output");
    return result;
}

ProbeResult Probe::parse(const QByteArray& json)
{
    ProbeResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.failKind = ProbeResult::FailKind::BadMedia;
        result.error = QStringLiteral("ffprobe JSON parse failure");
        return result;
    }
    const QJsonObject root = doc.object();
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();

    // Select the first real video stream: codec_type=video that is not an
    // attached picture (cover art must not become the preview source).
    for (const QJsonValue& v : streams) {
        const QJsonObject st = v.toObject();
        if (st.value(QStringLiteral("codec_type")).toString()
            != QLatin1String("video"))
            continue;
        const QJsonObject disposition =
            st.value(QStringLiteral("disposition")).toObject();
        if (disposition.value(QStringLiteral("attached_pic")).toInt(0) == 1)
            continue;
        result.selectedStreamIndex = st.value(QStringLiteral("index")).toInt(-1);
        result.codec = st.value(QStringLiteral("codec_name")).toString();
        result.codedWidth = static_cast<int>(jsonDouble(st.value(QStringLiteral("width")), 0));
        result.codedHeight = static_cast<int>(jsonDouble(st.value(QStringLiteral("height")), 0));
        break;
    }
    if (result.selectedStreamIndex < 0) {
        result.failKind = ProbeResult::FailKind::BadMedia;
        result.error = QStringLiteral("no video stream");
        return result;
    }

    // Duration: selected stream's positive finite duration first, then the
    // documented container fallback, else unknown. Subtitle streams are
    // never consulted (the fixture's mov_text stream is longer by design).
    const QJsonObject selected =
        streams.at(result.selectedStreamIndex).toObject();
    double durationSec = jsonDouble(selected.value(QStringLiteral("duration")), -1);
    if (!(durationSec > 0)) {
        durationSec = jsonDouble(
            root.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")),
            -1);
    }
    if (durationSec > 0)
        result.durationMs = static_cast<qint64>(std::llround(durationSec * 1000.0));

    // Rotation from the display matrix; ±90/270 swap the display dimensions.
    const QJsonArray sideData = selected.value(QStringLiteral("side_data_list")).toArray();
    for (const QJsonValue& v : sideData) {
        const QJsonObject sd = v.toObject();
        if (sd.contains(QStringLiteral("rotation"))) {
            result.rotationDeg = normalizedRotation(
                jsonDouble(sd.value(QStringLiteral("rotation")), 0));
            break;
        }
    }
    result.displayWidth = result.codedWidth;
    result.displayHeight = result.codedHeight;
    if (result.rotationDeg == 90 || result.rotationDeg == 270)
        std::swap(result.displayWidth, result.displayHeight);

    // Reject non-positive known dimensions (constraint mirrors the schema).
    if ((result.codedWidth > 0) != (result.codedHeight > 0)
        || result.codedWidth < 0 || result.codedHeight < 0) {
        result.failKind = ProbeResult::FailKind::BadMedia;
        result.error = QStringLiteral("invalid stream dimensions");
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace itub
