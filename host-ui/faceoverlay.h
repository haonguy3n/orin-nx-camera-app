#pragma once

#include <QRectF>
#include <QVector>
#include <QWidget>

// Transparent overlay that draws detection boxes over the video. Boxes are
// held in normalised (0..1) coordinates so they are resolution-independent:
// the source resolution the device detected at is divided out before they
// reach here, and paintEvent scales to whatever size the widget currently is.
//
// Deliberately plain (a green rectangle per box). Visual styling is the
// host-UI's to refine; this owns only the geometry/plumbing.
class FaceOverlay : public QWidget
{
public:
    explicit FaceOverlay(QWidget *parent = nullptr);

    // Replace the drawn boxes (normalised coordinates) and repaint.
    void setBoxes(const QVector<QRectF> &normalized);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<QRectF> m_boxes;
};
