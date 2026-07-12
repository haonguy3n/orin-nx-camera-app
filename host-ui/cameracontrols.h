#pragma once

#include <QJsonObject>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;

// Per-camera control widgets: exposure, gain, trigger, fire, zoom, and
// ISP overrides (WB, saturation, TNR, EE, AE comp). Emits high-level
// signals when the user changes a value — the orchestrator maps these
// to control-channel requests.
class CameraControls : public QWidget
{
    Q_OBJECT

public:
    explicit CameraControls(int cameraIndex, QWidget *parent = nullptr);

    void setEnabled(bool enabled);
    int cameraIndex() const { return m_index; }

    // Seed widget values from a get-status camera object (first poll only).
    void seedFromStatus(const QJsonObject &camera);
    // Seed ISP widgets from the "isp" sub-object of a camera status.
    void seedIsp(const QJsonObject &isp);

    // Read current widget values (for seeding / external queries).
    int exposureValue() const;
    double gainValue() const;
    double zoomValue() const;

signals:
    // High-level user-action signals — fired only on real user edits,
    // not on programmatic seeding.
    void exposureChanged(int camera, int us);
    void gainChanged(int camera, double gain);
    void triggerChanged(int camera, int mode);
    void fireRequested(int camera);
    void zoomChanged(int camera, double factor);
    void ispComboChanged(int camera, const QString &param, int value);
    void ispSpinChanged(int camera, const QString &param, double value);

private:
    int m_index;
    QSpinBox *m_exposure = nullptr;
    QDoubleSpinBox *m_gain = nullptr;
    QComboBox *m_trigger = nullptr;
    QPushButton *m_fire = nullptr;
    QDoubleSpinBox *m_zoom = nullptr;
    QComboBox *m_wbMode = nullptr;
    QDoubleSpinBox *m_saturation = nullptr;
    QComboBox *m_tnrMode = nullptr;
    QDoubleSpinBox *m_tnrStrength = nullptr;
    QComboBox *m_eeMode = nullptr;
    QDoubleSpinBox *m_eeStrength = nullptr;
    QDoubleSpinBox *m_exposureComp = nullptr;

    void setupUi();
    void setupIspSection(QWidget *parent);
};
