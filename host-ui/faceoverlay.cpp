#include "faceoverlay.h"

#include <QPainter>

FaceOverlay::FaceOverlay(QWidget *parent) : QWidget(parent)
{
    // Draw-only: never intercept clicks meant for the video beneath, and no
    // background of its own so the video shows through.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
}

void FaceOverlay::setBoxes(const QVector<QRectF> &normalized)
{
    m_boxes = normalized;
    update();
}

void FaceOverlay::paintEvent(QPaintEvent *)
{
    if (m_boxes.isEmpty())
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 220, 0), 2));
    const qreal w = width();
    const qreal h = height();
    for (const QRectF &b : m_boxes)
        painter.drawRect(QRectF(b.x() * w, b.y() * h, b.width() * w, b.height() * h));
}
