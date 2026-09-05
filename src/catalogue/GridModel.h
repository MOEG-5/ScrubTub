// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Milestone-0 placeholder grid model: proves a viewport-sized virtual grid
// with delegate reuse. Replaced by the paged catalogue card model in
// milestone 2.
#pragma once

#include <QAbstractListModel>

namespace itub {

class GridModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int cellSize READ cellSize WRITE setCellSize NOTIFY cellSizeChanged FINAL)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        DurationRole,
        SizeRole,
        DimensionsRole,
        HueRole,
    };
    Q_ENUM(Roles)

    explicit GridModel(int rows = 2000, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_rows; }
    int cellSize() const { return m_cellSize; }
    void setCellSize(int size);

signals:
    void countChanged();
    void cellSizeChanged();

private:
    int m_rows;
    int m_cellSize = 180;
};

} // namespace itub
