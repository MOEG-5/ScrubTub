// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "GridModel.h"

namespace itub {

GridModel::GridModel(int rows, QObject* parent)
    : QAbstractListModel(parent), m_rows(rows)
{
}

int GridModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows;
}

QVariant GridModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows)
        return {};
    const int row = index.row();
    switch (role) {
    case NameRole:
        return QStringLiteral("Sample video %1").arg(row, 4, 10, QLatin1Char('0'));
    case DurationRole:
        return QStringLiteral("%1:%2:%3")
            .arg(row / 3600, 2, 10, QLatin1Char('0'))
            .arg((row / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(row % 60, 2, 10, QLatin1Char('0'));
    case SizeRole:
        return QStringLiteral("%1 MiB").arg((row % 900) + 12);
    case DimensionsRole:
        return QStringLiteral("%1×%2").arg(1280 + (row % 5) * 160).arg(720 + (row % 5) * 90);
    case HueRole:
        return (row * 47) % 360;
    default:
        return {};
    }
}

QHash<int, QByteArray> GridModel::roleNames() const
{
    return {{NameRole, "name"}, {DurationRole, "duration"}, {SizeRole, "size"},
            {DimensionsRole, "dimensions"}, {HueRole, "hue"}};
}

void GridModel::setCellSize(int size)
{
    if (m_cellSize == size)
        return;
    m_cellSize = size;
    emit cellSizeChanged();
}

} // namespace itub
