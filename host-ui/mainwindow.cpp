#include "mainwindow.h"

#include <QCheckBox>
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
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoWidget>

#include "controlclient.h"
#include "discoveryclient.h"

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

    // Top bar: host/URL entry + connect/disconnect toggle + discovery.
    auto *topBar = new QHBoxLayout;
    auto *hostLabel = new QLabel(QStringLiteral("Device:"), central);
    m_hostEdit = new QLineEdit(QStringLiteral("192.168.55.1"), central);
    m_hostEdit->setToolTip(
        QStringLiteral("Device IP/hostname, or an rtsp://host:port base URL"));
    m_connectButton = new QPushButton(QStringLiteral("Connect"), central);
    m_discoverButton = new QPushButton(QStringLiteral("Discover"), central);
    m_discoverButton->setToolTip(
        QStringLiteral("find devices via UDP broadcast (port 8556)"));
    m_discoverMenu = new QMenu(m_discoverButton);
    topBar->addWidget(hostLabel);
    topBar->addWidget(m_hostEdit, 1);
    topBar->addWidget(m_connectButton);
    topBar->addWidget(m_discoverButton);
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

    // Device discovery (UDP broadcast, ../proto/PROTOCOL.md "Discovery").
    m_discovery = new DiscoveryClient(this);
    connect(m_discoverButton, &QPushButton::clicked, this,
            &MainWindow::runDiscovery);

    connect(m_discovery, &DiscoveryClient::deviceFound, this,
            [this](const QString &address, const QJsonObject &info) {
                const QString version =
                    info.value(QStringLiteral("version"))
                        .toString(QStringLiteral("?"));
                QAction *action = m_discoverMenu->addAction(
                    QStringLiteral("%1 (%2)").arg(address, version));
                connect(action, &QAction::triggered, this,
                        [this, address]() { m_hostEdit->setText(address); });

                // Fill the host box, but don't fight the user: only replace
                // an empty box, the shipped default, or an earlier discovery.
                const QString current = m_hostEdit->text().trimmed();
                if (current.isEmpty()
                    || current == QStringLiteral("192.168.55.1")
                    || m_discoveredHosts.contains(current))
                    m_hostEdit->setText(address);
                if (!m_discoveredHosts.contains(address))
                    m_discoveredHosts.append(address);
            });

    connect(m_discovery, &DiscoveryClient::finished, this, [this]() {
        m_discoverButton->setEnabled(true);
        const int found = m_discoverMenu->actions().count();
        if (found == 0)
            m_errorLabel->setText(QStringLiteral("no devices found"));
        else if (found > 1)
            // Several devices: let the user pick which fills the host box.
            m_discoverMenu->popup(m_discoverButton->mapToGlobal(
                QPoint(0, m_discoverButton->height())));
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
    m_syncCheck = new QCheckBox(QStringLiteral("Sync trigger"), statusGroup);
    m_syncCheck->setToolTip(QStringLiteral(
        "hardware-synchronized capture: all cameras → external trigger"));
    m_syncCheck->setEnabled(false);
    statusLayout->addWidget(m_controlStatus);
    statusLayout->addWidget(m_deviceStatus);
    statusLayout->addWidget(m_syncCheck);
    statusLayout->addWidget(m_errorLabel);
    layout->addWidget(statusGroup);

    // clicked (not toggled): user toggles only, never programmatic reverts.
    connect(m_syncCheck, &QCheckBox::clicked, this,
            [this](bool enabled) { applySync(enabled); });

    layout->addWidget(createCameraGroup(0));
    layout->addWidget(createCameraGroup(1));
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFixedWidth(320);  // leave room for the vertical scrollbar
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
    // argus: analog gain multiplier (1-16); v4l2: VC driver milli-dB
    // (0-48000 = 0-48 dB, step 100).
    controls.gain->setRange(0.0, 48000.0);
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

    controls.fire = new QPushButton(QStringLiteral("Fire"), controls.group);
    controls.fire->setToolTip(QStringLiteral(
        "software single trigger — set trigger mode 4 first"));
    form->addRow(controls.fire);

    // Digital zoom (set-zoom, GPU crop + upscale). Center crop only; the
    // protocol's x/y pan stays protocol-only for now.
    controls.zoom = new QDoubleSpinBox(controls.group);
    controls.zoom->setRange(1.0, 8.0);
    controls.zoom->setDecimals(1);
    controls.zoom->setSingleStep(0.5);
    controls.zoom->setValue(1.0);  // 1 = full field of view
    controls.zoom->setSuffix(QStringLiteral("x"));
    controls.zoom->setProperty("lastSent", 1.0);
    form->addRow(QStringLiteral("Zoom:"), controls.zoom);

    // ISP overrides (set-isp, argus source only — the server rejects the
    // whole group for v4l2/test). Same conventions as above: combos send on
    // user activation, spin boxes on editingFinished with a lastSent guard.
    auto *ispGroup = new QGroupBox(QStringLiteral("ISP"), controls.group);
    auto *ispForm = new QFormLayout(ispGroup);

    controls.wbMode = new QComboBox(ispGroup);
    controls.wbMode->addItem(QStringLiteral("(unchanged)"));
    controls.wbMode->addItem(QStringLiteral("0 off"));
    controls.wbMode->addItem(QStringLiteral("1 auto"));
    controls.wbMode->addItem(QStringLiteral("2 incandescent"));
    controls.wbMode->addItem(QStringLiteral("3 fluorescent"));
    controls.wbMode->addItem(QStringLiteral("4 warm-fluorescent"));
    controls.wbMode->addItem(QStringLiteral("5 daylight"));
    controls.wbMode->addItem(QStringLiteral("6 cloudy-daylight"));
    controls.wbMode->addItem(QStringLiteral("7 twilight"));
    controls.wbMode->addItem(QStringLiteral("8 shade"));
    controls.wbMode->addItem(QStringLiteral("9 manual"));
    ispForm->addRow(QStringLiteral("WB mode:"), controls.wbMode);

    controls.saturation = new QDoubleSpinBox(ispGroup);
    controls.saturation->setRange(0.0, 2.0);
    controls.saturation->setDecimals(2);
    controls.saturation->setSingleStep(0.1);
    controls.saturation->setValue(1.0);  // 1 = neutral
    controls.saturation->setProperty("lastSent", 1.0);
    ispForm->addRow(QStringLiteral("Saturation:"), controls.saturation);

    controls.tnrMode = new QComboBox(ispGroup);
    controls.tnrMode->addItem(QStringLiteral("(unchanged)"));
    controls.tnrMode->addItem(QStringLiteral("0 off"));
    controls.tnrMode->addItem(QStringLiteral("1 fast"));
    controls.tnrMode->addItem(QStringLiteral("2 high quality"));
    ispForm->addRow(QStringLiteral("TNR mode:"), controls.tnrMode);

    controls.tnrStrength = new QDoubleSpinBox(ispGroup);
    controls.tnrStrength->setRange(-1.0, 1.0);
    controls.tnrStrength->setDecimals(2);
    controls.tnrStrength->setSingleStep(0.1);
    controls.tnrStrength->setValue(-1.0);
    controls.tnrStrength->setSpecialValueText(QStringLiteral("auto")); // -1
    controls.tnrStrength->setProperty("lastSent", -1.0);
    ispForm->addRow(QStringLiteral("TNR strength:"), controls.tnrStrength);

    controls.eeMode = new QComboBox(ispGroup);
    controls.eeMode->addItem(QStringLiteral("(unchanged)"));
    controls.eeMode->addItem(QStringLiteral("0 off"));
    controls.eeMode->addItem(QStringLiteral("1 fast"));
    controls.eeMode->addItem(QStringLiteral("2 high quality"));
    ispForm->addRow(QStringLiteral("EE mode:"), controls.eeMode);

    controls.eeStrength = new QDoubleSpinBox(ispGroup);
    controls.eeStrength->setRange(-1.0, 1.0);
    controls.eeStrength->setDecimals(2);
    controls.eeStrength->setSingleStep(0.1);
    controls.eeStrength->setValue(-1.0);
    controls.eeStrength->setSpecialValueText(QStringLiteral("auto")); // -1
    controls.eeStrength->setProperty("lastSent", -1.0);
    ispForm->addRow(QStringLiteral("EE strength:"), controls.eeStrength);

    controls.exposureComp = new QDoubleSpinBox(ispGroup);
    controls.exposureComp->setRange(-2.0, 2.0);
    controls.exposureComp->setDecimals(1);
    controls.exposureComp->setSingleStep(0.5);
    controls.exposureComp->setValue(0.0);
    controls.exposureComp->setSuffix(QStringLiteral(" EV"));
    controls.exposureComp->setProperty("lastSent", 0.0);
    ispForm->addRow(QStringLiteral("AE comp:"), controls.exposureComp);

    form->addRow(ispGroup);

    connect(controls.wbMode, &QComboBox::activated, this, [this, index](
            int item) { applyIspCombo(index, QStringLiteral("wbmode"), item); });
    connect(controls.tnrMode, &QComboBox::activated, this, [this, index](
            int item) { applyIspCombo(index, QStringLiteral("tnr-mode"), item); });
    connect(controls.eeMode, &QComboBox::activated, this, [this, index](
            int item) { applyIspCombo(index, QStringLiteral("ee-mode"), item); });
    connect(controls.saturation, &QDoubleSpinBox::editingFinished, this,
            [this, index]() {
                applyIspSpin(index, QStringLiteral("saturation"),
                             m_cameraControls[index].saturation);
            });
    connect(controls.tnrStrength, &QDoubleSpinBox::editingFinished, this,
            [this, index]() {
                applyIspSpin(index, QStringLiteral("tnr-strength"),
                             m_cameraControls[index].tnrStrength);
            });
    connect(controls.eeStrength, &QDoubleSpinBox::editingFinished, this,
            [this, index]() {
                applyIspSpin(index, QStringLiteral("ee-strength"),
                             m_cameraControls[index].eeStrength);
            });
    connect(controls.exposureComp, &QDoubleSpinBox::editingFinished, this,
            [this, index]() {
                applyIspSpin(index, QStringLiteral("exposurecompensation"),
                             m_cameraControls[index].exposureComp);
            });

    connect(controls.fire, &QPushButton::clicked, this,
            [this, index]() { fireTrigger(index); });
    connect(controls.zoom, &QDoubleSpinBox::editingFinished, this,
            [this, index]() { applyZoom(index); });
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
    for (int i = 0; i < 2; ++i)
        restartPane(i);
    m_control->connectToDevice(controlHost(), kControlPort);
    m_connected = true;
    m_connectButton->setText(QStringLiteral("Disconnect"));
}

void MainWindow::restartPane(int index)
{
    Pane &pane = m_panes[index];
    setStatus(pane, QStringLiteral("connecting…"));
    pane.player->stop();
    // Reset first: setSource() with the current URL is a no-op in Qt 6,
    // but a zoom change needs a genuinely new RTSP session.
    pane.player->setSource(QUrl());
    pane.player->setSource(streamUrl(index));
    // Note: Qt 6 uses the FFmpeg media backend; there is no public
    // low-latency knob on QMediaPlayer. Acceptable for milestone 1.
    pane.player->play();
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
    m_syncCheck->setEnabled(enabled);
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

                QString line;
                if (camera.value(QStringLiteral("streaming")).toBool()) {
                    const qint64 frames = static_cast<qint64>(
                        camera.value(QStringLiteral("frames")).toDouble());
                    line = QStringLiteral("cam%1: streaming, %2 frames")
                               .arg(index)
                               .arg(frames);
                    // Optional (present while frames flow): last_frame
                    // metadata — the sequence number is the sync check.
                    const QJsonValue lastFrame =
                        camera.value(QStringLiteral("last_frame"));
                    if (lastFrame.isObject()) {
                        const qint64 sequence = static_cast<qint64>(
                            lastFrame.toObject()
                                .value(QStringLiteral("sequence"))
                                .toDouble());
                        line += QStringLiteral(", seq %1").arg(sequence);
                    }
                } else {
                    line = QStringLiteral("cam%1: idle").arg(index);
                }

                // Optional: what Argus AE programmed into the sensor right
                // now — exposure_current (µs) and gain_current (milli-dB).
                const QJsonValue expCurrent =
                    camera.value(QStringLiteral("exposure_current"));
                if (expCurrent.isDouble()) {
                    const double us = expCurrent.toDouble();
                    line += us < 1000.0
                        ? QStringLiteral(", exp %1 µs").arg(qRound(us))
                        : QStringLiteral(", exp %1 ms")
                              .arg(us / 1000.0, 0, 'f', 1);
                }
                const QJsonValue gainCurrent =
                    camera.value(QStringLiteral("gain_current"));
                if (gainCurrent.isDouble())
                    line += QStringLiteral(", gain %1 dB")
                                .arg(gainCurrent.toDouble() / 1000.0, 0,
                                     'f', 1);
                lines << line;

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
                    const double zoom =
                        camera.value(QStringLiteral("zoom")).toDouble(1.0);
                    controls.zoom->setValue(zoom);
                    controls.zoom->setProperty("lastSent", zoom);
                    seedIspControls(
                        controls,
                        camera.value(QStringLiteral("isp")).toObject());
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

void MainWindow::applySync(bool enabled)
{
    QJsonObject params;
    params.insert(QStringLiteral("enabled"), enabled);
    m_control->sendRequest(
        QStringLiteral("set-sync"), params,
        [this, enabled](const QJsonObject &, const QJsonObject &error) {
            if (error.isEmpty())
                return;
            // The device changed nothing on error — revert the checkbox.
            m_syncCheck->blockSignals(true);
            m_syncCheck->setChecked(!enabled);
            m_syncCheck->blockSignals(false);
            showRequestError(QStringLiteral("set-sync"), error);
        });
}

void MainWindow::fireTrigger(int camera)
{
    QJsonObject params;
    params.insert(QStringLiteral("camera"), camera);
    m_control->sendRequest(
        QStringLiteral("fire-trigger"), params,
        [this, camera](const QJsonObject &, const QJsonObject &error) {
            if (!error.isEmpty())
                showRequestError(
                    QStringLiteral("cam%1 fire-trigger").arg(camera), error);
        });
}

void MainWindow::applyZoom(int camera)
{
    QDoubleSpinBox *box = m_cameraControls[camera].zoom;
    const double factor = box->value();
    const QVariant lastSent = box->property("lastSent");
    if (lastSent.isValid() && lastSent.toDouble() == factor)
        return;
    box->setProperty("lastSent", factor);

    QJsonObject params;
    params.insert(QStringLiteral("camera"), camera);
    params.insert(QStringLiteral("factor"), factor);
    m_control->sendRequest(
        QStringLiteral("set-zoom"), params,
        [this, camera](const QJsonObject &, const QJsonObject &error) {
            if (!error.isEmpty()) {
                showRequestError(QStringLiteral("cam%1 set-zoom").arg(camera),
                                 error);
                return;
            }
            // Zoom reliably applies to new RTSP sessions only (the mount's
            // launch string is re-armed) — reconnect this pane to show it.
            if (m_connected)
                restartPane(camera);
        });
}

void MainWindow::applyIsp(int camera, const QString &param,
                          const QJsonValue &value)
{
    QJsonObject params;
    params.insert(QStringLiteral("camera"), camera);
    params.insert(QStringLiteral("param"), param);
    params.insert(QStringLiteral("value"), value);
    m_control->sendRequest(
        QStringLiteral("set-isp"), params,
        [this, camera, param](const QJsonObject &, const QJsonObject &error) {
            if (!error.isEmpty())
                showRequestError(QStringLiteral("cam%1 set-isp %2")
                                     .arg(camera)
                                     .arg(param),
                                 error);
        });
}

void MainWindow::applyIspCombo(int camera, const QString &param, int item)
{
    if (item <= 0)  // item 0 is "(unchanged)": send nothing
        return;
    applyIsp(camera, param, item - 1);
}

void MainWindow::applyIspSpin(int camera, const QString &param,
                              QDoubleSpinBox *box)
{
    const double value = box->value();
    const QVariant lastSent = box->property("lastSent");
    if (lastSent.isValid() && lastSent.toDouble() == value)
        return;
    box->setProperty("lastSent", value);
    applyIsp(camera, param, value);
}

void MainWindow::seedIspControls(CameraControls &controls,
                                 const QJsonObject &isp)
{
    // ISP override values arrive as strings (PROTOCOL.md); tolerate plain
    // numbers too. Absent/unparseable params leave the widget alone.
    const auto number = [&isp](const QString &param, double *out) {
        const QJsonValue value = isp.value(param);
        if (value.isDouble()) {
            *out = value.toDouble();
            return true;
        }
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok)
            *out = parsed;
        return ok;
    };
    const auto seedSpin = [&number](QDoubleSpinBox *box, const QString &param) {
        double value = 0.0;
        if (!number(param, &value))
            return;
        box->setValue(value);
        box->setProperty("lastSent", value);
    };
    const auto seedCombo = [&number](QComboBox *box, const QString &param) {
        double value = 0.0;
        if (!number(param, &value))
            return;
        const int mode = static_cast<int>(value);
        if (mode >= 0 && mode + 1 < box->count())
            box->setCurrentIndex(mode + 1);  // never emits activated
    };

    seedCombo(controls.wbMode, QStringLiteral("wbmode"));
    seedSpin(controls.saturation, QStringLiteral("saturation"));
    seedCombo(controls.tnrMode, QStringLiteral("tnr-mode"));
    seedSpin(controls.tnrStrength, QStringLiteral("tnr-strength"));
    seedCombo(controls.eeMode, QStringLiteral("ee-mode"));
    seedSpin(controls.eeStrength, QStringLiteral("ee-strength"));
    seedSpin(controls.exposureComp, QStringLiteral("exposurecompensation"));
}

void MainWindow::runDiscovery()
{
    m_discoverButton->setEnabled(false);  // re-enabled on finished()
    m_discoverMenu->clear();
    m_discovery->discover();
}

void MainWindow::showRequestError(const QString &what, const QJsonObject &error)
{
    if (!m_control->isConnected())
        return; // teardown noise; the control status line already says it
    m_errorLabel->setText(QStringLiteral("%1: %2").arg(
        what, error.value(QStringLiteral("message")).toString()));
}
