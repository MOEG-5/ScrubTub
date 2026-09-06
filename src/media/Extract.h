// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Poster and storyboard extraction (TECH_SPEC.md section 6): seek-based,
// selected-stream only, actual delivered timestamps recorded, bounded logs,
// app-owned temp destinations, source files only ever read.
#pragma once

#include <QSize>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

#include "Probe.h"

namespace itub {

struct ExtractResult {
    bool ok = false;
    QString error;                 // bounded
    bool timedOut = false;
    QString outputPath;            // app-owned artifact
    QVector<qint64> sampleTimesMs; // actual delivered timestamps (storyboards)
    QSize pixelSize;
};

struct PosterRequest {
    QString ffmpegPath;
    QString sourcePath;
    QString outputPath;      // app-owned destination (not inside a media root)
    qint64 durationMs = -1;  // selected-video-stream duration; -1 unknown
    int selectedStreamIndex = 0;
    int targetWidth = 320;
    int timeoutMs = 60000;   // §5: 60 s per-preview-job default
};

struct StoryboardRequest {
    QString ffmpegPath;
    QString sourcePath;
    QString outputDir;       // app-owned; per-sample frames are written here
    QString outputPath;      // atlas JPEG destination
    qint64 durationMs = -1;
    int selectedStreamIndex = 0;
    int sampleCount = 24;
    int tileWidth = 320;
    int columns = 6;
    int timeoutMs = 60000;
};

class Extract {
public:
    // One frame near 10 % of the duration (first decodable frame when the
    // duration is unknown or the clip is very short). Returns false with a
    // bounded reason on failure; the source file is only read.
    static ExtractResult poster(const PosterRequest& request,
                                const PidSink& pidSink = {},
                                const std::atomic_bool* cancelled = nullptr);

    // Evenly spaced samples across the video, composed into a JPEG atlas.
    // Records the actual timestamps (showinfo pts_time), not the requested
    // ones. Short clips yield fewer samples.
    static ExtractResult storyboard(const StoryboardRequest& request,
                                    const PidSink& pidSink = {},
                                    const std::atomic_bool* cancelled = nullptr);

    // Requested sample plan for a duration; exposed for tests.
    static QVector<qint64> samplePlanMs(qint64 durationMs, int sampleCount);
};

} // namespace itub
