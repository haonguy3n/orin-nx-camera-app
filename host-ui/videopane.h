#pragma once

#include <QElapsedTimer>
#include <QRectF>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QWidget>

class FrameView;
class QLabel;
class QMediaPlayer;
class QStackedWidget;
class QTimer;
class QVideoSink;

// One camera's video surface: QMediaPlayer or the secure USB decoder feeds
// a QVideoSink, and FrameView paints the frames plus any detection boxes.
// A QStackedWidget swaps in a black placeholder when no video is live.
class VideoPane : public QWidget
{
    Q_OBJECT

public:
    explicit VideoPane(const QString &name, QWidget *parent = nullptr);

    void start(const QUrl &url);
    // Secure USB mode: frames are pushed into videoSink() by the bridge, so
    // the internal player must stay out of the way.
    void startExternal();
    void stop();
    void restart(const QUrl &url);
    void setStatusText(const QString &text);
    QVideoSink *videoSink() const;
    QString name() const { return m_name; }

    // Detection boxes to draw over the video, in normalised (0..1) coords.
    // Empty clears the overlay. Delivered from the secure USB metadata channel.
    void setFaces(const QVector<QRectF> &normalized);

signals:
    void videoFrameAvailable();

private:
    QString m_name;
    QMediaPlayer *m_player = nullptr;
    FrameView *m_view = nullptr;
    QVideoSink *m_sink = nullptr;
    QLabel *m_status = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_placeholder = nullptr;

    void showVideo(bool live);

    // Displayed frame rate, measured at the sink so it is true end-to-end
    // fps for either transport (network QMediaPlayer or the secure USB
    // decoder both feed this same QVideoSink).
    void refreshStatus();
    QString m_stateText = QStringLiteral("disconnected");
    QTimer *m_fpsTimer = nullptr;
    QElapsedTimer m_fpsClock;
    int m_framesSinceTick = 0;
    double m_fps = 0.0;
    bool m_live = false;
    // False once stopped: a decoder worker can deliver a queued frame after
    // stop(), which would otherwise re-latch the status to "playing".
    bool m_active = false;
};
