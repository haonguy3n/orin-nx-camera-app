#include "frameview.h"

#include <QPainter>

FrameView::FrameView(QWidget *parent) : QWidget(parent)
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);  // we always fill, skip the erase
}

void FrameView::setImage(const QImage &image)
{
    m_image = image;
    update();
}

void FrameView::setBoxes(const QVector<QRectF> &normalized)
{
    m_boxes = normalized;
    update();
}

void FrameView::clear()
{
    m_image = QImage();
    m_boxes.clear();
    update();
}

QRect FrameView::targetRect() const
{
    if (m_image.isNull())
        return {};
    QSize scaled = m_image.size();
    scaled.scale(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - scaled.width()) / 2,
                        (height() - scaled.height()) / 2),
                 scaled);
}

void FrameView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (m_image.isNull())
        return;

    const QRect target = targetRect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, m_image);

    if (m_boxes.isEmpty())
        return;
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 220, 0), 2));
    painter.setBrush(Qt::NoBrush);
    for (const QRectF &b : m_boxes) {
        painter.drawRect(QRectF(target.x() + b.x() * target.width(),
                                target.y() + b.y() * target.height(),
                                b.width() * target.width(),
                                b.height() * target.height()));
    }
}
