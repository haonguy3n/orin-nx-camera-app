#include "videopane.h"

#include <QLabel>
#include <QMediaPlayer>
#include <QPalette>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>

VideoPane::VideoPane(const QString &name, QWidget *parent)
    : QWidget(parent)
    , m_name(name)
{
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_placeholder = new QWidget(this);
    m_placeholder->setAutoFillBackground(true);
    QPalette pal2 = m_placeholder->palette();
    pal2.setColor(QPalette::Window, Qt::black);
    m_placeholder->setPalette(pal2);

    m_video = new QVideoWidget(this);

    m_stack = new QStackedWidget(this);
    m_stack->setMinimumSize(320, 240);
    m_stack->addWidget(m_placeholder);
    m_stack->addWidget(m_video);
    showVideo(false);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setObjectName("mono");

    layout->addWidget(m_stack, 1);
    layout->addWidget(m_status);

    m_player = new QMediaPlayer(this);
    m_player->setVideoOutput(m_video);

    setStatusText("disconnected");

    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &msg) {
                setStatusText("error: " + msg);
                showVideo(false);
            });

    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                switch (status) {
                case QMediaPlayer::LoadingMedia:
                case QMediaPlayer::BufferingMedia:
                case QMediaPlayer::StalledMedia:
                    setStatusText("connecting…");
                    break;
                case QMediaPlayer::BufferedMedia:
                    setStatusText("playing");
                    break;
                case QMediaPlayer::EndOfMedia:
                    setStatusText("stream ended");
                    showVideo(false);
                    break;
                case QMediaPlayer::InvalidMedia:
                    setStatusText("error: " + m_player->errorString());
                    showVideo(false);
                    break;
                case QMediaPlayer::NoMedia:
                    setStatusText("disconnected");
                    showVideo(false);
                    break;
                default:
                    break;
                }
            });

    connect(m_video->videoSink(), &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                if (frame.isValid()) {
                    showVideo(true);
                    emit videoFrameAvailable();
                }
            });
}

void VideoPane::start(const QUrl &url)
{
    m_player->stop();
    m_player->setSource(QUrl());
    m_player->setSource(url);
    m_player->play();
}

void VideoPane::stop()
{
    m_player->stop();
    m_player->setSource(QUrl());
    setStatusText("disconnected");
    showVideo(false);
}

void VideoPane::restart(const QUrl &url)
{
    setStatusText("connecting…");
    showVideo(false);
    start(url);
}

void VideoPane::setStatusText(const QString &text)
{
    m_status->setText(m_name + ": " + text);
}

QVideoSink *VideoPane::videoSink() const
{
    return m_video->videoSink();
}

void VideoPane::showVideo(bool live)
{
    m_stack->setCurrentWidget(live ? m_video : m_placeholder);
}
