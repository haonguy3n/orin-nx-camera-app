#pragma once

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QVector>
#include <QWidget>

// The video surface: paints the frame and the detection boxes in a SINGLE
// paint pass.
//
// This replaces QVideoWidget, which cannot be overlaid. Its render surface
// composites above the Qt widget stack, so detection boxes drawn in a separate
// widget stayed invisible underneath the video whether that widget was a
// sibling of the video (raised) or a child of it -- both were tried on target.
// Drawing the frame ourselves removes the z-order question rather than
// fighting it, and costs one QImage conversion per frame.
class FrameView : public QWidget
{
    Q_OBJECT

public:
    explicit FrameView(QWidget *parent = nullptr);

    // Frames arrive on the GUI thread via a queued connection from the sink.
    void setImage(const QImage &image);
    // Detection boxes in normalised (0..1) frame coordinates; empty clears.
    void setBoxes(const QVector<QRectF> &normalized);
    void clear();

protected:
    void paintEvent(QPaintEvent *) override;

private:
    // Where the frame is actually drawn: aspect-fit and centred, so it may be
    // letterboxed. Boxes must be mapped into THIS rect, not the whole widget,
    // or they drift away from the faces whenever the aspect ratios differ.
    QRect targetRect() const;

    QImage m_image;
    QVector<QRectF> m_boxes;
};
