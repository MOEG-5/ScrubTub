// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// UI-thread facade over the worker-thread Catalogue (TECH_SPEC.md section 2:
// the UI thread must not wait on workers). QML talks only to this object;
// calls are queued to the catalogue thread and signals are forwarded back.
#pragma once

#include "CatalogueModel.h"

#include <QObject>

namespace scrubtub {

class Catalogue;

class CatalogueProxy : public QObject {
    Q_OBJECT
    Q_PROPERTY(scrubtub::CatalogueModel* model READ model CONSTANT FINAL)

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
    Q_INVOKABLE void openInDefaultPlayer(qint64 videoId);
    Q_INVOKABLE void openFileLocation(qint64 videoId);
    Q_INVOKABLE void requestFileInfo(qint64 videoId);
    Q_INVOKABLE void requestStoryboard(qint64 videoId);
    Q_INVOKABLE void hoverEngage(qint64 videoId);
    Q_INVOKABLE void requestSampleTimes(qint64 videoId);

    // Live search and tags (milestone 3). The spec map mirrors QuerySpec
    // fields; unknown keys are ignored.
    Q_INVOKABLE void search(const QVariantMap& spec);
    Q_INVOKABLE void clearSearch();
    Q_INVOKABLE void fetchRowsPage(const QVariantList& videoIds, quint64 generation);
    Q_INVOKABLE void addManualTag(qint64 videoId, const QString& text);
    Q_INVOKABLE void removeTag(qint64 videoId, qint64 tagId);
    Q_INVOKABLE void suppressAutoTag(qint64 videoId, qint64 tagId);
    Q_INVOKABLE void resetSuppressions();
    Q_INVOKABLE void regenerateAutoTags(qint64 videoId);
    Q_INVOKABLE void requestTags(qint64 videoId);

    // Milestone 4: explicit file operations and recovery.
    Q_INVOKABLE void trashVideos(const QVariantList& videoIds);
    Q_INVOKABLE void exportBackup(const QString& destPath);
    Q_INVOKABLE void importBackup(const QString& srcPath);
    Q_INVOKABLE void clearPreviews();
    Q_INVOKABLE void requestCacheUsage();
    Q_INVOKABLE void saveUiSettings(const QVariantMap& settings);
    Q_INVOKABLE void loadExistingState();

signals:
    void filterBoundsReady(qint64 rootId, qint64 durationMs, qint64 sizeBytes);
    void rootAdded(const scrubtub::RootInfo& root);
    void rootRemoved(qint64 rootId);
    void rootRejected(const QString& reason);
    void operationFailed(const QString& message);
    void hoverSourceReady(qint64 videoId, qint64 revision, const QString& absolutePath,
                          qint64 durationMs);
    void sampleTimesReady(qint64 videoId, const QVariantList& timesMs);
    void cacheEntryChanged(qint64 videoId, const QString& profile);
    void searchCompleted(quint64 generation, const QList<qint64>& orderedIds,
                         const QString& validationError);
    void tagsReady(qint64 videoId, const QVariantList& tags);
    void tagListChanged();
    void trashResult(qint64 videoId, bool ok, const QString& reason);
    void backupExported(const QString& destPath);
    void backupImported();
    void fileInfoReady(qint64 videoId, const QVariantMap& info);
    void cacheUsageReady(qint64 bytes);
    void settingsReady(const QVariantMap& settings);

private:
    Catalogue* m_catalogue;
    CatalogueModel* m_model;
};

} // namespace scrubtub
