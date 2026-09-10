// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "HoverFrameItem.h"

#include <QPainter>

namespace scrubtub {

HoverFrameItem::HoverFrameItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
}

void HoverFrameItem::paint(QPainter* painter)
{
    if (m_frame.isNull())
        return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QSizeF size = QSizeF(m_frame.size()).scaled(boundingRect().size(), Qt::KeepAspectRatio);
    painter->drawImage(QRectF(QPointF((width() - size.width()) / 2,
                                    (height() - size.height()) / 2), size), m_frame);
}

void HoverFrameItem::setFrame(const QImage& frame)
{
    if (m_frame.cacheKey() == frame.cacheKey())
        return;
    m_frame = frame;
    emit frameChanged();
    update();
}

} // namespace scrubtub
