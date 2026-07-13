#pragma once

#include <QWidget>

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

signals:
    /// Emitted when the user picks a file and clicks Upload.
    void uploadRequested(const QString &filePath);

private:
    void onPickFile();
    void onUpload();

    QLabel *m_fileLabel = nullptr;
    QPushButton *m_pickButton = nullptr;
    QPushButton *m_uploadButton = nullptr;
    QProgressBar *m_uploadProgress = nullptr;
    QLabel *m_statusLabel = nullptr;

    QString m_filePath;
};
