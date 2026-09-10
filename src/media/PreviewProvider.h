// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Async image provider for posters and storyboard atlases (TECH_SPEC.md
// sections 2 and 6): asynchronous disk reads with bounded decode sizes,
// immutable cache keys (videoId-revision-profile). Served through
// CatalogueProxy so QML never touches the worker thread.
#pragma once

#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>

#include <QHash>
#include <QMutex>
#include <QThreadPool>

#include <atomic>

namespace scrubtub {

class Catalogue;

// Serves "image://previews/poster/<id>-<rev>" and
// "image://previews/atlas/<id>-<rev>". Cache file locations are resolved on
// the catalogue thread via a path resolver callback (SQLite lives there).
class PreviewProvider : public QQuickAsyncImageProvider {
public:
    using PathResolver = std::function<QString(qint64 videoId, qint64 revision,
                                               bool poster)>;

    explicit PreviewProvider(PathResolver resolver);

    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;

private:
    PathResolver m_resolver;
};

class PreviewResponse : public QQuickImageResponse {
public:
    PreviewResponse(QThreadPool* pool, QString path, QSize requestedSize);
    ~PreviewResponse() override;

    QQuickTextureFactory* textureFactory() const override;
    QString errorString() const override;

    void start();
    void cancel() { m_cancelled.store(true); }

private:
    void run();

    QThreadPool* m_pool;
    QString m_path;
    QSize m_requestedSize;
    QImage m_image;
    QString m_error;
    std::atomic_bool m_cancelled{false};
};

} // namespace scrubtub
