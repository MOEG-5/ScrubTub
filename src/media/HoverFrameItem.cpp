// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
#include "HoverFrameItem.h"

#include <QPainter>

namespace itub {

HoverFrameItem::HoverFrameItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
}

void HoverFrameItem::paint(QPainter* painter)
{
    if (m_frame.isNull())
        return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(boundingRect(), m_frame);
}

void HoverFrameItem::setFrame(const QImage& frame)
{
    if (m_frame.cacheKey() == frame.cacheKey())
        return;
    m_frame = frame;
    emit frameChanged();
    update();
}

} // namespace itub
