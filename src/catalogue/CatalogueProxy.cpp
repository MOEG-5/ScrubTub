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
    connect(catalogue, &Catalogue::hoverSourceReady, this, &CatalogueProxy::hoverSourceReady);
    connect(catalogue, &Catalogue::sampleTimesReady, this, &CatalogueProxy::sampleTimesReady);
    connect(catalogue, &Catalogue::cacheEntryChanged, this, &CatalogueProxy::cacheEntryChanged);
    connect(catalogue, &Catalogue::searchCompleted, this, &CatalogueProxy::searchCompleted);
    connect(catalogue, &Catalogue::tagsReady, this, &CatalogueProxy::tagsReady);
    connect(catalogue, &Catalogue::tagListChanged, this, &CatalogueProxy::tagListChanged);
}

void CatalogueProxy::search(const QVariantMap& spec)
{
    itub::QuerySpec query;
    query.text = spec.value(QStringLiteral("text")).toString();
    query.sizeMin = spec.value(QStringLiteral("sizeMin"), -1).toLongLong();
    query.sizeMax = spec.value(QStringLiteral("sizeMax"), -1).toLongLong();
    query.widthMin = spec.value(QStringLiteral("widthMin"), 0).toInt();
    query.widthMax = spec.value(QStringLiteral("widthMax"), 0).toInt();
    query.heightMin = spec.value(QStringLiteral("heightMin"), 0).toInt();
    query.heightMax = spec.value(QStringLiteral("heightMax"), 0).toInt();
    query.resolutionPreset = spec.value(QStringLiteral("resolutionPreset")).toString();
    query.durationMinMs = spec.value(QStringLiteral("durationMinMs"), -1).toLongLong();
    query.durationMaxMs = spec.value(QStringLiteral("durationMaxMs"), -1).toLongLong();
    query.ratingMode = spec.value(QStringLiteral("ratingMode"), 0).toInt();
    query.ratingValue = spec.value(QStringLiteral("ratingValue"), 0).toInt();
    query.includeAllTags = spec.value(QStringLiteral("includeAllTags")).toStringList();
    query.includeAnyTags = spec.value(QStringLiteral("includeAnyTags")).toStringList();
    query.excludeTags = spec.value(QStringLiteral("excludeTags")).toStringList();
    query.rootId = spec.value(QStringLiteral("rootId"), -1).toLongLong();
    query.folderPrefix = spec.value(QStringLiteral("folderPrefix")).toString();
    query.viewsMin = spec.value(QStringLiteral("viewsMin"), -1).toLongLong();
    query.availability = spec.value(QStringLiteral("availability")).toStringList();
    query.sortKey = spec.value(QStringLiteral("sortKey"), QStringLiteral("added")).toString();
    query.sortDescending = spec.value(QStringLiteral("sortDescending"), false).toBool();
    query.userSort = spec.value(QStringLiteral("userSort"), false).toBool();
    QMetaObject::invokeMethod(m_catalogue, [this, query] {
        m_catalogue->search(query);
    });
}

void CatalogueProxy::clearSearch()
{
    QMetaObject::invokeMethod(m_catalogue, [this] { m_catalogue->clearSearch(); });
}

void CatalogueProxy::fetchRowsPage(const QVariantList& videoIds)
{
    QList<qint64> ids;
    for (const QVariant& id : videoIds)
        ids.append(id.toLongLong());
    QMetaObject::invokeMethod(m_catalogue, [this, ids] {
        m_catalogue->fetchRowsPage(ids);
    });
}

void CatalogueProxy::addManualTag(qint64 videoId, const QString& text)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId, text] {
        m_catalogue->addManualTag(videoId, text);
    });
}

void CatalogueProxy::removeTag(qint64 videoId, qint64 tagId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId, tagId] {
        m_catalogue->removeTag(videoId, tagId);
    });
}

void CatalogueProxy::suppressAutoTag(qint64 videoId, qint64 tagId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId, tagId] {
        m_catalogue->suppressAutoTag(videoId, tagId);
    });
}

void CatalogueProxy::resetSuppressions()
{
    QMetaObject::invokeMethod(m_catalogue, [this] { m_catalogue->resetSuppressions(); });
}

void CatalogueProxy::regenerateAutoTags(qint64 videoId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId] {
        m_catalogue->regenerateAutoTags(videoId);
    });
}

void CatalogueProxy::requestTags(qint64 videoId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId] {
        m_catalogue->requestTags(videoId);
    });
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

void CatalogueProxy::openInDefaultPlayer(qint64 videoId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId] {
        m_catalogue->openInDefaultPlayer(videoId);
    });
}

void CatalogueProxy::requestStoryboard(qint64 videoId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId] {
        m_catalogue->requestStoryboard(videoId);
    });
}

void CatalogueProxy::hoverEngage(qint64 videoId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId] {
        m_catalogue->hoverEngage(videoId);
    });
}

void CatalogueProxy::loadExistingState()
{
    QMetaObject::invokeMethod(m_catalogue, [this] {
        m_catalogue->loadExistingState();
    });
}

void CatalogueProxy::requestSampleTimes(qint64 videoId)
{
    QMetaObject::invokeMethod(m_catalogue, [this, videoId] {
        m_catalogue->requestSampleTimes(videoId);
    });
}

} // namespace itub
