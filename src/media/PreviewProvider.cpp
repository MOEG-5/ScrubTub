// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "PreviewProvider.h"

#include <QImage>
#include <QImageReader>
#include <QQuickTextureFactory>
#include <QThread>

#include <algorithm>

namespace scrubtub {

PreviewProvider::PreviewProvider(PathResolver resolver)
    : m_resolver(std::move(resolver))
{
}

QQuickImageResponse* PreviewProvider::requestImageResponse(const QString& id,
                                                           const QSize& requestedSize)
{
    // id: "poster/<videoId>-<revision>" or "atlas/<videoId>-<revision>".
    const bool poster = id.startsWith(QLatin1String("poster/"));
    const QStringList parts = id.mid(id.indexOf(QLatin1Char('/')) + 1).split(QLatin1Char('-'));
    qint64 videoId = 0;
    qint64 revision = 0;
    if (parts.size() == 2) {
        videoId = parts.first().toLongLong();
        revision = parts.last().toLongLong();
    }
    QString path;
    if (m_resolver && videoId > 0)
        path = m_resolver(videoId, revision, poster);

    static QThreadPool* providerPool = nullptr;
    if (!providerPool) {
        providerPool = new QThreadPool();
        // Image decode budget (§6): scale with the machine, bounded so many
        // queued card thumbnails cannot swamp the cores.
        providerPool->setMaxThreadCount(
            std::clamp(QThread::idealThreadCount() / 2, 2, 8));
    }
    auto* response = new PreviewResponse(providerPool, path, requestedSize);
    response->start();
    return response;
}

PreviewResponse::PreviewResponse(QThreadPool* pool, QString path, QSize requestedSize)
    : m_pool(pool), m_path(std::move(path)), m_requestedSize(requestedSize)
{
}

PreviewResponse::~PreviewResponse()
{
    m_cancelled.store(true);
}

void PreviewResponse::start()
{
    m_pool->start([this] { run(); });
}

void PreviewResponse::run()
{
    // finished() must be delivered on the response's own (GUI) thread.
    if (m_cancelled.load()) {
        QMetaObject::invokeMethod(this, [this] { emit finished(); },
                                  Qt::QueuedConnection);
        return;
    }
    if (!m_path.isEmpty()) {
        // Decode at the requested card size instead of decoding the full
        // artifact and scaling afterwards (§6: bound decoded RAM).
        QImage image;
        // autoTransform is left at its default (off): QImage(path) does not
        // apply EXIF orientation in Qt 6, so the delivered pixels stay byte
        // for byte what the previous full-decode path produced.
        QImageReader reader(m_path);
        const QSize source = reader.size();
        if (!m_requestedSize.isEmpty() && source.isValid() && !source.isEmpty()
            && source.width() > m_requestedSize.width()) {
            QSize target = source;
            target.scale(m_requestedSize, Qt::KeepAspectRatio);
            reader.setScaledSize(target);
            image = reader.read();
        }
        if (image.isNull()) {
            // No usable header size, an unreadable format, or nothing to
            // shrink: decode in full and apply the width-bound scale.
            image = QImage(m_path);
            if (!image.isNull() && !m_requestedSize.isEmpty()
                && image.width() > m_requestedSize.width()) {
                image = image.scaledToWidth(m_requestedSize.width(),
                                            Qt::SmoothTransformation);
            }
        }
        m_image = std::move(image);
    }
    QMetaObject::invokeMethod(this, [this] { emit finished(); },
                              Qt::QueuedConnection);
}

QQuickTextureFactory* PreviewResponse::textureFactory() const
{
    if (m_image.isNull())
        return nullptr;
    return QQuickTextureFactory::textureFactoryForImage(m_image);
}

QString PreviewResponse::errorString() const
{
    return m_error;
}

} // namespace scrubtub
