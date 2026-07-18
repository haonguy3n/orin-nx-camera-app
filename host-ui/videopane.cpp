#include "videopane.h"

#include <QLabel>
#include <QMediaPlayer>
#include <QPalette>
#include <QStackedWidget>
#include <QTimer>
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
                    // Emitted from a decode worker thread; this slot runs on
                    // the GUI thread via a queued connection, so the counter
                    // needs no lock.
                    ++m_framesSinceTick;
                    if (!m_live) {
                        m_live = true;
                        m_fpsClock.restart();
                    }
                    showVideo(true);
                    emit videoFrameAvailable();
                }
            });

    // Recompute displayed fps once a second.
    m_fpsTimer = new QTimer(this);
    m_fpsTimer->setInterval(1000);
    connect(m_fpsTimer, &QTimer::timeout, this, [this] {
        if (!m_live)
            return;
        const qint64 elapsed = m_fpsClock.restart();
        if (elapsed > 0)
            m_fps = m_framesSinceTick * 1000.0 / static_cast<double>(elapsed);
        m_framesSinceTick = 0;
        // A whole tick with no frame: the stream stalled -- stop claiming fps.
        if (m_fps == 0.0)
            m_live = false;
        refreshStatus();
    });
    m_fpsTimer->start();
}

void VideoPane::start(const QUrl &url)
{
    m_player->stop();
    m_player->setSource(QUrl());
    m_player->setSource(url);
    m_player->play();
}

void VideoPane::startExternal()
{
    m_player->stop();
    m_player->setSource(QUrl());
    setStatusText("waiting for frames…");
    showVideo(false);  // first pushed frame flips this via videoFrameChanged
}

void VideoPane::stop()
{
    m_player->stop();
    m_player->setSource(QUrl());
    m_live = false;
    m_fps = 0.0;
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
    m_stateText = text;
    refreshStatus();
}

void VideoPane::refreshStatus()
{
    QString line = m_name + ": " + m_stateText;
    if (m_live && m_fps > 0.0)
        line += QStringLiteral(" — %1 fps").arg(m_fps, 0, 'f', 1);
    m_status->setText(line);
}

QVideoSink *VideoPane::videoSink() const
{
    return m_video->videoSink();
}

void VideoPane::showVideo(bool live)
{
    m_stack->setCurrentWidget(live ? m_video : m_placeholder);
}
