#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QUrl>
#include <QWidget>

class QLabel;
class QMediaPlayer;
class QStackedWidget;
class QTimer;
class QVideoSink;
class QVideoWidget;

// One camera's video surface: QMediaPlayer + QVideoWidget with an idle
// black placeholder. Qt 6's QVideoWidget renders uninitialized GPU memory
// when idle, so a QStackedWidget swaps between a black placeholder and the
// live video surface (raised only when frames actually arrive).
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

signals:
    void videoFrameAvailable();

private:
    QString m_name;
    QMediaPlayer *m_player = nullptr;
    QVideoWidget *m_video = nullptr;
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
};
