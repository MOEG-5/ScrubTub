// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// UI-thread list model with asynchronous, generation-checked detail paging.
#pragma once

#include "catalogue/VideoRow.h"

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

namespace scrubtub {

QString formatIecBytes(qint64 bytes);          // MiB/GiB per §1; "?" when unknown
QString formatHms(qint64 durationMs);          // hh:mm:ss as needed

class CatalogueModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(QString scanState READ scanState NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 discovered READ discovered NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 probed READ probed NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 errors READ errors NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 previewsRemaining READ previewsRemaining NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 remaining READ remaining NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 processed READ processed NOTIFY progressChanged FINAL)
    Q_PROPERTY(quint64 failed READ failed NOTIFY progressChanged FINAL)

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
        ProbeErrorRole,
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
    quint64 previewsRemaining() const { return m_previewsRemaining; }
    quint64 remaining() const { return m_remaining; }
    quint64 processed() const { return m_processed; }
    quint64 failed() const { return m_failed; }

    using FetchCallback = std::function<void(const QList<qint64>&, quint64)>;

    // Sets the display order (search result or browse order). Details for
    // IDs not yet loaded are requested through the fetch callback in pages
    // of 200 (§8); delegates for missing rows show placeholders.
    Q_INVOKABLE void setOrder(const QList<qint64>& ids, bool isSearchResult);
    void setFetchCallback(FetchCallback callback);
    Q_INVOKABLE void setPageSize(int n) { m_pageSize = n; }
    qint64 generation() const { return m_generation; }

public slots:
    void applyRows(const QList<scrubtub::VideoRow>& rows, bool reset);
    void applyPage(const QList<scrubtub::VideoRow>& rows,
                   const QList<qint64>& requested, quint64 generation);
    void applyProgress(const scrubtub::ScanProgress& progress);

signals:
    void countChanged();
    void progressChanged();

private:
    void requestMissingDetails();

    QHash<qint64, VideoRow> m_rows;
    QVector<qint64> m_order;
    // Membership mirror of m_order: the linear QVector scan was the quadratic
    // term in page delivery and row updates.
    QSet<qint64> m_orderSet;
    // Next position in m_order not yet checked for missing details; avoids a
    // full order scan per page.
    int m_missingCursor = 0;
    FetchCallback m_fetch;
    bool m_fetching = false;
    QSet<qint64> m_requestedDetails;
    int m_pageSize = 200;
    quint64 m_generation = 0;
    QString m_scanState = QStringLiteral("idle");
    quint64 m_discovered = 0;
    quint64 m_probed = 0;
    quint64 m_errors = 0;
    quint64 m_failed = 0;
    quint64 m_processed = 0;
    quint64 m_remaining = 0;
    quint64 m_previewsRemaining = 0;
};


} // namespace scrubtub
