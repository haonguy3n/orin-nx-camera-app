#pragma once

#include <QJsonObject>
#include <QScrollArea>

class CameraControls;
class QCheckBox;
class QLabel;
class QPushButton;

// Right-hand sidebar: Device status section (control channel state, per-camera
// poll results, sync trigger, calibrate button, error line) plus two
// CameraControls sections. All user actions are forwarded as signals — the
// orchestrator maps them to control-channel requests.
class ControlPanel : public QScrollArea
{
    Q_OBJECT

public:
    explicit ControlPanel(QWidget *parent = nullptr);

    void setControlsEnabled(bool enabled);
    void setControlStatus(const QString &text);
    void setDeviceStatus(const QString &text);
    void setError(const QString &text);
    void clearError();
    void setCalibrateEnabled(bool enabled);
    void setSyncChecked(bool checked);

    // Access camera control widgets for seeding from get-status.
    CameraControls *cameraControls(int index) const;

    // Sync trigger checkbox state (for programmatic revert on error).
    bool syncChecked() const;

signals:
    void syncToggled(bool enabled);
    void calibrateRequested();
    void cameraExposureChanged(int camera, int us);
    void cameraGainChanged(int camera, double gain);
    void cameraTriggerChanged(int camera, int mode);
    void cameraFireRequested(int camera);
    void cameraZoomChanged(int camera, double factor);
    void cameraIspComboChanged(int camera, const QString &param, int value);
    void cameraIspSpinChanged(int camera, const QString &param, double value);

private:
    QLabel *m_controlStatus = nullptr;
    QLabel *m_deviceStatus = nullptr;
    QLabel *m_errorLabel = nullptr;
    QCheckBox *m_syncCheck = nullptr;
    QPushButton *m_calibrateButton = nullptr;
    CameraControls *m_cameras[2] = {nullptr, nullptr};
};
