// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Deterministic filename/folder/technical tagging (TECH_SPEC.md section 7).
//
// Normalization: NFKC + case folding for tag identity; a readable display
// label is preserved. Filename stems and directories below the selected root
// are split at punctuation/separators into Unicode letter/digit tokens;
// camel-case boundaries split before case folding. The user's home path and
// the root's own name never become tags. The source filename is never
// mutated.
#pragma once

#include <QString>
#include <QStringList>

namespace itub {

class TagEngine {
public:
    // Tag identity: NFKC + case folded; empty when nothing survives.
    static QString normalize(const QString& text);

    // Filename-derived tags for a stem (no extension). Discards empty and
    // single-character tokens, standalone numbers, recognized codec/
    // resolution/container tokens; keeps alphabetic words of at least two
    // characters and meaningful mixed alphanumeric tokens.
    static QStringList filenameTags(const QString& stem);

    // Directory-derived tags for the path segments below the catalogue root
    // (each segment split like a filename stem, in order).
    static QStringList folderTags(const QStringList& pathSegments);

    // Technical tags from extracted metadata (distinct provenance).
    static QStringList technicalTags(const QString& codec, int displayWidth,
                                     int displayHeight);

    // Splits raw text into Unicode letter/digit tokens; camel-case
    // boundaries split before folding. Used for search indexing too.
    static QStringList tokenize(const QString& text);

    // Resolution bucket from the shorter display side ("1080" for 1080–1439).
    static QString resolutionBucket(int displayWidth, int displayHeight);

    // Query token matching limits (§8): 1–2 chars exact/prefix/substring
    // only; 3–5 permit edit distance 1; ≥6 permit 2.
    static int editDistanceLimit(int queryTokenLength);

    // Diacritic-folded copy for convenient search matching (cafe → Café);
    // never used for identity or display.
    static QString diacriticFold(const QString& text);
};

} // namespace itub
