// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "CatalogueModel.h"

#include <algorithm>

namespace scrubtub {

QString formatIecBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("?");
    const double kib = 1024.0;
    const double mib = kib * 1024.0;
    const double gib = mib * 1024.0;
    if (bytes >= gib)
        return QStringLiteral("%1 GiB").arg(bytes / gib, 0, 'f', 2);
    return QStringLiteral("%1 MiB").arg(bytes / mib, 0, 'f', 1);
}

QString formatHms(qint64 durationMs)
{
    if (durationMs < 0)
        return QStringLiteral("?");
    const qint64 totalSeconds = durationMs / 1000;
    if (totalSeconds < 3600)
        return QStringLiteral("%1:%2")
            .arg(totalSeconds / 60)
            .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2:%3")
        .arg(totalSeconds / 3600)
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

CatalogueModel::CatalogueModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int CatalogueModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_order.size();
}

QVariant CatalogueModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_order.size())
        return {};
    const VideoRow& row = m_rows.value(m_order.at(index.row()));
    switch (role) {
    case IdRole:
        return row.id;
    case NameRole:
        return row.fileName;
    case PathRole:
        return row.relPath;
    case SizeRole:
        return row.sizeBytes;
    case SizeTextRole:
        return formatIecBytes(row.sizeBytes);
    case DurationRole:
        return row.durationMs;
    case DurationTextRole:
        return formatHms(row.durationMs);
    case ResolutionRole:
        return row.displayWidth > 0
            ? QStringLiteral("%1×%2").arg(row.displayWidth).arg(row.displayHeight)
            : QStringLiteral("?");
    case CodecRole:
        return row.codec.isEmpty() ? QStringLiteral("?") : row.codec;
    case RatingRole:
        return row.rating;
    case ViewsRole:
        return row.views;
    case AvailabilityRole:
        return row.availability;
    case ProbeStatusRole:
        return row.probeStatus;
    case DisplayWidthRole:
        return row.displayWidth;
    case DisplayHeightRole:
        return row.displayHeight;
    case RevisionRole:
        return row.revision;
    // URLs appear only when the artifact exists: requesting earlier would
    // cache a null result and never refresh (QML pixmap cache).
    case PosterSourceRole:
        return row.posterReady
            ? QStringLiteral("image://previews/poster/%1-%2").arg(row.id).arg(row.revision)
            : QString();
    case AtlasSourceRole:
        return row.atlasReady
            ? QStringLiteral("image://previews/atlas/%1-%2").arg(row.id).arg(row.revision)
            : QString();
    case ProbeErrorRole:
        return row.probeError;
    default:
        return {};
    }
}

QHash<int, QByteArray> CatalogueModel::roleNames() const
{
    return {{IdRole, "videoId"},       {NameRole, "name"},
            {PathRole, "path"},        {SizeRole, "sizeBytes"},
            {SizeTextRole, "sizeText"}, {DurationRole, "durationMs"},
            {DurationTextRole, "durationText"}, {ResolutionRole, "resolution"},
            {CodecRole, "codec"},      {RatingRole, "rating"},
            {ViewsRole, "views"},      {AvailabilityRole, "availability"},
            {ProbeStatusRole, "probeStatus"},
            {DisplayWidthRole, "displayWidth"}, {DisplayHeightRole, "displayHeight"},
            {RevisionRole, "revision"},
            {PosterSourceRole, "posterSource"},
            {AtlasSourceRole, "atlasSource"},
            {ProbeErrorRole, "probeError"}};
}

void CatalogueModel::rebuildPositions()
{
    m_pos.clear();
    m_pos.reserve(m_order.size());
    for (int i = 0; i < m_order.size(); ++i)
        m_pos.insert(m_order.at(i), i);
    m_posDirty = false;
}

void CatalogueModel::applyRows(const QList<VideoRow>& rows, bool reset)
{
    if (reset) {
        ++m_generation;
        m_requestedDetails.clear();
        beginResetModel();
        m_rows.clear();
        m_order.clear();
        m_orderSet.clear();
        m_missingCursor = 0;
        for (const VideoRow& row : rows) {
            m_rows.insert(row.id, row);
            m_order.append(row.id);
            m_orderSet.insert(row.id);
        }
        std::sort(m_order.begin(), m_order.end(), [this](qint64 a, qint64 b) {
            return m_rows.value(a).fileName.compare(m_rows.value(b).fileName,
                                                    Qt::CaseInsensitive)
                < 0;
        });
        rebuildPositions();
        endResetModel();
        emit countChanged();
        return;
    }

    // Positions stay usable for the whole call unless this call inserts rows.
    bool positionsUsable = !m_posDirty;
    for (const VideoRow& row : rows) {
        if (row.id <= 0) continue;
        const auto it = m_rows.constFind(row.id);
        if (it != m_rows.constEnd() || m_orderSet.contains(row.id)) {
            m_rows.insert(row.id, row);
            if (!positionsUsable) {
                rebuildPositions();
                positionsUsable = true;
            }
            const int pos = m_pos.value(row.id, -1);
            if (pos >= 0) {
                const QModelIndex idx = index(pos);
                emit dataChanged(idx, idx);
            }
        } else {
            m_rows.insert(row.id, row);
            const int insertAt = static_cast<int>(
                std::lower_bound(m_order.begin(), m_order.end(), row.id,
                                 [this](qint64 a, qint64 b) {
                                     return m_rows.value(a).fileName.compare(
                                                m_rows.value(b).fileName,
                                                Qt::CaseInsensitive)
                                         < 0;
                                 })
                    - m_order.begin());
            beginInsertRows(QModelIndex(), insertAt, insertAt);
            m_order.insert(insertAt, row.id);
            m_orderSet.insert(row.id);
            m_posDirty = true; // every position at/after insertAt shifted
            positionsUsable = false;
            if (insertAt <= m_missingCursor)
                ++m_missingCursor; // keep the cursor on the same boundary
            endInsertRows();
            emit countChanged();
        }
    }
}

void CatalogueModel::setOrder(const QList<qint64>& ids, bool isSearchResult)
{
    Q_UNUSED(isSearchResult);
    ++m_generation;
    m_requestedDetails.clear();
    beginResetModel();
    m_order = ids; // IDs without details yet fetch their pages on demand
    m_orderSet = QSet<qint64>(ids.cbegin(), ids.cend());
    m_missingCursor = 0;
    rebuildPositions();
    endResetModel();
    emit countChanged();
    requestMissingDetails();
}

void CatalogueModel::setFetchCallback(FetchCallback callback)
{
    m_fetch = std::move(callback);
}

// Request each ID once per order generation. Async delivery must return to the
// event loop, and synchronous test callbacks must not recurse through pages.
void CatalogueModel::requestMissingDetails()
{
    if (!m_fetch || m_fetching)
        return;
    m_fetching = true;
    for (;;) {
        QList<qint64> missing;
        // Resume where the last page stopped instead of rescanning the order.
        while (m_missingCursor < m_order.size()) {
            const qint64 id = m_order.at(m_missingCursor);
            if (!m_rows.contains(id) && !m_requestedDetails.contains(id)) {
                missing.append(id);
                m_requestedDetails.insert(id);
            }
            ++m_missingCursor;
            if (missing.size() >= m_pageSize)
                break;
        }
        if (missing.isEmpty())
            break;
        const auto generation = m_generation;
        m_fetch(missing, generation);
        // A queued reply has not arrived yet: let it request the next page.
        if (generation == m_generation && m_orderSet.contains(missing.first())
            && !m_rows.contains(missing.first()))
            break;
    }
    m_fetching = false;
}

void CatalogueModel::applyPage(const QList<VideoRow>& rows,
                               const QList<qint64>& requested, quint64 generation)
{
    if (generation != m_generation)
        return;
    // Page membership and order membership are set lookups: per-row linear
    // scans over the whole order were the dominant cost of detail paging.
    const QSet<qint64> requestedSet(requested.cbegin(), requested.cend());
    QSet<qint64> received;
    QList<VideoRow> visibleRows;
    for (const auto& row : rows) {
        if (row.id > 0 && requestedSet.contains(row.id) && m_orderSet.contains(row.id)) {
            received.insert(row.id);
            visibleRows.append(row);
        }
    }
    // A video may have been removed between the search and its page reply.
    for (const auto id : requested) {
        if (received.contains(id))
            continue;
        const int pos = m_order.indexOf(id);
        if (pos < 0)
            continue;
        beginRemoveRows({}, pos, pos);
        m_order.removeAt(pos);
        m_orderSet.remove(id);
        m_rows.remove(id);
        m_posDirty = true;
        m_pos.remove(id);
        if (pos < m_missingCursor)
            --m_missingCursor;
        endRemoveRows();
        emit countChanged();
    }
    applyRows(visibleRows, false);
    requestMissingDetails();
}

void CatalogueModel::applyProgress(const ScanProgress& progress)
{
    m_scanState = progress.state;
    m_discovered = progress.discovered;
    m_probed = progress.probed;
    m_errors = progress.errors;
    m_remaining = progress.remaining;
    m_previewsRemaining = progress.previewsRemaining;
    m_processed = progress.processed;
    m_failed = progress.failed;
    emit progressChanged();
}

} // namespace scrubtub
