// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "CatalogueProxy.h"

#include "Catalogue.h"

#include <QMetaObject>

namespace itub {

CatalogueProxy::CatalogueProxy(Catalogue* catalogue, CatalogueModel* model,
                               QObject* parent)
    : QObject(parent), m_catalogue(catalogue), m_model(model)
{
    // Queued signal deliveries from the catalogue thread; re-emitted on the
    // UI thread for QML.
    connect(catalogue, &Catalogue::rootAdded, this, &CatalogueProxy::rootAdded);
    connect(catalogue, &Catalogue::rootRemoved, this, &CatalogueProxy::rootRemoved);
    connect(catalogue, &Catalogue::rootRejected, this, &CatalogueProxy::rootRejected);
    connect(catalogue, &Catalogue::operationFailed, this, &CatalogueProxy::operationFailed);
}

void CatalogueProxy::addRoot(const QString& path, bool includeHidden)
{
    QMetaObject::invokeMethod(m_catalogue, [this, path, includeHidden] {
        m_catalogue->addRoot(path, includeHidden);
    });
}

void CatalogueProxy::removeRoot(qint64 rootId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, rootId] {
        m_catalogue->removeRoot(rootId);
    });
}

void CatalogueProxy::rescanRoot(qint64 rootId, bool force)
{
    QMetaObject::invokeMethod(m_catalogue, [this, rootId, force] {
        m_catalogue->rescanRoot(rootId, force);
    });
}

void CatalogueProxy::pauseScanning()
{
    QMetaObject::invokeMethod(m_catalogue, [this] { m_catalogue->pauseScanning(); });
}

void CatalogueProxy::resumeScanning()
{
    QMetaObject::invokeMethod(m_catalogue, [this] { m_catalogue->resumeScanning(); });
}

void CatalogueProxy::cancelScanning()
{
    QMetaObject::invokeMethod(m_catalogue, [this] { m_catalogue->cancelScanning(); });
}

void CatalogueProxy::setRating(qint64 videoId, int rating)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId, rating] {
        m_catalogue->setRating(videoId, rating);
    });
}

} // namespace itub
