#pragma once

#include <QMainWindow>
#include <QUrl>

class ControlClient;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QJsonObject;
class QLabel;
class QLineEdit;
class QMediaPlayer;
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
    void showRequestError(const QString &what, const QJsonObject &error);

    QLineEdit *m_hostEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    Pane m_panes[2];
    bool m_connected = false;

    ControlClient *m_control = nullptr;
    QTimer *m_statusTimer = nullptr;
    QLabel *m_controlStatus = nullptr;
    QLabel *m_deviceStatus = nullptr;
    QLabel *m_errorLabel = nullptr;
    CameraControls m_cameraControls[2];
    bool m_controlsPopulated = false;
};
