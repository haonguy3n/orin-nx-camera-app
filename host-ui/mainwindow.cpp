#include "mainwindow.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "cameracontrols.h"
#include "controlclient.h"
#include "controlpanel.h"
#include "discoveryclient.h"
#include "theme.h"
#include "updateclient.h"
#include "updatewidget.h"
#include "videopane.h"
#include "whitebalancecalibrator.h"

#include "proto/Protocol.h"

namespace {

constexpr quint16 kControlPort = proto::kControlPort;
constexpr quint16 kUpdatePort = proto::kUpdatePort;
constexpr int kStatusPollMs = 2000;
// Default device address on the USB CDC-NCM link (app default, not protocol).
const QLatin1String kDefaultHost("192.168.55.1");

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("camera-viewer"));
    setStyleSheet(Theme::stylesheet());
    Theme::applyFont(this);
    setAutoFillBackground(true);

    auto *central = new QWidget(this);
    central->setAutoFillBackground(true);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setSpacing(0);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    setupToolbar(central);
    setupVideoArea(central);

    setCentralWidget(central);
    resize(1280, 620);

    // Control channel + status poll.
    m_control = new ControlClient(this);
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusPollMs);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::pollStatus);

    // Calibration.
    m_calibrator = new WhiteBalanceCalibrator(m_control, this);

    // Discovery.
    m_discovery = new DiscoveryClient(this);

    // OTA update.
    m_updateClient = new UpdateClient(this);
    connect(m_updateClient, &UpdateClient::uploadProgress, this,
            [this](qint64 sent, qint64 total) {
                m_controlPanel->updateWidget()->setUploadProgress(sent, total);
            });
    connect(m_updateClient, &UpdateClient::uploadFinished, this,
            [this](const QJsonObject &/*response*/) {
                m_controlPanel->setError(QString());
                // Start polling install status immediately
                pollUpdateStatus();
            });
    connect(m_updateClient, &UpdateClient::uploadError, this,
            [this](const QString &message) {
                m_controlPanel->setError(
                    QStringLiteral("update: %1").arg(message));
            });

    setupConnections();
}

void MainWindow::setupToolbar(QWidget *parent)
{
    auto *topBar = new QHBoxLayout;
    topBar->setSpacing(6);
    topBar->setContentsMargins(8, 6, 8, 6);

    m_hostEdit = new QLineEdit(kDefaultHost, parent);
    m_hostEdit->setPlaceholderText(QStringLiteral("Device IP / rtsp://host:port"));
    m_hostEdit->setToolTip(
        QStringLiteral("Device IP/hostname, or an rtsp://host:port base URL"));

    m_cameraSelect = new QComboBox(parent);
    m_cameraSelect->setToolTip(
        QStringLiteral("switch the video pane between cameras"));
    // Populated dynamically from get-status / discovery (see populateCameraList)

    m_connectButton = new QPushButton(QStringLiteral("Connect"), parent);
    m_connectButton->setObjectName(QStringLiteral("accent"));
    m_connectButton->setCursor(Qt::PointingHandCursor);

    m_discoverButton = new QPushButton(QStringLiteral("Discover"), parent);
    m_discoverButton->setCursor(Qt::PointingHandCursor);
    m_discoverButton->setToolTip(
        QStringLiteral("find devices via UDP broadcast (port %1)").arg(proto::kDiscoveryPort));
    m_discoverMenu = new QMenu(m_discoverButton);

    topBar->addWidget(m_hostEdit, 1);
    topBar->addWidget(m_cameraSelect);
    topBar->addWidget(m_connectButton);
    topBar->addWidget(m_discoverButton);

    parent->layout()->addItem(topBar);
}

void MainWindow::setupVideoArea(QWidget *parent)
{
    m_paneStack = new QStackedWidget(parent);
    for (int i = 0; i < 2; ++i) {
        m_panes[i] = new VideoPane(QStringLiteral("cam%1").arg(i), parent);
        m_paneStack->addWidget(m_panes[i]);
    }
    m_paneStack->setCurrentIndex(0);

    m_controlPanel = new ControlPanel(parent);

    auto *paneLayout = new QHBoxLayout;
    paneLayout->setSpacing(0);
    paneLayout->setContentsMargins(0, 0, 0, 0);
    paneLayout->addWidget(m_paneStack, 1);
    paneLayout->addWidget(m_controlPanel);

    parent->layout()->addItem(paneLayout);
}

void MainWindow::setupConnections()
{
    // Camera selector — switch the visible pane.
    connect(m_cameraSelect, &QComboBox::activated, this, [this](int row) {
        const int camIndex = comboBoxToCameraIndex(row);
        if (camIndex >= 0 && camIndex < 2) {
            m_paneStack->setCurrentIndex(camIndex);
            // The viewer presents one camera at a time.  Opening every
            // mount on connect made a broken /cam1 RTSP session contend with
            // the selected healthy stream in Qt's media backend.  Defer a
            // stream until the user selects its pane so a failed camera is
            // isolated from the other one.
            if (m_connected)
                restartPane(camIndex);
        }
    });

    // Connect / Disconnect toggle.
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

    // Control channel lifecycle.
    connect(m_control, &ControlClient::connected, this, [this]() {
        m_controlPanel->setControlStatus(QStringLiteral("control: connected"));
        m_controlPanel->clearError();
        m_controlsPopulated = false;
        m_calibrationResult.clear();
        m_controlPanel->setControlsEnabled(true);
        m_statusTimer->start();
        pollStatus();
    });

    connect(m_control, &ControlClient::disconnected, this, [this]() {
        m_calibrator->abort();
        m_controlPanel->setControlStatus(QStringLiteral("control: disconnected"));
        m_controlPanel->setDeviceStatus(QString());
        m_controlPanel->setControlsEnabled(false);
        m_statusTimer->stop();
        // Reset camera list so it re-populates on next connect
        m_cameraSelect->clear();
        m_cameraIndices.clear();
        m_cameraListPopulated = false;
        m_controlsPopulated = false;
    });

    connect(m_control, &ControlClient::errorOccurred, this,
            [this](const QString &message) {
                m_controlPanel->setError(QStringLiteral("control: %1").arg(message));
                if (!m_control->isConnected()) {
                    m_controlPanel->setControlStatus(
                        QStringLiteral("control: disconnected"));
                    m_controlPanel->setControlsEnabled(false);
                    m_statusTimer->stop();
                }
            });

    // Discovery.
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

                // Populate camera list from discovery if not already done
                // (gives the user camera info before connecting).
                if (!m_cameraListPopulated) {
                    const QJsonArray cameras =
                        info.value(QStringLiteral("cameras")).toArray();
                    if (!cameras.isEmpty())
                        populateCameraList(cameras);
                }

                const QString current = m_hostEdit->text().trimmed();
                if (current.isEmpty()
                    || current == kDefaultHost
                    || m_discoveredHosts.contains(current))
                    m_hostEdit->setText(address);
                if (!m_discoveredHosts.contains(address))
                    m_discoveredHosts.append(address);
            });

    connect(m_discovery, &DiscoveryClient::finished, this, [this]() {
        m_discoverButton->setEnabled(true);
        const int found = m_discoverMenu->actions().count();
        if (found == 0)
            m_controlPanel->setError(QStringLiteral("no devices found"));
        else if (found > 1)
            m_discoverMenu->popup(m_discoverButton->mapToGlobal(
                QPoint(0, m_discoverButton->height())));
    });

    // Control panel → control channel requests.
    connect(m_controlPanel, &ControlPanel::syncToggled, this, [this](bool enabled) {
        QJsonObject params;
        params.insert(QStringLiteral("enabled"), enabled);
        m_control->sendRequest(
            QLatin1String(proto::methods::kSetSync), params,
            [this, enabled](const QJsonObject &, const QJsonObject &error) {
                if (error.isEmpty())
                    return;
                m_controlPanel->setSyncChecked(!enabled);  // revert
                showRequestError(QLatin1String(proto::methods::kSetSync), error);
            });
    });

    connect(m_controlPanel, &ControlPanel::calibrateRequested, this, [this]() {
        m_calibrator->start(m_panes[0], m_panes[1]);
        m_statusTimer->stop();
        updateCalibrateEnabled();
    });

    // Calibration progress / result → device status line.
    connect(m_calibrator, &WhiteBalanceCalibrator::progressMessage, this,
            [this](const QString &message) {
                m_controlPanel->setDeviceStatus(message);
            });
    connect(m_calibrator, &WhiteBalanceCalibrator::finished, this,
            [this](const QString &message) {
                m_calibrationResult = message;
                m_controlPanel->setDeviceStatus(message);
                if (m_control->isConnected()) {
                    m_statusTimer->start();
                    pollStatus();
                }
                updateCalibrateEnabled();
            });
    connect(m_calibrator, &WhiteBalanceCalibrator::restartPanesRequested, this,
            [this]() {
                restartPane(0);
                restartPane(1);
            });

    // Per-camera control signals → control channel requests.
    connect(m_controlPanel, &ControlPanel::cameraExposureChanged, this,
            [this](int camera, int us) {
                QJsonObject params;
                params.insert(QStringLiteral("camera"), camera);
                params.insert(QStringLiteral("us"), us);
                m_control->sendRequest(
                    QLatin1String(proto::methods::kSetExposure), params,
                    [this, camera](const QJsonObject &, const QJsonObject &error) {
                        if (!error.isEmpty())
                            showRequestError(
                                QStringLiteral("cam%1 set-exposure").arg(camera),
                                error);
                    });
            });

    connect(m_controlPanel, &ControlPanel::cameraGainChanged, this,
            [this](int camera, double gain) {
                QJsonObject params;
                params.insert(QStringLiteral("camera"), camera);
                params.insert(QStringLiteral("gain"), gain);
                m_control->sendRequest(
                    QLatin1String(proto::methods::kSetGain), params,
                    [this, camera](const QJsonObject &, const QJsonObject &error) {
                        if (!error.isEmpty())
                            showRequestError(
                                QStringLiteral("cam%1 set-gain").arg(camera),
                                error);
                    });
            });

    connect(m_controlPanel, &ControlPanel::cameraTriggerChanged, this,
            [this](int camera, int mode) {
                QJsonObject params;
                params.insert(QStringLiteral("camera"), camera);
                params.insert(QStringLiteral("mode"), mode);
                m_control->sendRequest(
                    QLatin1String(proto::methods::kSetTrigger), params,
                    [this, camera](const QJsonObject &, const QJsonObject &error) {
                        if (!error.isEmpty())
                            showRequestError(
                                QStringLiteral("cam%1 set-trigger").arg(camera),
                                error);
                    });
            });

    connect(m_controlPanel, &ControlPanel::cameraFireRequested, this,
            [this](int camera) {
                QJsonObject params;
                params.insert(QStringLiteral("camera"), camera);
                m_control->sendRequest(
                    QLatin1String(proto::methods::kFireTrigger), params,
                    [this, camera](const QJsonObject &, const QJsonObject &error) {
                        if (!error.isEmpty())
                            showRequestError(
                                QStringLiteral("cam%1 fire-trigger").arg(camera),
                                error);
                    });
            });

    connect(m_controlPanel, &ControlPanel::cameraZoomChanged, this,
            [this](int camera, double factor) {
                QJsonObject params;
                params.insert(QStringLiteral("camera"), camera);
                params.insert(QStringLiteral("factor"), factor);
                m_control->sendRequest(
                    QLatin1String(proto::methods::kSetZoom), params,
                    [this, camera](const QJsonObject &, const QJsonObject &error) {
                        if (!error.isEmpty()) {
                            showRequestError(
                                QStringLiteral("cam%1 set-zoom").arg(camera),
                                error);
                            return;
                        }
                        if (m_connected)
                            restartPane(camera);
                    });
            });

    connect(m_controlPanel, &ControlPanel::cameraIspComboChanged, this,
            [this](int camera, const QString &param, int value) {
                QJsonObject params;
                params.insert(QStringLiteral("camera"), camera);
                params.insert(QStringLiteral("param"), param);
                params.insert(QStringLiteral("value"), value);
                m_control->sendRequest(
                    QLatin1String(proto::methods::kSetIsp), params,
                    [this, camera, param](const QJsonObject &,
                                          const QJsonObject &error) {
                        if (!error.isEmpty())
                            showRequestError(
                                QStringLiteral("cam%1 set-isp %2").arg(camera).arg(param),
                                error);
                    });
            });

    connect(m_controlPanel, &ControlPanel::cameraIspSpinChanged, this,
            [this](int camera, const QString &param, double value) {
                QJsonObject params;
                params.insert(QStringLiteral("camera"), camera);
                params.insert(QStringLiteral("param"), param);
                params.insert(QStringLiteral("value"), value);
                m_control->sendRequest(
                    QLatin1String(proto::methods::kSetIsp), params,
                    [this, camera, param](const QJsonObject &,
                                          const QJsonObject &error) {
                        if (!error.isEmpty())
                            showRequestError(
                                QStringLiteral("cam%1 set-isp %2").arg(camera).arg(param),
                                error);
                    });
            });

    // OTA update: upload .swu file to device.
    connect(m_controlPanel, &ControlPanel::uploadRequested, this,
            [this](const QString &filePath) {
                if (m_updateClient->isBusy())
                    return;
                m_controlPanel->setError(QString());
                m_updateClient->uploadFile(controlHost(), kUpdatePort,
                                           filePath);
            });

    // Reboot device.
    connect(m_controlPanel, &ControlPanel::rebootRequested, this,
            &MainWindow::sendReboot);
}

void MainWindow::connectStreams()
{
    // This is a single-pane UI: only open the camera that is visible.  In
    // particular, an unavailable second camera must not interfere with the
    // selected camera's decoder/session.  Selecting the other camera opens
    // it on demand (see setupConnections()).
    restartPane(m_paneStack->currentIndex());
    m_control->connectToDevice(controlHost(), kControlPort);
    m_connected = true;
    m_connectButton->setText(QStringLiteral("Disconnect"));
    updateCalibrateEnabled();
}

void MainWindow::disconnectStreams()
{
    for (VideoPane *pane : m_panes)
        pane->stop();
    m_control->disconnectFromDevice();
    m_connected = false;
    m_connectButton->setText(QStringLiteral("Connect"));
    updateCalibrateEnabled();
}

void MainWindow::restartPane(int index)
{
    m_panes[index]->restart(streamUrl(index));
}

QUrl MainWindow::streamUrl(int index) const
{
    QString text = m_hostEdit->text().trimmed();
    if (text.isEmpty())
        text = kDefaultHost;

    if (text.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive)) {
        while (text.endsWith(QLatin1Char('/')))
            text.chop(1);
        return QUrl(QStringLiteral("%1/cam%2").arg(text).arg(index));
    }
    return QUrl(QStringLiteral("rtsp://%1:%2/cam%3")
                    .arg(text).arg(proto::kRtspPort).arg(index));
}

QString MainWindow::controlHost() const
{
    QString text = m_hostEdit->text().trimmed();
    if (text.isEmpty())
        text = kDefaultHost;
    if (text.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive))
        return QUrl(text).host();
    return text;
}

void MainWindow::pollStatus()
{
    m_control->sendRequest(
        QLatin1String(proto::methods::kGetStatus), QJsonObject(),
        [this](const QJsonObject &result, const QJsonObject &error) {
            if (!error.isEmpty()) {
                showRequestError(QLatin1String(proto::methods::kGetStatus), error);
                return;
            }

            // Latest device-wide tuning — the calibrator composes on top of it.
            const QJsonValue tuning = result.value(QStringLiteral("tuning"));
            if (tuning.isObject())
                m_calibrator->setTuning(tuning.toObject());

            QStringList lines;
            const QJsonArray cameras =
                result.value(QStringLiteral("cameras")).toArray();

            // Populate the camera dropdown from the device's actual camera
            // list on the first successful status poll.
            if (!m_cameraListPopulated && !cameras.isEmpty()) {
                populateCameraList(cameras);
                m_cameraListPopulated = true;
            }

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
                               .arg(index).arg(frames);
                    const QJsonValue lastFrame =
                        camera.value(QStringLiteral("last_frame"));
                    if (lastFrame.isObject()) {
                        const qint64 sequence = static_cast<qint64>(
                            lastFrame.toObject()
                                .value(QStringLiteral("sequence")).toDouble());
                        line += QStringLiteral(", seq %1").arg(sequence);
                    }
                } else {
                    line = QStringLiteral("cam%1: idle").arg(index);
                }

                const QJsonValue expCurrent =
                    camera.value(QStringLiteral("exposure_current"));
                if (expCurrent.isDouble()) {
                    const double us = expCurrent.toDouble();
                    line += us < 1000.0
                        ? QStringLiteral(", exp %1 us").arg(qRound(us))
                        : QStringLiteral(", exp %1 ms").arg(us / 1000.0, 0, 'f', 1);
                }
                const QJsonValue gainCurrent =
                    camera.value(QStringLiteral("gain_current"));
                if (gainCurrent.isDouble())
                    line += QStringLiteral(", gain %1 dB")
                                .arg(gainCurrent.toDouble() / 1000.0, 0, 'f', 1);
                lines << line;

                // Seed widgets from the first status only.
                if (!m_controlsPopulated)
                    m_controlPanel->cameraControls(index)->seedFromStatus(camera);
            }
            // Only mark seeded once cameras actually arrived — an early poll
            // during device startup can report an empty list.
            if (!cameras.isEmpty())
                m_controlsPopulated = true;
            if (!m_calibrationResult.isEmpty())
                lines << m_calibrationResult;
            if (!m_calibrator->isRunning())
                m_controlPanel->setDeviceStatus(lines.join(QLatin1Char('\n')));
        });
}

void MainWindow::runDiscovery()
{
    m_discoverButton->setEnabled(false);
    m_discoverMenu->clear();
    m_discovery->discover();
}

void MainWindow::showRequestError(const QString &what, const QJsonObject &error)
{
    if (!m_control->isConnected())
        return;
    m_controlPanel->setError(QStringLiteral("%1: %2").arg(
        what, error.value(QStringLiteral("message")).toString()));
}

void MainWindow::updateCalibrateEnabled()
{
    m_controlPanel->setCalibrateEnabled(
        m_control->isConnected() && m_connected && !m_calibrator->isRunning());
}

void MainWindow::pollUpdateStatus()
{
    if (!m_control->isConnected())
        return;
    m_control->sendRequest(
        QLatin1String(proto::methods::kGetUpdateStatus), QJsonObject(),
        [this](const QJsonObject &result, const QJsonObject &error) {
            if (!error.isEmpty())
                return;
            m_controlPanel->updateWidget()->updateStatus(result);

            // Keep polling while installing; stop on terminal states
            const QString state =
                result.value(QStringLiteral("state")).toString();
            if (state == QLatin1String(proto::update_states::kInstalling) ||
                state == QLatin1String(proto::update_states::kUploading))
                QTimer::singleShot(kStatusPollMs, this,
                                   &MainWindow::pollUpdateStatus);

            // Auto-reboot on success if the checkbox is checked
            if (state == QLatin1String(proto::update_states::kSuccess) ||
                state == QLatin1String(proto::update_states::kDone)) {
                if (m_controlPanel->updateWidget()->autoRebootChecked()) {
                    m_controlPanel->setError(QString());
                    QTimer::singleShot(2000, this, &MainWindow::sendReboot);
                }
            }
        });
}

void MainWindow::sendReboot()
{
    if (!m_control->isConnected())
        return;
    m_control->sendRequest(
        QLatin1String(proto::methods::kReboot), QJsonObject(),
        [this](const QJsonObject &result, const QJsonObject &error) {
            if (!error.isEmpty()) {
                showRequestError(QLatin1String(proto::methods::kReboot), error);
                return;
            }
            m_controlPanel->setError(QStringLiteral(
                "Rebooting device — connection will drop..."));
        });
}

void MainWindow::populateCameraList(const QJsonArray &cameras)
{
    // Guard against re-entry from discovery + first get-status racing
    if (m_cameraListPopulated)
        return;

    m_cameraSelect->clear();
    m_cameraIndices.clear();

    for (const QJsonValue &value : cameras) {
        const QJsonObject cam = value.toObject();
        const int index = cam.value(QStringLiteral("index")).toInt(-1);
        if (index < 0)
            continue;

        const bool enabled = cam.value(QStringLiteral("enabled")).toBool(true);
        const QString source =
            cam.value(QStringLiteral("source")).toString();
        const QString mount =
            cam.value(QStringLiteral("mount")).toString(
                QStringLiteral("cam%1").arg(index));

        // Build a descriptive label: "cam0 — argus 1440×1080"
        // or "cam1 — v4l2 (disabled)" if not enabled
        QString label = mount;
        if (!source.isEmpty())
            label += QStringLiteral(" — %1").arg(source);
        const int width =
            static_cast<int>(cam.value(QStringLiteral("width")).toDouble(0));
        const int height =
            static_cast<int>(cam.value(QStringLiteral("height")).toDouble(0));
        if (width > 0 && height > 0)
            label += QStringLiteral(" %1×%2").arg(width).arg(height);
        if (!enabled)
            label += QStringLiteral(" (disabled)");

        m_cameraSelect->addItem(label);
        m_cameraIndices.append(index);
    }

    // Select the first enabled camera, or cam0 as fallback
    int selectRow = 0;
    for (int i = 0; i < cameras.size(); ++i) {
        const QJsonObject cam = cameras.at(i).toObject();
        if (cam.value(QStringLiteral("enabled")).toBool(true)) {
            selectRow = i;
            break;
        }
    }
    if (m_cameraSelect->count() > 0) {
        m_cameraSelect->setCurrentIndex(selectRow);
        const int camIdx = comboBoxToCameraIndex(selectRow);
        if (camIdx >= 0 && camIdx < 2)
            m_paneStack->setCurrentIndex(camIdx);
    }
}

int MainWindow::comboBoxToCameraIndex(int comboIndex) const
{
    if (comboIndex < 0 || comboIndex >= m_cameraIndices.size())
        return -1;
    return m_cameraIndices.at(comboIndex);
}
