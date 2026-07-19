#include "controlclient.h"

#include <QJsonDocument>
#include <QTcpSocket>

namespace {

// Synthetic (client-side) error object, distinguishable from server errors
// which use JSON-RPC-flavored codes (PROTOCOL.md).
QJsonObject makeError(const QString &message)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), -1);
    error.insert(QStringLiteral("message"), message);
    return error;
}

} // namespace

ControlClient::ControlClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &ControlClient::connected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ControlClient::readLines);

    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        failAllPending(QStringLiteral("control connection closed"));
        emit disconnected();
    });

    connect(m_socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                // On a fatal error (e.g. remote close) disconnected() follows;
                // failing pending requests here too covers connect failures.
                const QString message = m_socket->errorString();
                failAllPending(message);
                emit errorOccurred(message);
            });
}

void ControlClient::connectToDevice(const QString &host, quint16 port)
{
    disconnectFromDevice();
    m_socket->connectToHost(host, port);
}

void ControlClient::disconnectFromDevice()
{
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        return;

    // Tear down silently, then report once: abort() may or may not emit
    // disconnected()/errorOccurred() depending on the socket state.
    const bool wasConnected = isConnected();
    m_socket->blockSignals(true);
    m_socket->abort();
    m_socket->blockSignals(false);

    failAllPending(QStringLiteral("disconnected"));
    if (wasConnected)
        emit disconnected();
}

bool ControlClient::isConnected() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void ControlClient::sendRequest(const QString &method, const QJsonObject &params,
                                Callback callback)
{
    if (!isConnected()) {
        if (callback)
            callback(QJsonObject(), makeError(QStringLiteral("not connected")));
        return;
    }

    const qint64 id = m_nextId++;
    QJsonObject request;
    request.insert(QStringLiteral("id"), id);
    request.insert(QStringLiteral("method"), method);
    if (!params.isEmpty())
        request.insert(QStringLiteral("params"), params);

    m_pending.insert(id, std::move(callback));
    m_socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

void ControlClient::flush()
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->flush();
        m_socket->waitForBytesWritten(200);
    }
}

void ControlClient::readLines()
{
    while (m_socket->canReadLine()) {
        const QByteArray line = m_socket->readLine().trimmed();
        if (line.isEmpty())
            continue;

        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            emit errorOccurred(QStringLiteral("malformed control response"));
            continue;
        }

        const QJsonObject response = doc.object();

        // A line with no "id" is a server-initiated event, not a reply. The
        // control protocol is no longer strictly request/response: the device
        // pushes detection boxes this way in network mode. Checked before the
        // id lookup, since an event would otherwise be silently dropped as an
        // unmatched response.
        if (!response.contains(QStringLiteral("id")) &&
            response.contains(QStringLiteral("event"))) {
            emit eventReceived(
                response.value(QStringLiteral("event")).toString(),
                response.value(QStringLiteral("camera")).toInt(-1),
                response.value(QStringLiteral("data")).toObject());
            continue;
        }

        // The server echoes our numeric id verbatim; unknown/absent ids
        // (or "id": null for our own malformed requests) match no callback.
        const qint64 id = static_cast<qint64>(
            response.value(QStringLiteral("id")).toDouble(-1));
        const Callback callback = m_pending.take(id);
        if (!callback)
            continue;

        callback(response.value(QStringLiteral("result")).toObject(),
                 response.value(QStringLiteral("error")).toObject());
    }
}

void ControlClient::failAllPending(const QString &message)
{
    if (m_pending.isEmpty())
        return;

    // Detach first: a callback may call sendRequest() and mutate m_pending.
    const QHash<qint64, Callback> pending = std::move(m_pending);
    m_pending.clear();

    const QJsonObject error = makeError(message);
    for (const Callback &callback : pending) {
        if (callback)
            callback(QJsonObject(), error);
    }
}
