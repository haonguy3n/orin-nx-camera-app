#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;

/// OTA update widget: file picker + upload button + progress bar + status.
///
/// Lives in the control panel sidebar. Uses UpdateClient to stream the
/// .swu file to the device, and polls get-update-status on the control
/// channel for installation progress.
class UpdateWidget : public QWidget
{
    Q_OBJECT

public:
    explicit UpdateWidget(QWidget *parent = nullptr);

    void setUpdateEnabled(bool enabled);

    /// Called by MainWindow with the latest get-update-status response.
    void updateStatus(const QJsonObject &status);

    /// Called by MainWindow during file upload to show byte progress.
    void setUploadProgress(qint64 sent, qint64 total);

    /// True if the auto-reboot checkbox is checked.
    bool autoRebootChecked() const;

signals:
    /// Emitted when the user picks a file and clicks Upload.
    void uploadRequested(const QString &filePath);

    /// Emitted when the user clicks the Reboot button.
    void rebootRequested();

private:
    void onPickFile();
    void onUpload();
    void onReboot();

    QLabel *m_fileLabel = nullptr;
    QPushButton *m_pickButton = nullptr;
    QPushButton *m_uploadButton = nullptr;
    QCheckBox *m_autoRebootCheck = nullptr;
    QPushButton *m_rebootButton = nullptr;
    QProgressBar *m_uploadProgress = nullptr;
    QLabel *m_statusLabel = nullptr;

    QString m_filePath;
};
