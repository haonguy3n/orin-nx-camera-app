#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>

class QTimer;
class QUdpSocket;

// UDP device discovery (see docs/PROTOCOL.md "Discovery"): broadcast
// {"method": "discover"} to port 8556, each device replies with one JSON
// datagram; the device address is the reply's source address.
class DiscoveryClient : public QObject
{
    Q_OBJECT

public:
    explicit DiscoveryClient(QObject *parent = nullptr);

    void discover(int timeoutMs = 1500);
    bool isRunning() const;

signals:
    // Emitted once per distinct device address; info is the reply object.
    void deviceFound(const QString &address, const QJsonObject &info);
    void finished();

private:
    void readReplies();

    QUdpSocket *m_socket = nullptr;
    QTimer *m_timer = nullptr;
    QSet<QString> m_seen;
};
