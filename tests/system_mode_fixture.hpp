#pragma once

#include <QtTest>
#include <QNetworkProxy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>
#include <memory>

// Shared by opt-in host acceptance and a standalone loopback-only fixture test.
namespace SystemModeAcceptance {

struct Destination {
    quint8 command = 0;
    quint8 type = 0;
    QString host;
    quint16 port = 0;
    QString Description() const {
        return QString("cmd=%1 atyp=%2 host=%3 port=%4").arg(command).arg(type).arg(host).arg(port);
    }
    bool Is(const QString &address, quint16 expectedPort) const {
        const QHostAddress left(host), right(address);
        const bool sameHost = !left.isNull() && !right.isNull() ? left == right : host.compare(address, Qt::CaseInsensitive) == 0;
        return sameHost && port == expectedPort;
    }
};

inline int AddressLength(const QByteArray &packet, int offset) {
    if (packet.size() <= offset) return -1;
    const auto type = quint8(packet[offset]);
    if (type == 1) return packet.size() >= offset + 7 ? 7 : -1;
    if (type == 4) return packet.size() >= offset + 19 ? 19 : -1;
    if (type == 3 && packet.size() > offset + 1) {
        const auto size = 4 + quint8(packet[offset + 1]);
        return packet.size() >= offset + size && size > 4 ? size : -1;
    }
    return -1;
}

inline Destination ReadDestination(const QByteArray &packet, int offset, quint8 command = 0) {
    Destination result;
    const auto length = AddressLength(packet, offset);
    if (length < 0) return result;
    result.command = command;
    result.type = quint8(packet[offset]);
    if (result.type == 1) {
        result.host = QHostAddress(qFromBigEndian<quint32>(packet.constData() + offset + 1)).toString();
    } else if (result.type == 4) {
        Q_IPV6ADDR address{};
        for (int i = 0; i < 16; ++i) address.c[i] = quint8(packet[offset + 1 + i]);
        result.host = QHostAddress(address).toString();
    } else result.host = QString::fromLatin1(packet.mid(offset + 2, quint8(packet[offset + 1])));
    result.port = qFromBigEndian<quint16>(packet.constData() + offset + length - 2);
    return result;
}

inline QByteArray DnsQuery(quint16 type) {
    auto query = QByteArray::fromHex("123401000001000000000000");
    for (const auto &label : QByteArray("acceptance.example.test").split('.')) query += char(label.size()) + label;
    query += char(0);
    query += char(type >> 8);
    query += char(type & 255);
    return query + QByteArray::fromHex("0001");
}

inline QByteArray DnsAnswer(const QByteArray &query) {
    if (query.size() < 17) return {};
    int end = 12;
    QByteArray name;
    while (end < query.size() && quint8(query[end]) != 0) {
        const int length = quint8(query[end++]);
        if (length > 63 || end + length > query.size()) return {};
        if (!name.isEmpty()) name += '.';
        name += query.mid(end, length);
        end += length;
    }
    if (name.toLower() != "acceptance.example.test") return {};
    if (end + 5 > query.size()) return {};
    ++end;
    const auto type = (quint8(query[end]) << 8) | quint8(query[end + 1]);
    auto response = query.left(end + 4);
    response[2] = char(0x81);
    response[3] = char(0x80);
    response[6] = 0;
    response[7] = char(type == 1 || type == 28 ? 1 : 0);
    response[8] = response[9] = response[10] = response[11] = 0;
    if (type == 1 || type == 28) {
        response += QByteArray::fromHex("c00c") + query.mid(end, 4) + QByteArray::fromHex("00000005");
        response += type == 1 ? QByteArray::fromHex("0004cb007121")
                             : QByteArray::fromHex("001020010db8000000000000000000000033");
    }
    return response;
}

class SocksPeer : public QTcpServer {
public:
    QUdpSocket relay;
    QUdpSocket dns;
    int controlsAccepted = 0, controlsActive = 0, greetings = 0, associations = 0, associationsActive = 0;
    int tcpRequests = 0, udpPackets = 0, udpRequests = 0, udpReplies = 0, malformedPackets = 0;
    int dnsRequests = 0, dnsReplies = 0;
    qint64 relayRead = 0, relayWritten = 0, dnsWritten = 0;
    QString relayError, dnsError, controlError;
    QString relaySource;
    QList<Destination> controlDestinations, tcpDestinations, udpDestinations;

    SocksPeer() {
        setProxy(QNetworkProxy::NoProxy);
        relay.setProxy(QNetworkProxy::NoProxy);
        dns.setProxy(QNetworkProxy::NoProxy);
        if (!listen(QHostAddress::LocalHost) || !relay.bind(QHostAddress(QHostAddress::LocalHost), 0) ||
            !dns.bind(QHostAddress(QHostAddress::LocalHost), 0)) qFatal("cannot start system acceptance peers");
        connect(&dns, &QUdpSocket::readyRead, this, [this] {
            while (dns.hasPendingDatagrams()) {
                QByteArray packet(int(dns.pendingDatagramSize()), 0);
                QHostAddress source;
                quint16 port = 0;
                const auto read = dns.readDatagram(packet.data(), packet.size(), &source, &port);
                if (read < 0) { dnsError = dns.errorString(); continue; }
                packet.resize(read);
                ++dnsRequests;
                const auto response = DnsAnswer(packet);
                if (response.isEmpty()) continue;
                dnsWritten = dns.writeDatagram(response, source, port);
                if (dnsWritten == response.size()) ++dnsReplies;
                else dnsError = dns.errorString();
            }
        });
        connect(&relay, &QUdpSocket::readyRead, this, [this] {
            while (relay.hasPendingDatagrams()) {
                QByteArray packet(int(relay.pendingDatagramSize()), 0);
                QHostAddress source;
                quint16 sourcePort = 0;
                ++udpPackets;
                relayRead = relay.readDatagram(packet.data(), packet.size(), &source, &sourcePort);
                relaySource = source.toString() + ":" + QString::number(sourcePort);
                if (relayRead < 0) { relayError = relay.errorString(); continue; }
                packet.resize(relayRead);
                const auto length = AddressLength(packet, 3);
                if (length < 0 || packet[0] != 0 || packet[1] != 0 || packet[2] != 0) { ++malformedPackets; continue; }
                const auto destination = ReadDestination(packet, 3);
                const int header = 3 + length;
                const auto payload = destination.port == dns.localPort() ? DnsAnswer(packet.mid(header)) : packet.mid(header);
                if (destination.port == dns.localPort() && payload.isEmpty()) continue;
                udpDestinations.append(destination);
                ++udpRequests;
                const auto response = packet.left(header) + payload;
                relayWritten = relay.writeDatagram(response, source, sourcePort);
                if (relayWritten == response.size()) ++udpReplies;
                else relayError = relay.errorString();
            }
        });
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto socket = nextPendingConnection();
                socket->setProxy(QNetworkProxy::NoProxy);
                ++controlsAccepted;
                ++controlsActive;
                auto buffer = std::make_shared<QByteArray>();
                auto stage = std::make_shared<int>(0);
                auto destination = std::make_shared<Destination>();
                auto associated = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, this, [this, socket, associated] {
                    --controlsActive;
                    if (*associated) --associationsActive;
                    socket->deleteLater();
                });
                connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
                    if (error != QAbstractSocket::RemoteHostClosedError) controlError = socket->errorString();
                });
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, stage, destination, associated] {
                    *buffer += socket->readAll();
                    if (*stage == 0) {
                        if (buffer->size() < 2 || buffer->size() < 2 + quint8((*buffer)[1])) return;
                        const int size = 2 + quint8((*buffer)[1]);
                        if ((*buffer)[0] != 5 || !buffer->mid(2, size - 2).contains(char(0))) {
                            controlError = "invalid SOCKS greeting";
                            socket->abort();
                            return;
                        }
                        buffer->remove(0, size);
                        socket->write(QByteArray::fromHex("0500"));
                        ++greetings;
                        *stage = 1;
                    }
                    if (*stage == 1) {
                        const int length = AddressLength(*buffer, 3);
                        if (length < 0) return;
                        const auto command = quint8((*buffer)[1]);
                        if ((*buffer)[0] != 5 || (*buffer)[2] != 0 || (command != 1 && command != 3)) {
                            controlError = "invalid SOCKS command";
                            socket->abort();
                            return;
                        }
                        *destination = ReadDestination(*buffer, 3, command);
                        controlDestinations.append(*destination);
                        buffer->remove(0, 3 + length);
                        auto response = QByteArray::fromHex("050000017f000001");
                        const auto port = command == 3 ? relay.localPort() : serverPort();
                        response += char(port >> 8);
                        response += char(port & 255);
                        socket->write(response);
                        if (command == 3) {
                            *associated = true;
                            ++associations;
                            ++associationsActive;
                        }
                        *stage = command == 3 ? 3 : 2;
                    }
                    if (*stage == 2 && buffer->contains("\r\n\r\n")) {
                        ++tcpRequests;
                        tcpDestinations.append(*destination);
                        socket->write("HTTP/1.1 204 Acceptance\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        buffer->clear();
                    }
                });
            }
        });
    }

    ~SocksPeer() override {
        // Stop callbacks before the members captured by them are destroyed.
        for (auto socket : findChildren<QTcpSocket *>()) {
            QObject::disconnect(socket, nullptr, nullptr, nullptr);
            socket->abort();
        }
    }

    QString Diagnostics() const {
        return QStringList{
            QString("peer=%1:%2 relay=%3:%4 dns=%5:%6").arg(serverAddress().toString()).arg(serverPort())
                .arg(relay.localAddress().toString()).arg(relay.localPort()).arg(dns.localAddress().toString()).arg(dns.localPort()),
            QString("controls=%1 active=%2 greetings=%3 udp-associations=%4 active=%5 tcp-http=%6")
                .arg(controlsAccepted).arg(controlsActive).arg(greetings).arg(associations).arg(associationsActive).arg(tcpRequests),
            QString("udp-packets=%1 valid=%2 replies=%3 malformed=%4 read=%5 write=%6 source=%7")
                .arg(udpPackets).arg(udpRequests).arg(udpReplies).arg(malformedPackets).arg(relayRead).arg(relayWritten).arg(relaySource),
            QString("dns-requests=%1 replies=%2 write=%3").arg(dnsRequests).arg(dnsReplies).arg(dnsWritten),
            "last-control=" + (controlDestinations.isEmpty() ? "none" : controlDestinations.last().Description()),
            "last-udp=" + (udpDestinations.isEmpty() ? "none" : udpDestinations.last().Description()),
            "errors control=" + controlError + " relay=" + relayError + " dns=" + dnsError,
        }.join(" | ");
    }
};

inline bool HttpViaTun(const QString &address, int timeout = 5000) {
    QTcpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(address, 80);
    if (!QTest::qWaitFor([&] { return socket.state() == QAbstractSocket::ConnectedState ||
                                      socket.state() == QAbstractSocket::UnconnectedState; }, timeout) ||
        socket.state() != QAbstractSocket::ConnectedState) return false;
    socket.write("GET /acceptance HTTP/1.1\r\nHost: acceptance.test\r\nConnection: close\r\n\r\n");
    QByteArray response;
    const bool received = QTest::qWaitFor([&] { response += socket.readAll(); return response.contains("\r\n\r\n"); }, timeout);
    return received && response.startsWith("HTTP/1.1 204");
}

inline QByteArray UdpViaTun(const QHostAddress &destination, quint16 port, const QByteArray &payload, const SocksPeer *peer = nullptr) {
    QUdpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    qint64 written = -1;
    QHostAddress source;
    quint16 sourcePort = 0;
    QString phase = "bind";
    auto diagnostic = qScopeGuard([&] {
        qInfo().noquote() << QString("UDP probe destination=%1:%2 local=%3:%4 write=%5/%6 source=%7:%8 phase=%9 socket-error=%10 (%11)")
            .arg(destination.toString()).arg(port).arg(socket.localAddress().toString()).arg(socket.localPort())
            .arg(written).arg(payload.size()).arg(source.toString()).arg(sourcePort).arg(phase).arg(socket.error()).arg(socket.errorString());
        if (peer) qInfo().noquote() << peer->Diagnostics();
    });
    if (!socket.bind(QHostAddress(destination.protocol() == QAbstractSocket::IPv6Protocol ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4), 0)) return {};
    phase = "write";
    written = socket.writeDatagram(payload, destination, port);
    if (written != payload.size()) return {};
    phase = "timeout";
    if (!QTest::qWaitFor([&] { return socket.hasPendingDatagrams(); }, 5000)) return {};
    QByteArray received(int(socket.pendingDatagramSize()), 0);
    const auto read = socket.readDatagram(received.data(), received.size(), &source, &sourcePort);
    if (read < 0) { phase = "read-error"; return {}; }
    if (source != destination || sourcePort != port) { phase = "unexpected-response-source"; return {}; }
    received.resize(read);
    phase = "received";
    return received;
}

// No core or TUN is started. Every actual TCP/UDP packet stays on loopback;
// reserved destinations appear only inside the SOCKS5 wire-format payload.
inline QString SocksUdpFixtureSanity() {
    SocksPeer peer;
    const auto fail = [&](const QString &message) { return message + " | " + peer.Diagnostics(); };
    if (peer.proxy().type() != QNetworkProxy::NoProxy || peer.relay.proxy().type() != QNetworkProxy::NoProxy ||
        peer.dns.proxy().type() != QNetworkProxy::NoProxy) return fail("fixture sockets inherited a proxy");
    QTcpSocket control;
    control.setProxy(QNetworkProxy::NoProxy);
    control.connectToHost(QHostAddress::LocalHost, peer.serverPort());
    if (!QTest::qWaitFor([&] { return control.state() == QAbstractSocket::ConnectedState; }, 2000)) return fail("control connect failed");
    QByteArray buffered;
    const auto readControl = [&](int size) {
        return QTest::qWaitFor([&] { buffered += control.readAll(); return buffered.size() >= size; }, 2000);
    };
    control.write(QByteArray::fromHex("050100"));
    if (!readControl(2) || buffered != QByteArray::fromHex("0500")) return fail("SOCKS greeting failed");
    buffered.clear();
    control.write(QByteArray::fromHex("05030001000000000000"));
    if (!readControl(10) || !buffered.startsWith(QByteArray::fromHex("05000001"))) return fail("UDP ASSOCIATE failed");
    const auto bound = ReadDestination(buffered, 3);
    if (!bound.Is("127.0.0.1", peer.relay.localPort()) || peer.associationsActive != 1) return fail("wrong relay address or missing active association");
    QUdpSocket client;
    client.setProxy(QNetworkProxy::NoProxy);
    if (!client.bind(QHostAddress(QHostAddress::LocalHost), 0)) return fail("UDP client bind failed");
    const QList<QByteArray> headers{
        QByteArray::fromHex("00000001cb0071214a92"),
        QByteArray::fromHex("0000000420010db80000000000000000000000334a92"),
        QByteArray::fromHex("0000000317") + QByteArray("acceptance.example.test") + QByteArray::fromHex("4a92"),
    };
    const QStringList expected{"203.0.113.33", "2001:db8::33", "acceptance.example.test"};
    const auto exchange = [&](const QByteArray &packet, quint16 relayPort, QByteArray &received) {
        if (client.writeDatagram(packet, QHostAddress::LocalHost, relayPort) != packet.size()) return false;
        if (!QTest::qWaitFor([&] { return client.hasPendingDatagrams(); }, 2000)) return false;
        received.resize(int(client.pendingDatagramSize()));
        QHostAddress source;
        quint16 sourcePort = 0;
        const auto read = client.readDatagram(received.data(), received.size(), &source, &sourcePort);
        return read == received.size() && source == QHostAddress::LocalHost && sourcePort == relayPort;
    };
    for (int index = 0; index < headers.size(); ++index) {
        const auto packet = headers[index] + "synthetic-udp-" + QByteArray::number(index);
        QByteArray response;
        if (!exchange(packet, bound.port, response) || response != packet) return fail("SOCKS UDP address header or payload changed");
        if (peer.udpDestinations.size() != index + 1 || !peer.udpDestinations.last().Is(expected[index], 19090))
            return fail("SOCKS UDP destination capture is incorrect");
    }
    QByteArray dnsResponse;
    if (!exchange(DnsQuery(1), peer.dns.localPort(), dnsResponse) || dnsResponse != DnsAnswer(DnsQuery(1)))
        return fail("loopback DNS reply failed");
    auto dnsHeader = QByteArray::fromHex("000000017f000001");
    dnsHeader += char(peer.dns.localPort() >> 8);
    dnsHeader += char(peer.dns.localPort() & 255);
    if (!exchange(dnsHeader + DnsQuery(28), bound.port, dnsResponse) || dnsResponse != dnsHeader + DnsAnswer(DnsQuery(28)))
        return fail("DNS-over-SOCKS UDP reply failed");
    if (peer.udpRequests != 4 || peer.udpReplies != 4 || peer.dnsRequests != 1 || peer.dnsReplies != 1)
        return fail("fixture packet accounting is incomplete");
    control.disconnectFromHost();
    if (!QTest::qWaitFor([&] { return peer.controlsActive == 0 && peer.associationsActive == 0; }, 2000))
        return fail("fixture control connection did not close");
    qInfo().noquote() << "PASS: loopback SOCKS IPv4/IPv6/domain UDP headers, DNS, control lifetime" << peer.Diagnostics();
    return {};
}

} // namespace SystemModeAcceptance
