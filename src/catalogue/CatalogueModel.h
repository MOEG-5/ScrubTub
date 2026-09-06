// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// UI-thread list model fed by Catalogue's rowsChanged signals. Milestone-1
// scope: flat, sorted list with formatted fields. Milestone 2 replaces this
// with the viewport-sized paged card model.
#pragma once

#include "catalogue/VideoRow.h"

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace itub {

QString formatIecBytes(qint64 bytes);          // MiB/GiB per §1; "?" when unknown
QString formatHms(qint64 durationMs);          // hh:mm:ss as needed

class CatalogueModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(QString scanState READ scanState NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 discovered READ discovered NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 probed READ probed NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 errors READ errors NOTIFY progressChanged FINAL)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        PathRole,
        SizeRole,
        SizeTextRole,
        DurationRole,
        DurationTextRole,
        ResolutionRole,
        CodecRole,
        RatingRole,
        ViewsRole,
        AvailabilityRole,
        ProbeStatusRole,
        DisplayWidthRole,
        DisplayHeightRole,
        RevisionRole,
        PosterSourceRole,
        AtlasSourceRole,
    };
    Q_ENUM(Roles)

    explicit CatalogueModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_order.size(); }
    QString scanState() const { return m_scanState; }
    quint64 discovered() const { return m_discovered; }
    quint64 probed() const { return m_probed; }
    quint64 errors() const { return m_errors; }

    using FetchCallback = std::function<void(const QList<qint64>&)>;

    // Sets the display order (search result or browse order). Details for
    // IDs not yet loaded are requested through the fetch callback in pages
    // of 200 (§8); delegates for missing rows show placeholders.
    Q_INVOKABLE void setOrder(const QList<qint64>& ids, bool isSearchResult);
    void setFetchCallback(FetchCallback callback);
    Q_INVOKABLE void setPageSize(int n) { m_pageSize = n; }
    qint64 generation() const { return m_generation; }

public slots:
    void applyRows(const QList<itub::VideoRow>& rows, bool reset);
    void applyProgress(const itub::ScanProgress& progress);

signals:
    void countChanged();
    void progressChanged();

private:
    void requestMissingDetails();

    QHash<qint64, VideoRow> m_rows;
    QVector<qint64> m_order;
    FetchCallback m_fetch;
    bool m_fetching = false;
    int m_pageSize = 200;
    quint64 m_generation = 0;
    QString m_scanState = QStringLiteral("idle");
    quint64 m_discovered = 0;
    quint64 m_probed = 0;
    quint64 m_errors = 0;
};


} // namespace itub
