// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "TagEngine.h"

#include <QChar>
#include <QStringList>

#include <rapidfuzz/distance/Levenshtein.hpp>

namespace scrubtub {

namespace {

// Tokens that carry no organizational value (§7); a visible editable ignore
// list extends this in the catalogue layer. Codec/resolution/container
// tokens are filtered so file naming patterns do not become tags.
const char* kBuiltinIgnore[] = {
    "final", "copy", "test", "backup", "export", "render", "output", "edit",
    "mp4", "m4v", "mkv", "webm", "avi", "mov", "wmv", "flv", "mpg", "mpeg",
    "ogv", "h264", "h265", "hevc", "av1", "vp9", "vp8", "xvid", "divx",
    "aac", "mp3", "opus", "vorbis", "flac", "ac3", "dts", "1080p", "720p",
    "480p", "2160p", "4k", "8k", "60fps", "30fps", "24fps", "25fps", "50fps",
};

bool isIgnored(const QString& normalizedToken)
{
    for (const char* word : kBuiltinIgnore)
        if (normalizedToken == QLatin1String(word))
            return true;
    return false;
}

// Meaningful mixed alphanumeric: contains both letters and digits with at
// least one letter run of ≥2 characters (e.g. "testvideo1" stays, "abc123"
// stays, "01" does not — standalone numbers are already dropped).
bool isMeaningfulMixed(const QString& token)
{
    bool hasLetterRun = false;
    bool hasDigit = false;
    int run = 0;
    for (const QChar c : token) {
        if (c.isLetter()) {
            ++run;
            if (run >= 2)
                hasLetterRun = true;
        } else {
            run = 0;
            if (c.isDigit())
                hasDigit = true;
        }
    }
    return hasLetterRun && hasDigit;
}

} // namespace

QString TagEngine::diacriticFold(const QString& text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing
            && c.category() != QChar::Mark_SpacingCombining)
            out.append(c);
    }
    return out;
}

QString TagEngine::normalize(const QString& text)
{
    // NFKC + case folding (§7): compatibility-equivalent spellings fold
    // together, accents are preserved, case differences collapse.
    return text.normalized(QString::NormalizationForm_KC).toCaseFolded().trimmed();
}

QStringList TagEngine::tokenize(const QString& text)
{
    QStringList tokens;
    QString current;
    auto flush = [&] {
        if (!current.isEmpty()) {
            tokens.append(current);
            current.clear();
        }
    };
    // Camel-case boundaries split before case folding: keep the original
    // characters, start a new token at a lower→upper transition.
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.isLetterOrNumber()) {
            if (!current.isEmpty() && c.isUpper()
                && text.at(i - 1).isLower())
                flush();
            current.append(c);
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

QStringList TagEngine::filenameTags(const QString& stem)
{
    QStringList tags;
    for (const QString& raw : tokenize(stem)) {
        const QString norm = normalize(raw);
        if (norm.size() < 2)
            continue;
        // Standalone numbers are discarded (pure numeric years included).
        bool allDigits = true;
        bool hasLetter = false;
        for (const QChar c : norm) {
            if (!c.isDigit())
                allDigits = false;
            if (c.isLetter())
                hasLetter = true;
        }
        if (allDigits || !hasLetter)
            continue;
        // Alphabetic words of ≥2 characters pass; mixed tokens must be
        // meaningful (§7).
        bool hasDigit = false;
        for (const QChar c : norm)
            if (c.isDigit())
                hasDigit = true;
        if (hasDigit && !isMeaningfulMixed(norm))
            continue;
        if (isIgnored(norm))
            continue;
        if (!tags.contains(norm))
            tags.append(norm);
    }
    return tags;
}

QStringList TagEngine::folderTags(const QStringList& pathSegments)
{
    QStringList tags;
    for (const QString& segment : pathSegments)
        for (const QString& tag : filenameTags(segment))
            if (!tags.contains(tag))
                tags.append(tag);
    return tags;
}

QString TagEngine::resolutionBucket(int displayWidth, int displayHeight)
{
    if (displayWidth <= 0 || displayHeight <= 0)
        return QString();
    const int shorter = qMin(displayWidth, displayHeight);
    if (shorter >= 2160)
        return QStringLiteral("2160");
    if (shorter >= 1440)
        return QStringLiteral("1440");
    if (shorter >= 1080)
        return QStringLiteral("1080");
    if (shorter >= 720)
        return QStringLiteral("720");
    if (shorter >= 480)
        return QStringLiteral("480");
    return QString();
}

QStringList TagEngine::technicalTags(const QString& codec, int displayWidth,
                                     int displayHeight)
{
    QStringList tags;
    const QString normCodec = normalize(codec);
    if (!normCodec.isEmpty() && normCodec != QStringLiteral("unknown"))
        tags.append(QStringLiteral("codec:") + normCodec);
    const QString bucket = resolutionBucket(displayWidth, displayHeight);
    if (!bucket.isEmpty())
        tags.append(QStringLiteral("resolution:") + bucket);
    return tags;
}

int TagEngine::editDistanceLimit(int queryTokenLength)
{
    if (queryTokenLength >= 6)
        return 2;
    if (queryTokenLength >= 3)
        return 1;
    return 0; // 1–2 characters: exact/prefix/substring only (§8)
}

} // namespace scrubtub
