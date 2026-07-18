#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <memory>

#include "secureusbbridge.h"
#include <QStringList>
#include <QUrl>

class ControlClient;
class ControlPanel;
class DiscoveryClient;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QMenu;
class QStackedWidget;
class QTimer;
class UpdateClient;
class VideoPane;
class WhiteBalanceCalibrator;

// Thin orchestrator: wires the top toolbar, video pane stack, and control
// panel sidebar together, and routes user actions to the control channel.
// All UI construction and domain logic is delegated to dedicated classes:
//   - Theme:           stylesheet + font
//   - VideoPane:       per-camera video surface + player
//   - ControlPanel:    sidebar (Device + camera sections)
//   - CameraControls:  per-camera control widgets (inside ControlPanel)
//   - WhiteBalanceCalibrator: calibration measurement + set-tuning loop
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void setupToolbar(QWidget *parent);
    void setupVideoArea(QWidget *parent);
    void setupConnections();
    void setConnectionState(const QString &text, bool online = false);
    void connectStreams();
    void disconnectStreams();
    // Start/stop the device's cameras via set-stream. connect -> start,
    // disconnect -> stop; symmetric so a reconnect always re-enables.
    void setStreamEnabled(bool enabled);
    // Parse a Channel::Meta face-box JSON payload and draw it on the camera's
    // pane. Runs on the GUI thread (marshalled from the bridge worker).
    void applyFaceMeta(int camera, const QByteArray &json);
    void restartPane(int index);
    QUrl streamUrl(int index) const;
    QString controlHost() const;
    bool usesSecureUsb() const;
    bool startSelectedTransport();
    void pollStatus();
    void pollUpdateStatus();
    void sendReboot();
    void runDiscovery();
    void populateCameraList(const QJsonArray &cameras);
    int comboBoxToCameraIndex(int comboIndex) const;
    void showRequestError(const QString &what, const QJsonObject &error);
    void updateCalibrateEnabled();

    // Toolbar widgets.
    QLineEdit *m_hostEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_discoverButton = nullptr;
    QMenu *m_discoverMenu = nullptr;
    QComboBox *m_transportSelect = nullptr;
    QComboBox *m_cameraSelect = nullptr;
    QLabel *m_connectionStatus = nullptr;

    // Video area.
    QStackedWidget *m_paneStack = nullptr;
    VideoPane *m_panes[2] = {nullptr, nullptr};

    // Control panel sidebar.
    ControlPanel *m_controlPanel = nullptr;

    // Networking.
    ControlClient *m_control = nullptr;
    DiscoveryClient *m_discovery = nullptr;
    UpdateClient *m_updateClient = nullptr;
    QStringList m_discoveredHosts;
    QTimer *m_statusTimer = nullptr;

    // Calibration.
    WhiteBalanceCalibrator *m_calibrator = nullptr;
    QString m_calibrationResult;

    // State.
    bool m_connected = false;
    bool m_usingSecureUsb = false;
    std::unique_ptr<SecureUsbBridge> m_secureUsbBridge;
    bool m_controlsPopulated = false;
    QList<int> m_cameraIndices;  // combo row -> actual camera index
    bool m_cameraListPopulated = false;
};
