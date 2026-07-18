#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>

#include <functional>

class QTcpSocket;

// JSON-over-TCP control channel client (see ../proto/PROTOCOL.md):
// newline-delimited JSON objects, request/response matched by "id".
class ControlClient : public QObject
{
    Q_OBJECT

public:
    // Called with the response: exactly one of result/error is meaningful;
    // error is an empty object on success, {"code", "message"} on failure.
    using Callback =
        std::function<void(const QJsonObject &result, const QJsonObject &error)>;

    explicit ControlClient(QObject *parent = nullptr);

    void connectToDevice(const QString &host, quint16 port);
    void disconnectFromDevice();
    bool isConnected() const;

    void sendRequest(const QString &method, const QJsonObject &params,
                     Callback callback);

    // Push buffered request bytes and wait briefly for them to leave, so a
    // final command (set-stream stop) reaches the wire before a disconnect.
    void flush();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);

private:
    void readLines();
    void failAllPending(const QString &message);

    QTcpSocket *m_socket = nullptr;
    QHash<qint64, Callback> m_pending;
    qint64 m_nextId = 1;
};
