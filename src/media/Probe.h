// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// ffprobe runner (TECH_SPEC.md sections 3, 5): argument-array invocation,
// no shell, -nostdin, bounded output, video-stream selection with the
// attached-picture exclusion, video-stream duration first with a documented
// container fallback, display dimensions after rotation.
#pragma once

#include <QString>

#include <atomic>
#include <functional>

namespace scrubtub {

// Registered with the process's PID at spawn (0 when done) so the catalogue
// can kill process trees promptly on cancellation (TECH_SPEC.md §11).
using PidSink = std::function<void(qint64)>;

struct ProbeResult {
    enum class FailKind { None, BadMedia, TransientIo, Timeout };

    bool ok = false;
    FailKind failKind = FailKind::None;
    QString error;               // bounded human-readable reason on failure

    qint64 durationMs = -1;      // -1 = unknown (stored NULL)
    int codedWidth = 0;
    int codedHeight = 0;
    int displayWidth = 0;        // display-oriented dimensions after rotation
    int displayHeight = 0;
    int rotationDeg = 0;         // normalized to 0/90/180/270
    QString codec;
    int selectedStreamIndex = -1;
};

class Probe {
public:
    // Runs ffprobe with a hard timeout. The source file is only read.
    // Default timeout: 30 s (configurable for slow media, TECH_SPEC.md §5).
    static ProbeResult run(const QString& ffprobePath, const QString& filePath,
                           int timeoutMs = 30000,
                           const PidSink& pidSink = {},
                           const std::atomic_bool* cancelled = nullptr);

    // Argument list for -show_streams/-show_format JSON output; shared by the
    // blocking runner and the asynchronous catalogue job runner.
    static QStringList arguments(const QString& filePath);

    // Parses ffprobe JSON output (exposed for table-driven tests).
    static ProbeResult parse(const QByteArray& json);
};

} // namespace scrubtub
