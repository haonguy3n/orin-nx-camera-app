#include "videopane.h"

#include "frameview.h"

#include <QLabel>
#include <QMediaPlayer>
#include <QPalette>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>

VideoPane::VideoPane(const QString &name, QWidget *parent)
    : QWidget(parent)
    , m_name(name)
{
    setObjectName(QStringLiteral("videoPane"));
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    m_placeholder = new QWidget(this);
    m_placeholder->setObjectName(QStringLiteral("videoPlaceholder"));
    m_placeholder->setAutoFillBackground(true);
    QPalette pal2 = m_placeholder->palette();
    pal2.setColor(QPalette::Window, Qt::black);
    m_placeholder->setPalette(pal2);
    auto *emptyLayout = new QVBoxLayout(m_placeholder);
    emptyLayout->setAlignment(Qt::AlignCenter);
    emptyLayout->setSpacing(8);
    auto *cameraGlyph = new QLabel(QStringLiteral("◉"), m_placeholder);
    cameraGlyph->setObjectName(QStringLiteral("cameraGlyph"));
    cameraGlyph->setAlignment(Qt::AlignCenter);
    auto *emptyTitle = new QLabel(QStringLiteral("No live video"), m_placeholder);
    emptyTitle->setObjectName(QStringLiteral("emptyTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    auto *emptyHint = new QLabel(
        QStringLiteral("Connect to a camera to begin monitoring"), m_placeholder);
    emptyHint->setObjectName(QStringLiteral("emptyHint"));
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(cameraGlyph);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyHint);

    m_view = new FrameView(this);
    // Our own sink, so BOTH transports (QMediaPlayer for RTSP and the
    // secure USB decoder) land in one place we can paint from.
    m_sink = new QVideoSink(this);

    m_stack = new QStackedWidget(this);
    m_stack->setMinimumSize(320, 240);
    m_stack->addWidget(m_placeholder);
    m_stack->addWidget(m_view);
    showVideo(false);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setObjectName("mono");
    m_status->setProperty("role", "videoStatus");

    layout->addWidget(m_stack, 1);
    layout->addWidget(m_status);

    m_player = new QMediaPlayer(this);
    m_player->setVideoSink(m_sink);

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

    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                if (frame.isValid() && m_active) {
                    // Convert once here; FrameView paints this image and the
                    // detection boxes together (see frameview.h for why).
                    m_view->setImage(frame.toImage());
                    // Emitted from a decode worker thread; this slot runs on
                    // the GUI thread via a queued connection, so the counter
                    // needs no lock. The m_active guard drops frames that were
                    // already queued when stop() ran.
                    ++m_framesSinceTick;
                    if (!m_live) {
                        m_live = true;
                        m_fpsClock.restart();
                        // The secure USB path pushes frames straight to the
                        // sink with no QMediaPlayer state change, so nothing
                        // else moves the label off "waiting for frames".
                        setStatusText(QStringLiteral("playing"));
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
    m_active = true;
    m_player->stop();
    m_player->setSource(QUrl());
    m_player->setSource(url);
    m_player->play();
}

void VideoPane::startExternal()
{
    m_active = true;
    m_player->stop();
    m_player->setSource(QUrl());
    setStatusText("waiting for frames…");
    showVideo(false);  // first pushed frame flips this via videoFrameChanged
}

void VideoPane::stop()
{
    m_active = false;
    m_player->stop();
    m_player->setSource(QUrl());
    m_live = false;
    m_fps = 0.0;
    if (m_view)
        m_view->clear();  // drop the last frame and any stale boxes
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
    QString displayName = m_name;
    if (m_name.startsWith(QStringLiteral("cam")))
        displayName = QStringLiteral("CAMERA %1").arg(m_name.mid(3).toInt() + 1);
    QString line = displayName + QStringLiteral("   •   ") + m_stateText;
    if (m_live && m_fps > 0.0)
        line += QStringLiteral(" — %1 fps").arg(m_fps, 0, 'f', 1);
    m_status->setText(line);
}

QVideoSink *VideoPane::videoSink() const
{
    return m_sink;
}

void VideoPane::setFaces(const QVector<QRectF> &normalized)
{
    // Straight to the video surface: it draws the boxes in the same paint as
    // the frame, so there is no overlay widget to keep positioned or raised.
    if (m_view)
        m_view->setBoxes(normalized);
}

void VideoPane::showVideo(bool live)
{
    m_stack->setCurrentWidget(live ? m_view : m_placeholder);
}
