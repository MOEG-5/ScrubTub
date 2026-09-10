// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// Paints the hovered card's live paused-player frame. One instance exists in
// the UI (a single overlay over the hovered card), fed by HoverSession.
#pragma once

#include <QImage>
#include <QQuickPaintedItem>

namespace scrubtub {

class HoverFrameItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QImage frame READ frame WRITE setFrame NOTIFY frameChanged FINAL)

public:
    explicit HoverFrameItem(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;
    QImage frame() const { return m_frame; }
    void setFrame(const QImage& frame);

signals:
    void frameChanged();

private:
    QImage m_frame;
};

} // namespace scrubtub
