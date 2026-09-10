// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Test media access per AGENTS.md: read-only suites scan the repository's
// vids/ folder as-is; cases that must mutate media synthesize their own tiny
// originals with ffmpeg. Nothing here ever copies or writes into vids/.
#pragma once

#include <QString>
#include <QStringList>

namespace scrubtub::testsupport {

// Absolute path of the vids/ corpus scanned as-is, or an empty string when it
// is unavailable on this machine. Resolution: SCRUBTUB_TEST_VIDS_DIR, then the
// SCRUBTUB_TEST_VIDS_DIR_DEFAULT path configured by CMake.
QString vidsDir();

// True when vidsDir() points at a readable directory with at least one video.
bool vidsAvailable();

// Sorted file names directly inside vids/ (no recursion).
QStringList vidsFiles();

// Smallest video in vids/ (optionally restricted to one suffix such as
// "mp4"), by size. Empty when vids/ is unavailable. Read-only: callers must
// never modify the returned file.
QString smallestVideo(const QString& suffix = QString());

// Video with a given exact file name in vids/, or empty when absent.
QString videoNamed(const QString& fileName);

// Fingerprint of vids/ (names, sizes, mtimes) used to prove the suites never
// wrote to it. Cheap: one stat per file.
QString vidsFingerprint();

// Synthesizes an ORIGINAL clip (ffmpeg lavfi testsrc2) at
// directory/fileName. Never derived from vids/: mutation cases (Trash,
// truncation, appended bytes, removal) need media they are allowed to change.
// Returns false and sets *error when ffmpeg is missing or fails.
bool makeSyntheticVideo(const QString& directory, const QString& fileName,
                        int durationMs, QString* error);

// Convenience: several synthetic clips of the same duration.
bool makeSyntheticVideos(const QString& directory, const QStringList& fileNames,
                         int durationMs, QString* error);

} // namespace scrubtub::testsupport
