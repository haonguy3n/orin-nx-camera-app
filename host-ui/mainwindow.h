#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <QStringList>
#include <QUrl>

class ControlClient;
class ControlPanel;
class DiscoveryClient;
class QComboBox;
class QLineEdit;
class QPushButton;
class QMenu;
class QStackedWidget;
class QTimer;
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
    void connectStreams();
    void disconnectStreams();
    void restartPane(int index);
    QUrl streamUrl(int index) const;
    QString controlHost() const;
    void pollStatus();
    void runDiscovery();
    void showRequestError(const QString &what, const QJsonObject &error);
    void updateCalibrateEnabled();

    // Toolbar widgets.
    QLineEdit *m_hostEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_discoverButton = nullptr;
    QMenu *m_discoverMenu = nullptr;
    QComboBox *m_cameraSelect = nullptr;

    // Video area.
    QStackedWidget *m_paneStack = nullptr;
    VideoPane *m_panes[2] = {nullptr, nullptr};

    // Control panel sidebar.
    ControlPanel *m_controlPanel = nullptr;

    // Networking.
    ControlClient *m_control = nullptr;
    DiscoveryClient *m_discovery = nullptr;
    QStringList m_discoveredHosts;
    QTimer *m_statusTimer = nullptr;

    // Calibration.
    WhiteBalanceCalibrator *m_calibrator = nullptr;
    QString m_calibrationResult;

    // State.
    bool m_connected = false;
    bool m_controlsPopulated = false;
};
