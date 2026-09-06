// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "CatalogueModel.h"

#include <algorithm>

namespace itub {

QString formatIecBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("?");
    const double mib = 1024.0 * 1024.0;
    if (bytes >= 1024.0 * mib)
        return QStringLiteral("%1 GiB").arg(bytes / mib, 0, 'f', 2);
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
            {AtlasSourceRole, "atlasSource"}};
}

void CatalogueModel::applyRows(const QList<VideoRow>& rows, bool reset)
{
    if (reset) {
        beginResetModel();
        m_rows.clear();
        m_order.clear();
        for (const VideoRow& row : rows) {
            m_rows.insert(row.id, row);
            m_order.append(row.id);
        }
        std::sort(m_order.begin(), m_order.end(), [this](qint64 a, qint64 b) {
            return m_rows.value(a).fileName.compare(m_rows.value(b).fileName,
                                                    Qt::CaseInsensitive)
                < 0;
        });
        endResetModel();
        emit countChanged();
        return;
    }

    for (const VideoRow& row : rows) {
        const auto it = m_rows.constFind(row.id);
        if (it != m_rows.constEnd()) {
            m_rows.insert(row.id, row);
            const int pos = m_order.indexOf(row.id);
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
            endInsertRows();
            emit countChanged();
        }
    }
}

void CatalogueModel::applyProgress(const ScanProgress& progress)
{
    m_scanState = progress.state;
    m_discovered = progress.discovered;
    m_probed = progress.probed;
    m_errors = progress.errors;
    emit progressChanged();
}

} // namespace itub
