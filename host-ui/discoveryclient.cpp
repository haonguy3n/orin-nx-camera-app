#include "discoveryclient.h"

#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

#include "proto/Protocol.h"

namespace {

constexpr quint16 kDiscoveryPort = proto::kDiscoveryPort;

} // namespace

DiscoveryClient::DiscoveryClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_socket->close();  // drop late replies
        emit finished();
    });

    connect(m_socket, &QUdpSocket::readyRead, this,
            &DiscoveryClient::readReplies);
}

void DiscoveryClient::discover(int timeoutMs)
{
    if (isRunning())
        return;

    m_seen.clear();
    m_socket->bind();  // ephemeral port; replies land here

    QJsonObject request;
    request.insert(QStringLiteral("method"), QLatin1String(proto::methods::kDiscover));
    const QByteArray datagram =
        QJsonDocument(request).toJson(QJsonDocument::Compact);

    // Targets: the global broadcast, every up interface's IPv4 broadcast
    // (some networks filter 255.255.255.255), and loopback unicast —
    // broadcasts don't traverse lo, so local testing needs it. The QSet
    // drops duplicate broadcast addresses across interfaces.
    QSet<QHostAddress> targets;
    targets.insert(QHostAddress(QHostAddress::Broadcast));
    targets.insert(QHostAddress(QHostAddress::LocalHost));
    const QList<QNetworkInterface> interfaces =
        QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp)
            || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress broadcast = entry.broadcast();
            if (!broadcast.isNull())
                targets.insert(broadcast);
        }
    }
    for (const QHostAddress &target : targets)
        m_socket->writeDatagram(datagram, target, kDiscoveryPort);

    m_timer->start(timeoutMs);
}

bool DiscoveryClient::isRunning() const
{
    return m_timer->isActive();
}

void DiscoveryClient::readReplies()
{
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram();

        const QJsonDocument doc = QJsonDocument::fromJson(datagram.data());
        if (!doc.isObject())
            continue;
        const QJsonObject info = doc.object();
        // A device reply carries at least these two; anything else —
        // including our own broadcast looping back — is not a device.
        if (!info.contains(QStringLiteral("device"))
            || !info.contains(QStringLiteral("control_port")))
            continue;

        // The device address is the reply's source address (PROTOCOL.md);
        // normalize IPv4-mapped IPv6 so deduplication works.
        const QHostAddress sender = datagram.senderAddress();
        bool isV4 = false;
        const quint32 v4 = sender.toIPv4Address(&isV4);
        const QString address =
            isV4 ? QHostAddress(v4).toString() : sender.toString();
        if (m_seen.contains(address))
            continue;
        m_seen.insert(address);

        emit deviceFound(address, info);
    }
}
