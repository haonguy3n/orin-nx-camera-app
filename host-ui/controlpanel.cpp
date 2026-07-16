#include "controlpanel.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "cameracontrols.h"
#include "collapsiblesection.h"
#include "updatewidget.h"

ControlPanel::ControlPanel(QWidget *parent)
    : QScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setMinimumWidth(340);
    setFixedWidth(360);

    auto *panel = new QWidget;
    panel->setMinimumWidth(340);

    auto *layout = new QVBoxLayout(panel);
    layout->setSpacing(2);
    layout->setContentsMargins(0, 0, 0, 0);

    // Device section: control channel state, per-camera poll results,
    // sync trigger, calibrate button, and the last request error.
    auto *deviceSection = new CollapsibleSection(
        QStringLiteral("DEVICE"), panel);
    auto *statusLayout = new QVBoxLayout(deviceSection->contentWidget());
    statusLayout->setSpacing(6);
    statusLayout->setContentsMargins(10, 8, 10, 8);

    m_controlStatus = new QLabel(
        QStringLiteral("control: disconnected"), deviceSection->contentWidget());
    m_controlStatus->setObjectName(QStringLiteral("mono"));

    m_deviceStatus = new QLabel(deviceSection->contentWidget());
    m_deviceStatus->setObjectName(QStringLiteral("mono"));
    m_deviceStatus->setWordWrap(true);

    m_errorLabel = new QLabel(deviceSection->contentWidget());
    m_errorLabel->setObjectName(QStringLiteral("error"));
    m_errorLabel->setWordWrap(true);

    m_syncCheck = new QCheckBox(
        QStringLiteral("Sync trigger"), deviceSection->contentWidget());
    m_syncCheck->setToolTip(QStringLiteral(
        "hardware-synchronized capture: all cameras -> external trigger"));
    m_syncCheck->setEnabled(false);

    m_calibrateButton = new QPushButton(
        QStringLiteral("Calibrate whites"), deviceSection->contentWidget());
    m_calibrateButton->setObjectName(QStringLiteral("accent"));
    m_calibrateButton->setToolTip(
        QStringLiteral("point cam0 at something white/gray first"));
    m_calibrateButton->setEnabled(false);
    m_calibrateButton->setCursor(Qt::PointingHandCursor);

    statusLayout->addWidget(m_controlStatus);
    statusLayout->addWidget(m_deviceStatus);
    statusLayout->addWidget(m_syncCheck);
    statusLayout->addWidget(m_calibrateButton);
    statusLayout->addWidget(m_errorLabel);
    layout->addWidget(deviceSection);

    // clicked (not toggled): user toggles only, never programmatic reverts.
    connect(m_syncCheck, &QCheckBox::clicked, this,
            [this](bool enabled) { emit syncToggled(enabled); });
    connect(m_calibrateButton, &QPushButton::clicked, this,
            [this]() { emit calibrateRequested(); });

    // Two camera control sections.
    for (int i = 0; i < 2; ++i) {
        m_cameras[i] = new CameraControls(i, panel);
        layout->addWidget(m_cameras[i]);

        // Forward camera control signals to the panel's own signals.
        connect(m_cameras[i], &CameraControls::exposureChanged, this,
                &ControlPanel::cameraExposureChanged);
        connect(m_cameras[i], &CameraControls::gainChanged, this,
                &ControlPanel::cameraGainChanged);
        connect(m_cameras[i], &CameraControls::triggerChanged, this,
                &ControlPanel::cameraTriggerChanged);
        connect(m_cameras[i], &CameraControls::fireRequested, this,
                &ControlPanel::cameraFireRequested);
        connect(m_cameras[i], &CameraControls::zoomChanged, this,
                &ControlPanel::cameraZoomChanged);
        connect(m_cameras[i], &CameraControls::ispComboChanged, this,
                &ControlPanel::cameraIspComboChanged);
        connect(m_cameras[i], &CameraControls::ispSpinChanged, this,
                &ControlPanel::cameraIspSpinChanged);
    }

    // OTA update section.
    auto *updateSection = new CollapsibleSection(
        QStringLiteral("FIRMWARE UPDATE"), panel);
    m_updateWidget = new UpdateWidget(updateSection->contentWidget());
    auto *updateLayout = new QVBoxLayout(updateSection->contentWidget());
    updateLayout->setSpacing(6);
    updateLayout->setContentsMargins(10, 8, 10, 8);
    updateLayout->addWidget(m_updateWidget);
    updateSection->setExpanded(false);
    layout->addWidget(updateSection);

    connect(m_updateWidget, &UpdateWidget::uploadRequested, this,
            &ControlPanel::uploadRequested);
    connect(m_updateWidget, &UpdateWidget::rebootRequested, this,
            &ControlPanel::rebootRequested);

    layout->addStretch(1);

    setWidget(panel);
}

void ControlPanel::setControlsEnabled(bool enabled)
{
    for (CameraControls *c : m_cameras)
        c->setEnabled(enabled);
    m_syncCheck->setEnabled(enabled);
    m_updateWidget->setUpdateEnabled(enabled);
}

void ControlPanel::setControlStatus(const QString &text)
{
    m_controlStatus->setText(text);
}

void ControlPanel::setDeviceStatus(const QString &text)
{
    m_deviceStatus->setText(text);
}

void ControlPanel::setError(const QString &text)
{
    m_errorLabel->setText(text);
}

void ControlPanel::clearError()
{
    m_errorLabel->clear();
}

void ControlPanel::setCalibrateEnabled(bool enabled)
{
    m_calibrateButton->setEnabled(enabled);
}

void ControlPanel::setSyncChecked(bool checked)
{
    m_syncCheck->setChecked(checked);
}

CameraControls *ControlPanel::cameraControls(int index) const
{
    return m_cameras[index];
}

UpdateWidget *ControlPanel::updateWidget() const
{
    return m_updateWidget;
}

bool ControlPanel::syncChecked() const
{
    return m_syncCheck->isChecked();
}
