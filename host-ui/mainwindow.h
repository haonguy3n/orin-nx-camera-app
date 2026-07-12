#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QUrl>

class ControlClient;
class DiscoveryClient;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QJsonObject;
class QJsonValue;
class QLabel;
class QLineEdit;
class QMediaPlayer;
class QMenu;
class QPushButton;
class QSpinBox;
class QTimer;
class QVideoWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    struct Pane {
        QString name;
        QMediaPlayer *player = nullptr;
        QVideoWidget *video = nullptr;
        QLabel *status = nullptr;
    };

    struct CameraControls {
        QGroupBox *group = nullptr;
        QSpinBox *exposure = nullptr;
        QDoubleSpinBox *gain = nullptr;
        QComboBox *trigger = nullptr;
        QPushButton *fire = nullptr;
        // ISP overrides (argus source only, PROTOCOL.md set-isp).
        QComboBox *wbMode = nullptr;
        QDoubleSpinBox *saturation = nullptr;
        QComboBox *tnrMode = nullptr;
        QDoubleSpinBox *tnrStrength = nullptr;
        QComboBox *eeMode = nullptr;
        QDoubleSpinBox *eeStrength = nullptr;
        QDoubleSpinBox *exposureComp = nullptr;
    };

    QWidget *createPane(Pane &pane, const QString &name);
    QWidget *createControlPanel();
    QWidget *createCameraGroup(int index);
    void connectStreams();
    void disconnectStreams();
    void setStatus(Pane &pane, const QString &text);
    QUrl streamUrl(int index) const;
    QString controlHost() const;
    void setCameraControlsEnabled(bool enabled);
    void pollStatus();
    void applyExposure(int camera);
    void applyGain(int camera);
    void applyTrigger(int camera, int item);
    void applySync(bool enabled);
    void fireTrigger(int camera);
    void applyIsp(int camera, const QString &param, const QJsonValue &value);
    void applyIspCombo(int camera, const QString &param, int item);
    void applyIspSpin(int camera, const QString &param, QDoubleSpinBox *box);
    void seedIspControls(CameraControls &controls, const QJsonObject &isp);
    void runDiscovery();
    void showRequestError(const QString &what, const QJsonObject &error);

    QLineEdit *m_hostEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_discoverButton = nullptr;
    QMenu *m_discoverMenu = nullptr;
    Pane m_panes[2];
    bool m_connected = false;

    ControlClient *m_control = nullptr;
    DiscoveryClient *m_discovery = nullptr;
    QStringList m_discoveredHosts;
    QTimer *m_statusTimer = nullptr;
    QLabel *m_controlStatus = nullptr;
    QLabel *m_deviceStatus = nullptr;
    QLabel *m_errorLabel = nullptr;
    QCheckBox *m_syncCheck = nullptr;
    CameraControls m_cameraControls[2];
    bool m_controlsPopulated = false;
};
