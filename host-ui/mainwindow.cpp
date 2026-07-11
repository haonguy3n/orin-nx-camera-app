#include "mainwindow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVideoWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("camera-viewer"));

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);

    // Top bar: host/URL entry + connect/disconnect toggle.
    auto *topBar = new QHBoxLayout;
    auto *hostLabel = new QLabel(QStringLiteral("Device:"), central);
    m_hostEdit = new QLineEdit(QStringLiteral("192.168.55.1"), central);
    m_hostEdit->setToolTip(
        QStringLiteral("Device IP/hostname, or an rtsp://host:port base URL"));
    m_connectButton = new QPushButton(QStringLiteral("Connect"), central);
    topBar->addWidget(hostLabel);
    topBar->addWidget(m_hostEdit, 1);
    topBar->addWidget(m_connectButton);
    rootLayout->addLayout(topBar);

    // Two video panes side by side.
    auto *paneLayout = new QHBoxLayout;
    paneLayout->addWidget(createPane(m_panes[0], QStringLiteral("cam0")), 1);
    paneLayout->addWidget(createPane(m_panes[1], QStringLiteral("cam1")), 1);
    rootLayout->addLayout(paneLayout, 1);

    setCentralWidget(central);
    resize(1280, 560);

    connect(m_connectButton, &QPushButton::clicked, this, [this]() {
        if (m_connected)
            disconnectStreams();
        else
            connectStreams();
    });

    connect(m_hostEdit, &QLineEdit::returnPressed, this, [this]() {
        if (m_connected)
            disconnectStreams();
        connectStreams();
    });
}

QWidget *MainWindow::createPane(Pane &pane, const QString &name)
{
    pane.name = name;

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    pane.video = new QVideoWidget(container);
    pane.video->setMinimumSize(320, 240);

    pane.status = new QLabel(container);
    pane.status->setWordWrap(true);

    layout->addWidget(pane.video, 1);
    layout->addWidget(pane.status);

    pane.player = new QMediaPlayer(this);
    pane.player->setVideoOutput(pane.video);

    setStatus(pane, QStringLiteral("disconnected"));

    Pane *p = &pane; // stable: member array, same lifetime as this window

    connect(pane.player, &QMediaPlayer::errorOccurred, this,
            [this, p](QMediaPlayer::Error, const QString &errorString) {
                setStatus(*p, QStringLiteral("error: %1").arg(errorString));
            });

    connect(pane.player, &QMediaPlayer::mediaStatusChanged, this,
            [this, p](QMediaPlayer::MediaStatus status) {
                switch (status) {
                case QMediaPlayer::LoadingMedia:
                case QMediaPlayer::BufferingMedia:
                case QMediaPlayer::StalledMedia:
                    setStatus(*p, QStringLiteral("connecting…"));
                    break;
                case QMediaPlayer::BufferedMedia:
                    setStatus(*p, QStringLiteral("playing"));
                    break;
                case QMediaPlayer::EndOfMedia:
                    setStatus(*p, QStringLiteral("stream ended"));
                    break;
                case QMediaPlayer::InvalidMedia:
                    setStatus(*p, QStringLiteral("error: %1")
                                      .arg(p->player->errorString()));
                    break;
                case QMediaPlayer::NoMedia:
                    setStatus(*p, QStringLiteral("disconnected"));
                    break;
                default:
                    break;
                }
            });
    return container;
}

void MainWindow::connectStreams()
{
    for (int i = 0; i < 2; ++i) {
        Pane &pane = m_panes[i];
        setStatus(pane, QStringLiteral("connecting…"));
        pane.player->setSource(streamUrl(i));
        // Note: Qt 6 uses the FFmpeg media backend; there is no public
        // low-latency knob on QMediaPlayer. Acceptable for milestone 1.
        pane.player->play();
    }
    m_connected = true;
    m_connectButton->setText(QStringLiteral("Disconnect"));
}

void MainWindow::disconnectStreams()
{
    for (Pane &pane : m_panes) {
        pane.player->stop();
        pane.player->setSource(QUrl());
        setStatus(pane, QStringLiteral("disconnected"));
    }
    m_connected = false;
    m_connectButton->setText(QStringLiteral("Connect"));
}

void MainWindow::setStatus(Pane &pane, const QString &text)
{
    pane.status->setText(QStringLiteral("%1: %2").arg(pane.name, text));
}

QUrl MainWindow::streamUrl(int index) const
{
    QString text = m_hostEdit->text().trimmed();
    if (text.isEmpty())
        text = QStringLiteral("192.168.55.1");

    if (text.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive)) {
        // Full base URL given, e.g. rtsp://192.168.55.1:8554
        while (text.endsWith(QLatin1Char('/')))
            text.chop(1);
        return QUrl(QStringLiteral("%1/cam%2").arg(text).arg(index));
    }

    // Bare host/IP: use the default device RTSP port and mount points.
    return QUrl(QStringLiteral("rtsp://%1:8554/cam%2").arg(text).arg(index));
}
