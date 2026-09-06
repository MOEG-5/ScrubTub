// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Milestone-3 catalogue layer: deterministic tags, live fuzzy search with
// structured filters, and paged detail fetching. Included by Catalogue.cpp;
// every function runs on the catalogue thread (TECH_SPEC.md sections 7, 8).
#include "Catalogue.h"
#include "Database.h"
#include "TagEngine.h"
#include "media/Probe.h"

#include <QFileInfo>

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <rapidfuzz/distance/Levenshtein.hpp>

namespace itub {

namespace {

QStringList splitQueryTokens(const QString& text)
{
    return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

// Search snapshot record for one row (§8: compact pre-normalized records).
void buildSearchRecord(Database* db, qint64 videoId, SearchRecord* out)
{
    Statement st = db->prepare(
        "SELECT v.file_name, v.rel_path FROM videos v WHERE v.id=?");
    st.bind(1, videoId);
    if (!st.step())
        return;
    out->id = videoId;
    out->fileName = st.text(0);
    out->relPath = st.text(1);

    QStringList sourceTexts{out->fileName, out->relPath};
    Statement tagSt = db->prepare(
        "SELECT t.norm FROM video_tags vt JOIN tags t ON t.id = vt.tag_id "
        "WHERE vt.video_id=? AND NOT EXISTS("
        "  SELECT 1 FROM tag_suppressions s WHERE s.video_id = vt.video_id "
        "  AND s.tag_id = vt.tag_id AND vt.origin != 'manual')");
    tagSt.bind(1, videoId);
    while (tagSt.step())
        sourceTexts.append(tagSt.text(0));

    QStringList tokens;
    for (const QString& text : sourceTexts)
        for (const QString& token : TagEngine::tokenize(text)) {
            const QString norm = TagEngine::normalize(token);
            if (!norm.isEmpty() && !tokens.contains(norm))
                tokens.append(norm);
        }
    out->tokens = tokens;
    QStringList folded;
    for (const QString& token : tokens) {
        const QString f = TagEngine::diacriticFold(token);
        if (!folded.contains(f))
            folded.append(f);
    }
    out->foldedTokens = folded;
}

struct MatchResult {
    int worstClass = -1;        // 0 exact, 1 prefix, 2 substring, 3 typo
    int totalDistance = 0;
    bool nameMatched = false;   // matched in the filename (not only folders)
    bool matched = true;
};

MatchResult matchTokens(const SearchRecord& record,
                        const QStringList& queryTokens,
                        const QStringList& foldedQuery)
{
    MatchResult result;
    for (int qi = 0; qi < queryTokens.size(); ++qi) {
        const QString& q = queryTokens.at(qi);
        const QString fq = foldedQuery.at(qi);
        int bestClass = -1;
        int bestDistance = -1;
        bool nameOnly = false;
        // Exact/prefix/substring over the folded full strings and tokens.
        const QString foldedName = TagEngine::diacriticFold(record.fileName);
        const QString foldedPath = TagEngine::diacriticFold(record.relPath);
        for (int ti = 0; ti < record.foldedTokens.size(); ++ti) {
            const QString& token = record.foldedTokens.at(ti);
            int cls = -1;
            int dist = 0;
            if (token == fq)
                cls = 0;
            else if (token.startsWith(fq))
                cls = 1;
            else if (token.contains(fq))
                cls = 2;
            else {
                const int limit = TagEngine::editDistanceLimit(fq.size());
                if (limit > 0) {
                    const int d = rapidfuzz::levenshtein_distance(
                        token.toStdU32String(), fq.toStdU32String());
                    if (d <= limit) {
                        cls = 3;
                        dist = d;
                    }
                }
            }
            if (cls >= 0 && (bestClass < 0 || cls < bestClass
                             || (cls == bestClass && dist < bestDistance))) {
                bestClass = cls;
                bestDistance = dist;
                Q_UNUSED(ti);
            }
        }
        // Substring across the whole folded name/path (multi-word queries).
        if (bestClass < 0 || bestClass > 2) {
            if (foldedName.contains(fq)) {
                bestClass = bestClass < 0 ? 2 : qMin(bestClass, 2);
                bestDistance = 0;
                nameOnly = true;
            } else if (foldedPath.contains(fq) && bestClass < 0) {
                bestClass = 2;
                bestDistance = 0;
            }
        }
        if (bestClass < 0) {
            result.matched = false;
            return result;
        }
        result.worstClass = qMax(result.worstClass, bestClass);
        result.totalDistance += qMax(0, bestDistance);
        if (nameOnly)
            result.nameMatched = true;
    }
    return result;
}

} // namespace

// ----------------------------- search (§8) ---------------------------------

void Catalogue::refreshSearchRecord(qint64 videoId)
{
    SearchRecord record;
    buildSearchRecord(m_db.get(), videoId, &record);
    m_searchRecords.insert(videoId, record);
}

QString Catalogue::whereForFilters(const QuerySpec& spec, QStringList* wheres) const
{
    if (spec.sizeMin >= 0)
        wheres->append(QStringLiteral("size_bytes >= %1").arg(spec.sizeMin));
    if (spec.sizeMax >= 0)
        wheres->append(QStringLiteral("size_bytes <= %1").arg(spec.sizeMax));
    if (spec.widthMin > 0)
        wheres->append(QStringLiteral("display_width >= %1").arg(spec.widthMin));
    if (spec.widthMax > 0)
        wheres->append(QStringLiteral("display_width <= %1").arg(spec.widthMax));
    if (spec.heightMin > 0)
        wheres->append(QStringLiteral("display_height >= %1").arg(spec.heightMin));
    if (spec.heightMax > 0)
        wheres->append(QStringLiteral("display_height <= %1").arg(spec.heightMax));
    if (!spec.resolutionPreset.isEmpty()) {
        // Shorter-side bucket, e.g. 1080 means 1080–1439 (§1).
        bool ok = false;
        const int preset = spec.resolutionPreset.toInt(&ok);
        if (ok && preset > 0)
            wheres->append(QStringLiteral(
                "MIN(display_width, display_height) >= %1 "
                "AND MIN(display_width, display_height) < %2")
                .arg(preset).arg(preset + preset / 3));
    }
    if (spec.durationMinMs >= 0)
        wheres->append(QStringLiteral("duration_ms >= %1").arg(spec.durationMinMs));
    if (spec.durationMaxMs >= 0)
        wheres->append(QStringLiteral("duration_ms <= %1").arg(spec.durationMaxMs));
    if (spec.ratingMode == 1)
        wheres->append(QStringLiteral("rating IS NULL"));
    else if (spec.ratingMode == 2)
        wheres->append(QStringLiteral("rating = %1").arg(spec.ratingValue));
    if (spec.rootId >= 0)
        wheres->append(QStringLiteral("root_id = %1").arg(spec.rootId));
    if (!spec.folderPrefix.isEmpty())
        wheres->append(QStringLiteral(
            "substr(rel_path, 1, %1) = '%2'").arg(spec.folderPrefix.size())
            .arg(QString(spec.folderPrefix).replace(QLatin1Char('\''), QLatin1String("''"))));
    if (spec.viewsMin >= 0)
        wheres->append(QStringLiteral("views >= %1").arg(spec.viewsMin));
    if (!spec.availability.isEmpty()) {
        QStringList quoted;
        for (const QString& a : spec.availability)
            quoted.append(QStringLiteral("'%1'").arg(
                QString(a).replace(QLatin1Char('\''), QLatin1String("''"))));
        wheres->append(QStringLiteral("availability IN (%1)").arg(quoted.join(", ")));
    }
    return QString();
}

void Catalogue::search(const QuerySpec& spec)
{
    const quint64 generation = ++m_searchGeneration;

    // Visible query validation, never silent truncation (§8).
    QString validationError;
    if (spec.text.size() > 256)
        validationError = QStringLiteral("Query exceeds 256 characters");
    const QStringList queryTokens = splitQueryTokens(spec.text);
    if (queryTokens.size() > 16)
        validationError = QStringLiteral("Query exceeds 16 tokens");
    QStringList foldedQuery;
    for (const QString& token : queryTokens)
        foldedQuery.append(TagEngine::diacriticFold(TagEngine::normalize(token)));

    // Structured filters run in SQL; tag filters constrain the candidate set.
    QStringList wheres;
    whereForFilters(spec, &wheres);
    for (const QString& tag : spec.includeAllTags) {
        wheres.append(QStringLiteral(
            "EXISTS(SELECT 1 FROM video_tags vt JOIN tags t ON t.id=vt.tag_id "
            "WHERE vt.video_id=videos.id AND t.norm='%1' AND NOT EXISTS("
            "SELECT 1 FROM tag_suppressions s WHERE s.video_id=videos.id "
            "AND s.tag_id=vt.tag_id AND vt.origin!='manual'))")
            .arg(QString(tag).replace(QLatin1Char('\''), QLatin1String("''"))));
    }
    for (const QString& tag : spec.excludeTags) {
        wheres.append(QStringLiteral(
            "NOT EXISTS(SELECT 1 FROM video_tags vt JOIN tags t ON t.id=vt.tag_id "
            "WHERE vt.video_id=videos.id AND t.norm='%1' AND NOT EXISTS("
            "SELECT 1 FROM tag_suppressions s WHERE s.video_id=videos.id "
            "AND s.tag_id=vt.tag_id AND vt.origin!='manual'))")
            .arg(QString(tag).replace(QLatin1Char('\''), QLatin1String("''"))));
    }
    if (!spec.includeAnyTags.isEmpty()) {
        QStringList quoted;
        for (const QString& tag : spec.includeAnyTags)
            quoted.append(QStringLiteral("'%1'").arg(
                QString(tag).replace(QLatin1Char('\''), QLatin1String("''"))));
        wheres.append(QStringLiteral(
            "EXISTS(SELECT 1 FROM video_tags vt JOIN tags t ON t.id=vt.tag_id "
            "WHERE vt.video_id=videos.id AND t.norm IN (%1) AND NOT EXISTS("
            "SELECT 1 FROM tag_suppressions s WHERE s.video_id=videos.id "
            "AND s.tag_id=vt.tag_id AND vt.origin!='manual'))")
            .arg(quoted.join(QStringLiteral(", "))));
    }

    // Stable sort: key first, then the video-ID tie-breaker (§1). Unknowns
    // sort last in both directions.
    QString orderBy = QStringLiteral("videos.id ASC");
    const QString key = spec.sortKey;
    const QString dir = spec.sortDescending ? QStringLiteral("DESC")
                                            : QStringLiteral("ASC");
    struct SortColumn { const char* key; const char* sql; };
    const SortColumn columns[] = {
        {"name", "file_name"}, {"size", "size_bytes"}, {"duration", "duration_ms"},
        {"resolution", "MIN(display_width, display_height)"},
        {"rating", "rating"}, {"views", "views"}, {"added", "added_ms"},
        {"mtime", "mtime_ms"}, {"lastOpened", "last_opened_ms"},
    };
    for (const SortColumn& column : columns) {
        if (key == QLatin1String(column.key)) {
            orderBy = QStringLiteral("(%1 IS NULL) ASC, %2 %3, videos.id ASC")
                          .arg(column.sql, column.sql, dir);
            break;
        }
    }

    QString sql = QStringLiteral("SELECT id FROM videos");
    if (!wheres.isEmpty())
        sql += QStringLiteral(" WHERE ") + wheres.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY ") + orderBy;

    QList<qint64> ordered;
    if (!validationError.isEmpty()) {
        emit searchCompleted(generation, ordered, validationError);
        return;
    }

    Statement st = m_db->prepare(sql.toUtf8().constData());
    if (!st.isValid()) {
        emit operationFailed(m_db->lastError());
        return;
    }
    struct Candidate {
        qint64 id;
        MatchResult match;
    };
    QVector<Candidate> candidates;
    while (st.step()) {
        if (generation != m_searchGeneration)
            return; // stale query: discard even if this raced completion (§8)
        const qint64 id = st.int64(0);
        if (!m_searchRecords.contains(id))
            refreshSearchRecord(id);
        const SearchRecord& record = m_searchRecords.value(id);
        if (queryTokens.isEmpty()) {
            candidates.append({id, MatchResult{}});
            continue;
        }
        const MatchResult match = matchTokens(record, queryTokens, foldedQuery);
        if (match.matched)
            candidates.append({id, match});
    }

    if (!queryTokens.isEmpty() && !spec.userSort) {
        // Relevance (§8): match class, summed edit distance, filename before
        // folder-only matches, then normalized filename, then ID.
        std::sort(candidates.begin(), candidates.end(),
                  [this](const Candidate& a, const Candidate& b) {
                      if (a.match.worstClass != b.match.worstClass)
                          return a.match.worstClass < b.match.worstClass;
                      if (a.match.totalDistance != b.match.totalDistance)
                          return a.match.totalDistance < b.match.totalDistance;
                      if (a.match.nameMatched != b.match.nameMatched)
                          return a.match.nameMatched;
                      const QString& an = m_searchRecords.value(a.id).fileName;
                      const QString& bn = m_searchRecords.value(b.id).fileName;
                      const int c = TagEngine::normalize(an)
                                        .compare(TagEngine::normalize(bn),
                                                 Qt::CaseInsensitive);
                      if (c != 0)
                          return c < 0;
                      return a.id < b.id;
                  });
    }
    ordered.reserve(candidates.size());
    for (const Candidate& candidate : candidates)
        ordered.append(candidate.id);

    emit searchCompleted(generation, ordered, validationError);
}

void Catalogue::fetchRowsPage(const QList<qint64>& videoIds)
{
    QList<VideoRow> rows;
    for (const qint64 id : videoIds)
        rows.append(readRow(id));
    emit rowsChanged(rows, false);
}

void Catalogue::clearSearch()
{
    ++m_searchGeneration;
}

// ------------------------------- tags (§7) ---------------------------------

namespace {

qint64 ensureTag(Database* db, const QString& normalized, const QString& label)
{
    Statement find = db->prepare("SELECT id FROM tags WHERE norm=?");
    find.bind(1, normalized);
    if (find.step())
        return find.int64(0);
    Statement ins = db->prepare("INSERT INTO tags(norm, label) VALUES(?,?)");
    ins.bind(1, normalized);
    ins.bind(2, label);
    ins.run();
    return db->lastInsertRowId();
}

} // namespace

void Catalogue::applyAutoTags(qint64 videoId, const ProbeResult& probe)
{
    Statement info = m_db->prepare(
        "SELECT v.rel_path, v.file_name FROM videos v WHERE v.id=?");
    info.bind(1, videoId);
    if (!info.step())
        return;
    const QString relPath = info.text(0);
    const QString fileName = info.text(1);

    // Directories below the selected root only (§7): strip the root from the
    // relative path; the root's own name never becomes a tag.
    QStringList segments = relPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!segments.isEmpty())
        segments.removeLast(); // the filename itself
    QStringList folderSegs;
    for (const QString& segment : segments)
        folderSegs.append(QFileInfo(segment).completeBaseName());

    const QStringList nameTags = TagEngine::filenameTags(
        QFileInfo(fileName).completeBaseName());
    const QStringList folderTagList = TagEngine::folderTags(folderSegs);
    const QStringList techTags =
        TagEngine::technicalTags(probe.codec, probe.displayWidth, probe.displayHeight);

    m_db->transaction([this, videoId, &nameTags, &folderTagList, &techTags] {
        // Regeneration replaces automatic assignments only, atomically (§7).
        Statement del = m_db->prepare(
            "DELETE FROM video_tags WHERE video_id=? AND origin IN "
            "('filename','folder','technical')");
        del.bind(1, videoId);
        del.run();
        auto insert = [&](const QStringList& tags, const char* origin) {
            for (const QString& tag : tags) {
                const qint64 tagId = ensureTag(m_db.get(), tag, tag);
                Statement vt = m_db->prepare(
                    "INSERT OR IGNORE INTO video_tags(video_id, tag_id, origin) "
                    "VALUES(?,?,?)");
                vt.bind(1, videoId);
                vt.bind(2, tagId);
                // TEXT, not blob: the origin CHECK constraint compares text.
                vt.bind(3, QString::fromLatin1(origin));
                vt.run();
            }
        };
        insert(nameTags, "filename");
        insert(folderTagList, "folder");
        insert(techTags, "technical");
        return true;
    });
    refreshSearchRecord(videoId);
    emit tagListChanged();
}

void Catalogue::addManualTag(qint64 videoId, const QString& text)
{
    const QString norm = TagEngine::normalize(text);
    if (norm.isEmpty())
        return;
    m_db->transaction([this, videoId, &norm, &text] {
        const qint64 tagId = ensureTag(m_db.get(), norm, text);
        // A manual assignment takes precedence over an automatic suppression
        // (§7): dropping the suppression restores visibility.
        Statement drop = m_db->prepare(
            "DELETE FROM tag_suppressions WHERE video_id=? AND tag_id=?");
        drop.bind(1, videoId);
        drop.bind(2, tagId);
        drop.run();
        Statement vt = m_db->prepare(
            "INSERT OR IGNORE INTO video_tags(video_id, tag_id, origin) "
            "VALUES(?,?,'manual')");
        vt.bind(1, videoId);
        vt.bind(2, tagId);
        vt.run();
        return true;
    });
    refreshSearchRecord(videoId);
    requestTags(videoId);
    emit tagListChanged();
}

void Catalogue::removeTag(qint64 videoId, qint64 tagId)
{
    m_db->transaction([this, videoId, tagId] {
        // Manual origin removed; an automatic origin (if any) remains and
        // stays visible unless suppressed (§7).
        Statement del = m_db->prepare(
            "DELETE FROM video_tags WHERE video_id=? AND tag_id=? AND origin='manual'");
        del.bind(1, videoId);
        del.bind(2, tagId);
        del.run();
        return true;
    });
    refreshSearchRecord(videoId);
    requestTags(videoId);
    emit tagListChanged();
}

void Catalogue::suppressAutoTag(qint64 videoId, qint64 tagId)
{
    // Deleting an automatic tag creates a suppression so rescans and rule
    // updates do not restore it (§7).
    m_db->transaction([this, videoId, tagId] {
        Statement ins = m_db->prepare(
            "INSERT OR IGNORE INTO tag_suppressions(video_id, tag_id) VALUES(?,?)");
        ins.bind(1, videoId);
        ins.bind(2, tagId);
        ins.run();
        return true;
    });
    refreshSearchRecord(videoId);
    requestTags(videoId);
    emit tagListChanged();
}

void Catalogue::resetSuppressions()
{
    m_db->exec("DELETE FROM tag_suppressions");
    emit tagListChanged();
}

void Catalogue::regenerateAutoTags(qint64 videoId)
{
    Statement info = m_db->prepare(
        "SELECT rel_path, codec, display_width, display_height, duration_ms "
        "FROM videos WHERE id=?");
    info.bind(1, videoId);
    if (!info.step())
        return;
    ProbeResult probe;
    probe.codec = info.text(1);
    probe.displayWidth = info.isNull(2) ? 0 : static_cast<int>(info.int64(2));
    probe.displayHeight = info.isNull(3) ? 0 : static_cast<int>(info.int64(3));
    applyAutoTags(videoId, probe);
    requestTags(videoId);
}

void Catalogue::requestTags(qint64 videoId)
{
    QVariantList tags;
    Statement st = m_db->prepare(
        "SELECT t.id, t.norm, t.label, vt.origin, "
        "EXISTS(SELECT 1 FROM tag_suppressions s WHERE s.video_id=vt.video_id "
        "       AND s.tag_id=vt.tag_id) AS suppressed "
        "FROM video_tags vt JOIN tags t ON t.id = vt.tag_id WHERE vt.video_id=? "
        "ORDER BY t.norm");
    st.bind(1, videoId);
    while (st.step()) {
        QVariantMap tag;
        tag.insert(QStringLiteral("tagId"), st.int64(0));
        tag.insert(QStringLiteral("norm"), st.text(1));
        tag.insert(QStringLiteral("label"), st.text(2));
        tag.insert(QStringLiteral("origin"), st.text(3));
        tag.insert(QStringLiteral("suppressed"), st.int64(4) != 0);
        tags.append(tag);
    }
    emit tagsReady(videoId, tags);
}

} // namespace itub
