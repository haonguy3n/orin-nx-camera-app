#include "cameracontrols.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonValue>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include "collapsiblesection.h"

CameraControls::CameraControls(int cameraIndex, QWidget *parent)
    : QWidget(parent), m_index(cameraIndex)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setupUi();
    setEnabled(false);
}

void CameraControls::setupUi()
{
    auto *section = new CollapsibleSection(
        QStringLiteral("CAM%1").arg(m_index), this);
    section->setExpanded(false);  // collapsed by default — click to show
    layout()->addWidget(section);

    auto *form = new QFormLayout(section->contentWidget());
    form->setSpacing(6);
    form->setContentsMargins(10, 8, 10, 8);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_exposure = new QSpinBox(section->contentWidget());
    m_exposure->setRange(0, 1000000);
    m_exposure->setSuffix(QStringLiteral(" µs"));
    m_exposure->setSpecialValueText(QStringLiteral("auto")); // 0 = auto
    form->addRow(QStringLiteral("Exposure:"), m_exposure);

    // argus: analog gain multiplier (1-16); v4l2: VC driver milli-dB
    // (0-48000 = 0-48 dB, step 100).
    m_gain = new QDoubleSpinBox(section->contentWidget());
    m_gain->setRange(0.0, 48000.0);
    m_gain->setDecimals(1);
    m_gain->setSpecialValueText(QStringLiteral("auto")); // 0 = auto
    form->addRow(QStringLiteral("Gain:"), m_gain);

    // Trigger has no readable "current" semantic worth fighting over;
    // "(unchanged)" sends nothing, everything else maps to mode = row - 1.
    m_trigger = new QComboBox(section->contentWidget());
    m_trigger->addItem(QStringLiteral("(unchanged)"));
    m_trigger->addItem(QStringLiteral("0 disabled"));
    m_trigger->addItem(QStringLiteral("1 external"));
    m_trigger->addItem(QStringLiteral("2 pulse width"));
    m_trigger->addItem(QStringLiteral("3 self"));
    m_trigger->addItem(QStringLiteral("4 single"));
    m_trigger->addItem(QStringLiteral("5 sync"));
    m_trigger->addItem(QStringLiteral("6 stream edge"));
    m_trigger->addItem(QStringLiteral("7 stream level"));
    form->addRow(QStringLiteral("Trigger:"), m_trigger);

    m_fire = new QPushButton(QStringLiteral("Fire"), section->contentWidget());
    m_fire->setToolTip(QStringLiteral(
        "software single trigger — set trigger mode 4 first"));
    m_fire->setCursor(Qt::PointingHandCursor);
    form->addRow(m_fire);

    // Digital zoom (set-zoom, GPU crop + upscale). Center crop only; the
    // protocol's x/y pan stays protocol-only for now.
    m_zoom = new QDoubleSpinBox(section->contentWidget());
    m_zoom->setRange(1.0, 8.0);
    m_zoom->setDecimals(1);
    m_zoom->setSingleStep(0.5);
    m_zoom->setValue(1.0);  // 1 = full field of view
    m_zoom->setSuffix(QStringLiteral("x"));
    m_zoom->setProperty("lastSent", 1.0);
    form->addRow(QStringLiteral("Zoom:"), m_zoom);

    setupIspSection(section->contentWidget());

    // activated (not currentIndexChanged): user picks only, never programmatic.
    connect(m_trigger, &QComboBox::activated, this, [this](int item) {
        if (item > 0)
            emit triggerChanged(m_index, item - 1);
    });
    connect(m_fire, &QPushButton::clicked, this, [this]() {
        emit fireRequested(m_index);
    });
    connect(m_zoom, &QDoubleSpinBox::editingFinished, this, [this]() {
        const double factor = m_zoom->value();
        const QVariant lastSent = m_zoom->property("lastSent");
        if (lastSent.isValid() && lastSent.toDouble() == factor)
            return;
        m_zoom->setProperty("lastSent", factor);
        emit zoomChanged(m_index, factor);
    });
    connect(m_exposure, &QSpinBox::editingFinished, this, [this]() {
        const int us = m_exposure->value();
        const QVariant lastSent = m_exposure->property("lastSent");
        if (lastSent.isValid() && lastSent.toInt() == us)
            return;
        m_exposure->setProperty("lastSent", us);
        emit exposureChanged(m_index, us);
    });
    connect(m_gain, &QDoubleSpinBox::editingFinished, this, [this]() {
        const double gain = m_gain->value();
        const QVariant lastSent = m_gain->property("lastSent");
        if (lastSent.isValid() && lastSent.toDouble() == gain)
            return;
        m_gain->setProperty("lastSent", gain);
        emit gainChanged(m_index, gain);
    });
}

void CameraControls::setupIspSection(QWidget *parent)
{
    // ISP overrides (set-isp, argus source only — the server rejects the
    // whole group for v4l2/test). Same conventions as above: combos send on
    // user activation, spin boxes on editingFinished with a lastSent guard.
    auto *ispSection = new CollapsibleSection(QStringLiteral("ISP"), parent);
    ispSection->setExpanded(false);  // collapsed by default — click to show

    auto *ispForm = new QFormLayout(ispSection->contentWidget());
    ispForm->setSpacing(6);
    ispForm->setContentsMargins(10, 8, 10, 8);
    ispForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    ispForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    ispForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_wbMode = new QComboBox(ispSection->contentWidget());
    m_wbMode->addItem(QStringLiteral("(unchanged)"));
    m_wbMode->addItem(QStringLiteral("0 off"));
    m_wbMode->addItem(QStringLiteral("1 auto"));
    m_wbMode->addItem(QStringLiteral("2 incandescent"));
    m_wbMode->addItem(QStringLiteral("3 fluorescent"));
    m_wbMode->addItem(QStringLiteral("4 warm-fluorescent"));
    m_wbMode->addItem(QStringLiteral("5 daylight"));
    m_wbMode->addItem(QStringLiteral("6 cloudy-daylight"));
    m_wbMode->addItem(QStringLiteral("7 twilight"));
    m_wbMode->addItem(QStringLiteral("8 shade"));
    m_wbMode->addItem(QStringLiteral("9 manual"));
    ispForm->addRow(QStringLiteral("WB mode:"), m_wbMode);

    m_saturation = new QDoubleSpinBox(ispSection->contentWidget());
    m_saturation->setRange(0.0, 2.0);
    m_saturation->setDecimals(2);
    m_saturation->setSingleStep(0.1);
    m_saturation->setValue(1.0);  // 1 = neutral
    m_saturation->setProperty("lastSent", 1.0);
    ispForm->addRow(QStringLiteral("Saturation:"), m_saturation);

    m_tnrMode = new QComboBox(ispSection->contentWidget());
    m_tnrMode->addItem(QStringLiteral("(unchanged)"));
    m_tnrMode->addItem(QStringLiteral("0 off"));
    m_tnrMode->addItem(QStringLiteral("1 fast"));
    m_tnrMode->addItem(QStringLiteral("2 high quality"));
    ispForm->addRow(QStringLiteral("TNR mode:"), m_tnrMode);

    m_tnrStrength = new QDoubleSpinBox(ispSection->contentWidget());
    m_tnrStrength->setRange(-1.0, 1.0);
    m_tnrStrength->setDecimals(2);
    m_tnrStrength->setSingleStep(0.1);
    m_tnrStrength->setValue(-1.0);
    m_tnrStrength->setSpecialValueText(QStringLiteral("auto")); // -1
    m_tnrStrength->setProperty("lastSent", -1.0);
    ispForm->addRow(QStringLiteral("TNR strength:"), m_tnrStrength);

    m_eeMode = new QComboBox(ispSection->contentWidget());
    m_eeMode->addItem(QStringLiteral("(unchanged)"));
    m_eeMode->addItem(QStringLiteral("0 off"));
    m_eeMode->addItem(QStringLiteral("1 fast"));
    m_eeMode->addItem(QStringLiteral("2 high quality"));
    ispForm->addRow(QStringLiteral("EE mode:"), m_eeMode);

    m_eeStrength = new QDoubleSpinBox(ispSection->contentWidget());
    m_eeStrength->setRange(-1.0, 1.0);
    m_eeStrength->setDecimals(2);
    m_eeStrength->setSingleStep(0.1);
    m_eeStrength->setValue(-1.0);
    m_eeStrength->setSpecialValueText(QStringLiteral("auto")); // -1
    m_eeStrength->setProperty("lastSent", -1.0);
    ispForm->addRow(QStringLiteral("EE strength:"), m_eeStrength);

    m_exposureComp = new QDoubleSpinBox(ispSection->contentWidget());
    m_exposureComp->setRange(-2.0, 2.0);
    m_exposureComp->setDecimals(1);
    m_exposureComp->setSingleStep(0.5);
    m_exposureComp->setValue(0.0);
    m_exposureComp->setSuffix(QStringLiteral(" EV"));
    m_exposureComp->setProperty("lastSent", 0.0);
    ispForm->addRow(QStringLiteral("AE comp:"), m_exposureComp);

    auto *form = qobject_cast<QFormLayout *>(parent->layout());
    if (form)
        form->addRow(ispSection);

    connect(m_wbMode, &QComboBox::activated, this, [this](int item) {
        if (item > 0)
            emit ispComboChanged(m_index, QStringLiteral("wbmode"), item - 1);
    });
    connect(m_tnrMode, &QComboBox::activated, this, [this](int item) {
        if (item > 0)
            emit ispComboChanged(m_index, QStringLiteral("tnr-mode"), item - 1);
    });
    connect(m_eeMode, &QComboBox::activated, this, [this](int item) {
        if (item > 0)
            emit ispComboChanged(m_index, QStringLiteral("ee-mode"), item - 1);
    });
    connect(m_saturation, &QDoubleSpinBox::editingFinished, this, [this]() {
        const double value = m_saturation->value();
        const QVariant lastSent = m_saturation->property("lastSent");
        if (lastSent.isValid() && lastSent.toDouble() == value)
            return;
        m_saturation->setProperty("lastSent", value);
        emit ispSpinChanged(m_index, QStringLiteral("saturation"), value);
    });
    connect(m_tnrStrength, &QDoubleSpinBox::editingFinished, this, [this]() {
        const double value = m_tnrStrength->value();
        const QVariant lastSent = m_tnrStrength->property("lastSent");
        if (lastSent.isValid() && lastSent.toDouble() == value)
            return;
        m_tnrStrength->setProperty("lastSent", value);
        emit ispSpinChanged(m_index, QStringLiteral("tnr-strength"), value);
    });
    connect(m_eeStrength, &QDoubleSpinBox::editingFinished, this, [this]() {
        const double value = m_eeStrength->value();
        const QVariant lastSent = m_eeStrength->property("lastSent");
        if (lastSent.isValid() && lastSent.toDouble() == value)
            return;
        m_eeStrength->setProperty("lastSent", value);
        emit ispSpinChanged(m_index, QStringLiteral("ee-strength"), value);
    });
    connect(m_exposureComp, &QDoubleSpinBox::editingFinished, this, [this]() {
        const double value = m_exposureComp->value();
        const QVariant lastSent = m_exposureComp->property("lastSent");
        if (lastSent.isValid() && lastSent.toDouble() == value)
            return;
        m_exposureComp->setProperty("lastSent", value);
        emit ispSpinChanged(m_index, QStringLiteral("exposurecompensation"),
                            value);
    });
}

void CameraControls::setEnabled(bool enabled)
{
    QWidget::setEnabled(enabled);
}

void CameraControls::seedFromStatus(const QJsonObject &camera)
{
    const int us = camera.value(QStringLiteral("exposure")).toInt();
    m_exposure->setValue(us);
    m_exposure->setProperty("lastSent", us);
    const double gain = camera.value(QStringLiteral("gain")).toDouble();
    m_gain->setValue(gain);
    m_gain->setProperty("lastSent", gain);
    const double zoom =
        camera.value(QStringLiteral("zoom")).toDouble(1.0);
    m_zoom->setValue(zoom);
    m_zoom->setProperty("lastSent", zoom);
    seedIsp(camera.value(QStringLiteral("isp")).toObject());
}

void CameraControls::seedIsp(const QJsonObject &isp)
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
    const auto seedSpin = [&number](QDoubleSpinBox *box,
                                    const QString &param) {
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

    seedCombo(m_wbMode, QStringLiteral("wbmode"));
    seedSpin(m_saturation, QStringLiteral("saturation"));
    seedCombo(m_tnrMode, QStringLiteral("tnr-mode"));
    seedSpin(m_tnrStrength, QStringLiteral("tnr-strength"));
    seedCombo(m_eeMode, QStringLiteral("ee-mode"));
    seedSpin(m_eeStrength, QStringLiteral("ee-strength"));
    seedSpin(m_exposureComp, QStringLiteral("exposurecompensation"));
}

int CameraControls::exposureValue() const
{
    return m_exposure->value();
}

double CameraControls::gainValue() const
{
    return m_gain->value();
}

double CameraControls::zoomValue() const
{
    return m_zoom->value();
}
