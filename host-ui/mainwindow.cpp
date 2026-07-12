#include "mainwindow.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoWidget>

#include "controlclient.h"

namespace {

constexpr quint16 kControlPort = 8555;  // PROTOCOL.md default
constexpr int kStatusPollMs = 2000;

} // namespace

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

    // Two video panes side by side, control panel on the right.
    auto *paneLayout = new QHBoxLayout;
    paneLayout->addWidget(createPane(m_panes[0], QStringLiteral("cam0")), 1);
    paneLayout->addWidget(createPane(m_panes[1], QStringLiteral("cam1")), 1);
    paneLayout->addWidget(createControlPanel());
    rootLayout->addLayout(paneLayout, 1);

    setCentralWidget(central);
    resize(1560, 560);

    // Control channel (JSON over TCP, ../proto/PROTOCOL.md) + status poll.
    m_control = new ControlClient(this);
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusPollMs);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::pollStatus);

    connect(m_control, &ControlClient::connected, this, [this]() {
        m_controlStatus->setText(QStringLiteral("control: connected"));
        m_errorLabel->clear();
        m_controlsPopulated = false;  // repopulate from the first get-status
        setCameraControlsEnabled(true);
        m_statusTimer->start();
        pollStatus();
    });

    connect(m_control, &ControlClient::disconnected, this, [this]() {
        m_controlStatus->setText(QStringLiteral("control: disconnected"));
        m_deviceStatus->clear();
        setCameraControlsEnabled(false);
        m_statusTimer->stop();
    });

    connect(m_control, &ControlClient::errorOccurred, this,
            [this](const QString &message) {
                m_errorLabel->setText(QStringLiteral("control: %1").arg(message));
                if (!m_control->isConnected()) {
                    // Connect failure: no disconnected() will follow.
                    m_controlStatus->setText(
                        QStringLiteral("control: disconnected"));
                    setCameraControlsEnabled(false);
                    m_statusTimer->stop();
                }
            });

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

QWidget *MainWindow::createControlPanel()
{
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);

    // Device status: control channel state, per-camera poll results,
    // and the last request error (non-modal, no dialogs).
    auto *statusGroup = new QGroupBox(QStringLiteral("Device"), panel);
    auto *statusLayout = new QVBoxLayout(statusGroup);
    m_controlStatus =
        new QLabel(QStringLiteral("control: disconnected"), statusGroup);
    m_deviceStatus = new QLabel(statusGroup);
    m_deviceStatus->setWordWrap(true);
    m_errorLabel = new QLabel(statusGroup);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #c04040;"));
    statusLayout->addWidget(m_controlStatus);
    statusLayout->addWidget(m_deviceStatus);
    statusLayout->addWidget(m_errorLabel);
    layout->addWidget(statusGroup);

    layout->addWidget(createCameraGroup(0));
    layout->addWidget(createCameraGroup(1));
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFixedWidth(300);
    return scroll;
}

QWidget *MainWindow::createCameraGroup(int index)
{
    CameraControls &controls = m_cameraControls[index];
    controls.group = new QGroupBox(QStringLiteral("cam%1").arg(index));
    auto *form = new QFormLayout(controls.group);

    controls.exposure = new QSpinBox(controls.group);
    controls.exposure->setRange(0, 1000000);
    controls.exposure->setSuffix(QStringLiteral(" µs"));
    controls.exposure->setSpecialValueText(QStringLiteral("auto")); // 0 = auto
    form->addRow(QStringLiteral("Exposure:"), controls.exposure);

    controls.gain = new QDoubleSpinBox(controls.group);
    controls.gain->setRange(0.0, 480.0);
    controls.gain->setDecimals(1);
    controls.gain->setSpecialValueText(QStringLiteral("auto")); // 0 = auto
    form->addRow(QStringLiteral("Gain:"), controls.gain);

    // Trigger has no readable "current" semantic worth fighting over;
    // "(unchanged)" sends nothing, everything else maps to mode = row - 1.
    controls.trigger = new QComboBox(controls.group);
    controls.trigger->addItem(QStringLiteral("(unchanged)"));
    controls.trigger->addItem(QStringLiteral("0 disabled"));
    controls.trigger->addItem(QStringLiteral("1 external"));
    controls.trigger->addItem(QStringLiteral("2 pulse width"));
    controls.trigger->addItem(QStringLiteral("3 self"));
    controls.trigger->addItem(QStringLiteral("4 single"));
    controls.trigger->addItem(QStringLiteral("5 sync"));
    controls.trigger->addItem(QStringLiteral("6 stream edge"));
    controls.trigger->addItem(QStringLiteral("7 stream level"));
    form->addRow(QStringLiteral("Trigger:"), controls.trigger);

    connect(controls.exposure, &QSpinBox::editingFinished, this,
            [this, index]() { applyExposure(index); });
    connect(controls.gain, &QDoubleSpinBox::editingFinished, this,
            [this, index]() { applyGain(index); });
    // activated (not currentIndexChanged): user picks only, never programmatic.
    connect(controls.trigger, &QComboBox::activated, this,
            [this, index](int item) { applyTrigger(index, item); });

    controls.group->setEnabled(false);
    return controls.group;
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
    m_control->connectToDevice(controlHost(), kControlPort);
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
    m_control->disconnectFromDevice();
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

QString MainWindow::controlHost() const
{
    QString text = m_hostEdit->text().trimmed();
    if (text.isEmpty())
        text = QStringLiteral("192.168.55.1");

    // Same host as the video; the control port is always kControlPort.
    if (text.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive))
        return QUrl(text).host();
    return text;
}

void MainWindow::setCameraControlsEnabled(bool enabled)
{
    for (CameraControls &controls : m_cameraControls)
        controls.group->setEnabled(enabled);
}

void MainWindow::pollStatus()
{
    m_control->sendRequest(
        QStringLiteral("get-status"), QJsonObject(),
        [this](const QJsonObject &result, const QJsonObject &error) {
            if (!error.isEmpty()) {
                showRequestError(QStringLiteral("get-status"), error);
                return;
            }

            QStringList lines;
            const QJsonArray cameras =
                result.value(QStringLiteral("cameras")).toArray();
            for (const QJsonValue &value : cameras) {
                const QJsonObject camera = value.toObject();
                const int index =
                    camera.value(QStringLiteral("index")).toInt(-1);
                if (index < 0 || index > 1)
                    continue;

                if (camera.value(QStringLiteral("streaming")).toBool()) {
                    const qint64 frames = static_cast<qint64>(
                        camera.value(QStringLiteral("frames")).toDouble());
                    lines << QStringLiteral("cam%1: streaming, %2 frames")
                                 .arg(index)
                                 .arg(frames);
                } else {
                    lines << QStringLiteral("cam%1: idle").arg(index);
                }

                // Seed the widgets from the first status only — after that
                // the user owns them; don't fight edits on every poll.
                if (!m_controlsPopulated) {
                    CameraControls &controls = m_cameraControls[index];
                    const int us =
                        camera.value(QStringLiteral("exposure")).toInt();
                    const double gain =
                        camera.value(QStringLiteral("gain")).toDouble();
                    controls.exposure->setValue(us);
                    controls.exposure->setProperty("lastSent", us);
                    controls.gain->setValue(gain);
                    controls.gain->setProperty("lastSent", gain);
                }
            }
            m_controlsPopulated = true;
            m_deviceStatus->setText(lines.join(QLatin1Char('\n')));
        });
}

void MainWindow::applyExposure(int camera)
{
    QSpinBox *box = m_cameraControls[camera].exposure;
    const int us = box->value();
    // editingFinished also fires on plain focus loss — only send changes.
    const QVariant lastSent = box->property("lastSent");
    if (lastSent.isValid() && lastSent.toInt() == us)
        return;
    box->setProperty("lastSent", us);

    QJsonObject params;
    params.insert(QStringLiteral("camera"), camera);
    params.insert(QStringLiteral("us"), us);
    m_control->sendRequest(
        QStringLiteral("set-exposure"), params,
        [this, camera](const QJsonObject &, const QJsonObject &error) {
            if (!error.isEmpty())
                showRequestError(
                    QStringLiteral("cam%1 set-exposure").arg(camera), error);
        });
}

void MainWindow::applyGain(int camera)
{
    QDoubleSpinBox *box = m_cameraControls[camera].gain;
    const double gain = box->value();
    const QVariant lastSent = box->property("lastSent");
    if (lastSent.isValid() && lastSent.toDouble() == gain)
        return;
    box->setProperty("lastSent", gain);

    QJsonObject params;
    params.insert(QStringLiteral("camera"), camera);
    params.insert(QStringLiteral("gain"), gain);
    m_control->sendRequest(
        QStringLiteral("set-gain"), params,
        [this, camera](const QJsonObject &, const QJsonObject &error) {
            if (!error.isEmpty())
                showRequestError(QStringLiteral("cam%1 set-gain").arg(camera),
                                 error);
        });
}

void MainWindow::applyTrigger(int camera, int item)
{
    const int mode = item - 1; // item 0 is "(unchanged)": send nothing
    if (mode < 0)
        return;

    QJsonObject params;
    params.insert(QStringLiteral("camera"), camera);
    params.insert(QStringLiteral("mode"), mode);
    m_control->sendRequest(
        QStringLiteral("set-trigger"), params,
        [this, camera](const QJsonObject &, const QJsonObject &error) {
            if (!error.isEmpty())
                showRequestError(
                    QStringLiteral("cam%1 set-trigger").arg(camera), error);
        });
}

void MainWindow::showRequestError(const QString &what, const QJsonObject &error)
{
    if (!m_control->isConnected())
        return; // teardown noise; the control status line already says it
    m_errorLabel->setText(QStringLiteral("%1: %2").arg(
        what, error.value(QStringLiteral("message")).toString()));
}
