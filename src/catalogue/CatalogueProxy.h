// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// UI-thread facade over the worker-thread Catalogue (TECH_SPEC.md section 2:
// the UI thread must not wait on workers). QML talks only to this object;
// calls are queued to the catalogue thread and signals are forwarded back.
#pragma once

#include "CatalogueModel.h"

#include <QObject>

namespace itub {

class Catalogue;

class CatalogueProxy : public QObject {
    Q_OBJECT
    Q_PROPERTY(itub::CatalogueModel* model READ model CONSTANT FINAL)

public:
    explicit CatalogueProxy(Catalogue* catalogue, CatalogueModel* model,
                            QObject* parent = nullptr);

    CatalogueModel* model() const { return m_model; }

    Q_INVOKABLE void addRoot(const QString& path, bool includeHidden = false);
    Q_INVOKABLE void removeRoot(qint64 rootId);
    Q_INVOKABLE void rescanRoot(qint64 rootId, bool force = false);
    Q_INVOKABLE void pauseScanning();
    Q_INVOKABLE void resumeScanning();
    Q_INVOKABLE void cancelScanning();
    Q_INVOKABLE void setRating(qint64 videoId, int rating);

signals:
    void rootAdded(const itub::RootInfo& root);
    void rootRemoved(qint64 rootId);
    void rootRejected(const QString& reason);
    void operationFailed(const QString& message);

private:
    Catalogue* m_catalogue;
    CatalogueModel* m_model;
};

} // namespace itub
