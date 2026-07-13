#pragma once

#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QTcpSocket;

/// Binary file upload client for OTA updates.
///
/// Connects to the device's update server (port 8557), sends a JSON
/// header with the file size, waits for an ACK, then streams the .swu
/// file as raw bytes in flow-controlled chunks. Emits progress signals
/// during upload and a finished signal with the server's final response.
class UpdateClient : public QObject
{
    Q_OBJECT

public:
    explicit UpdateClient(QObject *parent = nullptr);

    /// Uploads |filePath| to |host|:|port|. Emits uploadProgress during
    /// the transfer and uploadFinished when done. Only one upload at a
    /// time; calling again while busy is ignored.
    void uploadFile(const QString &host, quint16 port,
                    const QString &filePath);

    /// Cancels an in-progress upload.
    void cancel();

    bool isBusy() const;

signals:
    void uploadProgress(qint64 sent, qint64 total);
    void uploadFinished(const QJsonObject &response);
    void uploadError(const QString &message);

private:
    void onConnected();
    void onReadyRead();
    void onBytesWritten(qint64 bytes);
    void startFileStreaming();
    void writeNextChunk();
    void finish(const QJsonObject &response, const QString &error);

    QTcpSocket *m_socket = nullptr;
    QFile m_file;
    QString m_filePath;
    qint64 m_fileSize = 0;
    qint64 m_sent = 0;       // file bytes confirmed written to kernel
    bool m_headerSent = false;
    bool m_acked = false;    // device ACKed our header
    bool m_busy = false;
};
