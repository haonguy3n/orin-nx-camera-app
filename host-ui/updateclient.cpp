#include "updateclient.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

UpdateClient::UpdateClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &UpdateClient::onConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &UpdateClient::onReadyRead);

    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        if (m_busy) {
            // Unexpected disconnect during upload
            finish(QJsonObject(), QStringLiteral("connection closed by device"));
        }
    });

    connect(m_socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                if (m_busy)
                    finish(QJsonObject(), m_socket->errorString());
            });

    // When the kernel's send buffer drains, write more file data.
    // This provides backpressure: we only queue a bounded amount of
    // data in Qt's internal buffer instead of all 600 MB at once.
    connect(m_socket, &QTcpSocket::bytesWritten, this,
            &UpdateClient::onBytesWritten);
}

void UpdateClient::uploadFile(const QString &host, quint16 port,
                              const QString &filePath)
{
    if (m_busy)
        return;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit uploadError(QStringLiteral("Cannot open %1: %2")
                             .arg(filePath, file.errorString()));
        return;
    }
    m_fileSize = file.size();
    file.close();

    if (m_fileSize <= 0) {
        emit uploadError(QStringLiteral("File is empty or unreadable"));
        return;
    }

    m_filePath = filePath;
    m_sent = 0;
    m_acked = false;
    m_busy = true;
    m_socket->connectToHost(host, port);
}

void UpdateClient::cancel()
{
    if (!m_busy)
        return;
    finish(QJsonObject(), QStringLiteral("cancelled"));
}

bool UpdateClient::isBusy() const
{
    return m_busy;
}

void UpdateClient::onConnected()
{
    // Send JSON header: {"size": N}\n
    QJsonObject header;
    header.insert(QStringLiteral("size"), m_fileSize);
    const QByteArray json =
        QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n';
    m_socket->write(json);
    // Don't send file data yet — wait for the device's ACK (onReadyRead).
}

void UpdateClient::onReadyRead()
{
    while (m_socket->canReadLine()) {
        const QByteArray line = m_socket->readLine().trimmed();
        if (line.isEmpty())
            continue;

        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            finish(QJsonObject(), QStringLiteral("malformed device response"));
            return;
        }
        const QJsonObject response = doc.object();
        const bool ok = response.value(QStringLiteral("ok")).toBool(false);

        if (!m_acked) {
            // First response is the ACK to our header
            if (!ok) {
                finish(response,
                       response.value(QStringLiteral("error"))
                           .toString(QStringLiteral("device rejected upload")));
                return;
            }
            m_acked = true;
            startFileStreaming();
            return;
        }

        // Second response is the final result after upload
        if (ok)
            finish(response, QString());
        else
            finish(response,
                   response.value(QStringLiteral("error"))
                       .toString(QStringLiteral("unknown error")));
        return;
    }
}

void UpdateClient::startFileStreaming()
{
    m_file.setFileName(m_filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        finish(QJsonObject(),
               QStringLiteral("Cannot re-open %1: %2").arg(
                   m_filePath, m_file.errorString()));
        return;
    }
    // Kick off the first chunk; subsequent chunks are driven by
    // onBytesWritten as the kernel send buffer drains.
    writeNextChunk();
}

void UpdateClient::writeNextChunk()
{
    // Keep Qt's internal write buffer filled with up to ~4 MB of
    // pending data. This provides smooth streaming without loading
    // the entire file into memory.
    constexpr qint64 kTargetBuffer = 4 * 1024 * 1024;
    constexpr qint64 kChunkSize = 65536;

    while (m_file.isOpen() &&
           m_socket->bytesToWrite() < kTargetBuffer &&
           m_sent < m_fileSize) {
        const qint64 toRead =
            std::min(kChunkSize, m_fileSize - m_sent);
        const QByteArray chunk = m_file.read(toRead);
        if (chunk.isEmpty())
            break;
        m_socket->write(chunk);
        // m_sent is updated in onBytesWritten (when data actually leaves)
    }

    // If we've read all the file, close it — the socket still has
    // buffered data to flush.
    if (m_sent >= m_fileSize && m_file.isOpen())
        m_file.close();
}

void UpdateClient::onBytesWritten(qint64 bytes)
{
    if (!m_busy || !m_acked)
        return;
    m_sent += bytes;
    emit uploadProgress(m_sent, m_fileSize);

    if (m_sent < m_fileSize) {
        // More file data to send — top up the buffer
        writeNextChunk();
    }
    // If all data is sent, just wait for the device's final response
    // (handled by onReadyRead).
}

void UpdateClient::finish(const QJsonObject &response, const QString &error)
{
    m_busy = false;
    m_acked = false;
    if (m_file.isOpen())
        m_file.close();
    m_socket->abort();

    if (!error.isEmpty())
        emit uploadError(error);
    else
        emit uploadFinished(response);
}
