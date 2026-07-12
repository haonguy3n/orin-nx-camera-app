#pragma once

#include <QString>
#include <QUrl>
#include <QWidget>

class QLabel;
class QMediaPlayer;
class QStackedWidget;
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
};
