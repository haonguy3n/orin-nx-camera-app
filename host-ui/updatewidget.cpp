#include "updatewidget.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

UpdateWidget::UpdateWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);
    layout->setContentsMargins(0, 0, 0, 0);

    m_fileLabel = new QLabel(QStringLiteral("No file selected"), this);
    m_fileLabel->setObjectName(QStringLiteral("mono"));
    m_fileLabel->setWordWrap(true);

    m_pickButton = new QPushButton(QStringLiteral("Select .swu file..."), this);
    m_pickButton->setCursor(Qt::PointingHandCursor);

    m_uploadButton = new QPushButton(QStringLiteral("Upload & Install"), this);
    m_uploadButton->setObjectName(QStringLiteral("accent"));
    m_uploadButton->setCursor(Qt::PointingHandCursor);
    m_uploadButton->setEnabled(false);

    m_autoRebootCheck = new QCheckBox(
        QStringLiteral("Auto-reboot after update"), this);
    m_autoRebootCheck->setToolTip(QStringLiteral(
        "automatically reboot the device when the update completes"));

    m_rebootButton = new QPushButton(QStringLiteral("Reboot device"), this);
    m_rebootButton->setCursor(Qt::PointingHandCursor);
    m_rebootButton->setEnabled(false);

    m_uploadProgress = new QProgressBar(this);
    m_uploadProgress->setRange(0, 100);
    m_uploadProgress->setValue(0);
    m_uploadProgress->setVisible(false);
    m_uploadProgress->setTextVisible(true);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("mono"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setText(QStringLiteral("Status: idle"));

    layout->addWidget(m_pickButton);
    layout->addWidget(m_fileLabel);
    layout->addWidget(m_uploadButton);
    layout->addWidget(m_autoRebootCheck);
    layout->addWidget(m_rebootButton);
    layout->addWidget(m_uploadProgress);
    layout->addWidget(m_statusLabel);

    connect(m_pickButton, &QPushButton::clicked, this,
            &UpdateWidget::onPickFile);
    connect(m_uploadButton, &QPushButton::clicked, this,
            &UpdateWidget::onUpload);
    connect(m_rebootButton, &QPushButton::clicked, this,
            &UpdateWidget::onReboot);
}

void UpdateWidget::setUpdateEnabled(bool enabled)
{
    m_pickButton->setEnabled(enabled);
    // Upload button stays disabled until a file is picked
    m_uploadButton->setEnabled(enabled && !m_filePath.isEmpty());
    m_rebootButton->setEnabled(enabled);
}

void UpdateWidget::setUploadProgress(qint64 sent, qint64 total)
{
    const int percent = total > 0 ? static_cast<int>(sent * 100 / total) : 0;
    const double sentMB = sent / (1024.0 * 1024.0);
    const double totalMB = total / (1024.0 * 1024.0);

    m_uploadProgress->setVisible(true);
    m_uploadProgress->setValue(percent);
    m_uploadProgress->setFormat(QStringLiteral("Uploading %p%"));

    m_statusLabel->setText(
        QStringLiteral("Status: uploading %1/%2 MB (%3%)")
            .arg(sentMB, 0, 'f', 1)
            .arg(totalMB, 0, 'f', 1)
            .arg(percent));
}

void UpdateWidget::updateStatus(const QJsonObject &status)
{
    const QString state =
        status.value(QStringLiteral("state")).toString(QStringLiteral("idle"));
    const int percent =
        static_cast<int>(status.value(QStringLiteral("percent")).toDouble(0));
    const int step =
        static_cast<int>(status.value(QStringLiteral("step")).toDouble(0));
    const int total =
        static_cast<int>(
            status.value(QStringLiteral("total_steps")).toDouble(0));
    const QString error =
        status.value(QStringLiteral("error")).toString();

    // Build status text
    QString text = QStringLiteral("Status: %1").arg(state);
    if (state == QStringLiteral("uploading")) {
        text += QStringLiteral(" (%1%)").arg(percent);
    } else if (state == QStringLiteral("installing") && total > 0) {
        text += QStringLiteral(" (step %1/%2, %3%)").arg(step).arg(total).arg(percent);
    } else if (state == QStringLiteral("installing")) {
        text += QStringLiteral(" (%1%)").arg(percent);
    } else if (state == QStringLiteral("success") ||
               state == QStringLiteral("done")) {
        text = QStringLiteral("Status: update complete — reboot the device");
    }
    if (!error.isEmpty())
        text += QStringLiteral(" — %1").arg(error);

    m_statusLabel->setText(text);

    // Progress bar: visible during upload and install, hidden otherwise
    if (state == QStringLiteral("uploading")) {
        m_uploadProgress->setVisible(true);
        m_uploadProgress->setValue(percent);
        m_uploadProgress->setFormat(QStringLiteral("Uploading %p%"));
    } else if (state == QStringLiteral("installing")) {
        m_uploadProgress->setVisible(true);
        m_uploadProgress->setValue(percent);
        m_uploadProgress->setFormat(QStringLiteral("Installing %p%"));
    } else if (state == QStringLiteral("success") ||
               state == QStringLiteral("done")) {
        m_uploadProgress->setVisible(true);
        m_uploadProgress->setValue(100);
        m_uploadProgress->setFormat(QStringLiteral("Done"));
    } else if (state == QStringLiteral("failure")) {
        m_uploadProgress->setVisible(false);
    } else {
        // idle or unknown
        m_uploadProgress->setVisible(false);
        m_uploadProgress->setValue(0);
    }
}

void UpdateWidget::onPickFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select SWUpdate package"),
        QString(), QStringLiteral("SWUpdate packages (*.swu);;All files (*)"));
    if (path.isEmpty())
        return;

    m_filePath = path;
    const QFileInfo fi(path);
    m_fileLabel->setText(fi.fileName());
    m_uploadButton->setEnabled(true);
}

void UpdateWidget::onUpload()
{
    if (m_filePath.isEmpty())
        return;
    emit uploadRequested(m_filePath);
}

void UpdateWidget::onReboot()
{
    const auto ret = QMessageBox::question(
        this, QStringLiteral("Reboot device"),
        QStringLiteral("Reboot the device now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret == QMessageBox::Yes)
        emit rebootRequested();
}

bool UpdateWidget::autoRebootChecked() const
{
    return m_autoRebootCheck->isChecked();
}
