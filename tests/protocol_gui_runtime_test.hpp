#pragma once

// Included after GuiRegressionTest. The companion Go fixture owns the real
// protocol servers; this suite owns only its temporary profiles, local core
// instances and HTTP/UDP destinations. No TUN or system proxy is enabled.
#include <QDateTime>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUuid>

#include "db/CoreCapabilities.hpp"
#include "fmt/AnyTLSBean.hpp"
#include "fmt/QUICBean.hpp"
#include "fmt/SnellBean.hpp"
#include "sub/GroupUpdater.hpp"
#include "tests/subscription_metadata_test.hpp"
#include "tests/system_mode_fixture.hpp"
#include "ui/edit/dialog_edit_profile.h"

namespace ProtocolGuiRuntimeAcceptance {
    inline QByteArray Address(quint16 port) {
        auto address = QByteArray::fromHex("017f000001");
        address += char(port >> 8);
        address += char(port & 255);
        return address;
    }

    inline QByteArray HttpResponse(const QByteArray &path) {
        if (path.startsWith("/url/"))
            return "HTTP/1.1 204 Protocol fixture\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        const auto body = QByteArray("nekoray-protocol-response:") + path;
        return "HTTP/1.1 200 Protocol fixture\r\nContent-Length: " + QByteArray::number(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    }

    struct Targets {
        QTcpServer http;
        QUdpSocket udp;
        QHash<QByteArray, int> httpRequests;
        QList<QByteArray> udpPayloads;
        int malformedRequests = 0;
        QString udpError;

        Targets() {
            http.setProxy(QNetworkProxy::NoProxy);
            udp.setProxy(QNetworkProxy::NoProxy);
            if (!http.listen(QHostAddress::LocalHost, 0) || !udp.bind(QHostAddress(QHostAddress::LocalHost), 0))
                qFatal("cannot create protocol acceptance destinations on loopback");
            QObject::connect(&http, &QTcpServer::newConnection, &http, [this] {
                while (http.hasPendingConnections()) {
                    auto socket = http.nextPendingConnection();
                    socket->setProxy(QNetworkProxy::NoProxy);
                    auto buffered = std::make_shared<QByteArray>();
                    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    QObject::connect(socket, &QTcpSocket::readyRead, &http, [this, socket, buffered] {
                        *buffered += socket->readAll();
                        if (buffered->size() > 65536) {
                            ++malformedRequests;
                            socket->abort();
                            return;
                        }
                        if (!buffered->contains("\r\n\r\n")) return;
                        const auto words = buffered->left(buffered->indexOf("\r\n")).split(' ');
                        if (words.size() != 3 || words[0] != "GET" ||
                            (!words[1].startsWith("/body/") && !words[1].startsWith("/url/"))) {
                            ++malformedRequests;
                            socket->abort();
                            return;
                        }
                        ++httpRequests[words[1]];
                        socket->write(HttpResponse(words[1]));
                        socket->disconnectFromHost();
                        buffered->clear();
                    });
                }
            });
            QObject::connect(&udp, &QUdpSocket::readyRead, &udp, [this] {
                while (udp.hasPendingDatagrams()) {
                    QByteArray payload(int(udp.pendingDatagramSize()), 0);
                    QHostAddress source;
                    quint16 sourcePort = 0;
                    const auto read = udp.readDatagram(payload.data(), payload.size(), &source, &sourcePort);
                    if (read < 0) { udpError = udp.errorString(); continue; }
                    payload.resize(read);
                    if (!source.isLoopback()) { udpError = "non-loopback UDP source"; continue; }
                    udpPayloads.append(payload);
                    if (udp.writeDatagram(payload, source, sourcePort) != payload.size()) udpError = udp.errorString();
                }
            });
        }

        ~Targets() {
            // Socket callbacks capture these counters, so disconnect before
            // member destruction can emit any final disconnected signals.
            for (auto socket : http.findChildren<QTcpSocket *>()) {
                QObject::disconnect(socket, nullptr, nullptr, nullptr);
                socket->abort();
            }
        }
    };

    class SocksConnection {
    public:
        QTcpSocket socket;
        QByteArray buffered;
        SystemModeAcceptance::Destination bound;
        QString error;
        QDeadlineTimer deadline{5000};

        int Remaining() const { return int(qMax<qint64>(1, deadline.remainingTime())); }

        bool Read(int minimum) {
            const auto enough = QTest::qWaitFor([&] {
                buffered += socket.readAll();
                return buffered.size() >= minimum || socket.state() == QAbstractSocket::UnconnectedState;
            }, Remaining());
            return enough && buffered.size() >= minimum;
        }

        bool Open(quint16 socksPort, quint8 command, quint16 destinationPort = 0) {
            socket.setProxy(QNetworkProxy::NoProxy);
            socket.connectToHost(QHostAddress::LocalHost, socksPort);
            if (!QTest::qWaitFor([&] {
                    return socket.state() == QAbstractSocket::ConnectedState ||
                           socket.state() == QAbstractSocket::UnconnectedState;
                }, Remaining()) || socket.state() != QAbstractSocket::ConnectedState) {
                error = "SOCKS connect: " + socket.errorString();
                return false;
            }
            socket.write(QByteArray::fromHex("050100"));
            if (!Read(2) || buffered.left(2) != QByteArray::fromHex("0500")) {
                error = "SOCKS no-auth greeting failed";
                return false;
            }
            buffered.remove(0, 2);
            QByteArray request;
            request += char(5);
            request += char(command);
            request += char(0);
            request += command == 3 ? QByteArray::fromHex("01000000000000") : Address(destinationPort);
            socket.write(request);
            if (!Read(4) || buffered[0] != 5 || buffered[1] != 0 || buffered[2] != 0) {
                error = QString("SOCKS command %1 rejected or timed out").arg(command);
                return false;
            }
            const auto complete = QTest::qWaitFor([&] {
                buffered += socket.readAll();
                return SystemModeAcceptance::AddressLength(buffered, 3) > 0 ||
                       socket.state() == QAbstractSocket::UnconnectedState;
            }, Remaining());
            const int length = SystemModeAcceptance::AddressLength(buffered, 3);
            if (!complete || length < 0) {
                error = "incomplete SOCKS reply address";
                return false;
            }
            bound = SystemModeAcceptance::ReadDestination(buffered, 3, command);
            buffered.remove(0, 3 + length);
            return true;
        }
    };

    inline QString HttpViaSocks(quint16 socksPort, quint16 destinationPort, const QByteArray &path) {
        SocksConnection connection;
        if (!connection.Open(socksPort, 1, destinationPort)) return connection.error;
        const auto request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (connection.socket.write(request) != request.size()) return "cannot write HTTP request through SOCKS";
        const auto expected = HttpResponse(path);
        if (!connection.Read(expected.size())) return "incomplete HTTP response through the selected protocol";
        if (connection.buffered != expected) return "HTTP status/body changed through the selected protocol";
        return {};
    }

    inline QString UdpViaSocks(quint16 socksPort, quint16 destinationPort, const QByteArray &payload) {
        SocksConnection control;
        if (!control.Open(socksPort, 3)) return control.error;
        QHostAddress relay(control.bound.host);
        if (relay == QHostAddress::Any || relay == QHostAddress::AnyIPv4 || relay == QHostAddress::AnyIPv6)
            relay = control.socket.peerAddress();
        if (!relay.isLoopback() || control.bound.port == 0) return "SOCKS advertised a non-loopback or invalid UDP relay";
        QUdpSocket client;
        client.setProxy(QNetworkProxy::NoProxy);
        const auto local = relay.protocol() == QAbstractSocket::IPv6Protocol ? QHostAddress::LocalHostIPv6 : QHostAddress::LocalHost;
        if (!client.bind(QHostAddress(local), 0)) return "UDP client bind: " + client.errorString();
        const auto packet = QByteArray::fromHex("000000") + Address(destinationPort) + payload;
        if (client.writeDatagram(packet, relay, control.bound.port) != packet.size()) return "UDP relay write: " + client.errorString();
        if (!QTest::qWaitFor([&] { return client.hasPendingDatagrams(); }, control.Remaining()))
            return "UDP response timed out through the selected protocol";
        QByteArray received(int(client.pendingDatagramSize()), 0);
        QHostAddress source;
        quint16 sourcePort = 0;
        const auto read = client.readDatagram(received.data(), received.size(), &source, &sourcePort);
        if (read < 0) return "UDP relay read: " + client.errorString();
        received.resize(read);
        if (source != relay || sourcePort != control.bound.port) return "UDP response came from a different relay";
        const int length = SystemModeAcceptance::AddressLength(received, 3);
        if (length < 0 || received.left(3) != QByteArray::fromHex("000000")) return "invalid SOCKS UDP response header";
        if (!SystemModeAcceptance::ReadDestination(received, 3).Is("127.0.0.1", destinationPort))
            return "SOCKS UDP response reported a different destination";
        if (received.mid(3 + length) != payload) return "UDP payload changed through the selected protocol";
        if (control.socket.state() != QAbstractSocket::ConnectedState) return "UDP association control connection closed early";
        return {};
    }

    inline std::shared_ptr<NekoGui::ProxyEntity> NodeFromManifest(const QJsonObject &peer, QString &error) {
        const auto type = peer["type"].toString();
        auto node = NekoGui::ProfileManager::NewProxyEntity(type);
        if (!node || !node->bean || node->bean->version == -114514) {
            error = "protocol factory is not registered";
            return {};
        }
        node->bean->name = peer["name"].toString();
        node->bean->serverAddress = peer["server"].toString();
        node->bean->serverPort = peer["port"].toInt();
        if (auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(node->bean.get())) {
            bean->password = peer["password"].toString();
            bean->sni = peer["sni"].toString();
            bean->caText = peer["caText"].toString();
            bean->allowInsecure = false;
            bean->idleSessionCheckInterval = peer["idleSessionCheckInterval"].toString();
            bean->idleSessionTimeout = peer["idleSessionTimeout"].toString();
            bean->minIdleSession = peer["minIdleSession"].toInt();
            bean->clientMetadata = peer["clientMetadata"].toString();
            error = bean->Validate();
        } else if (auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(node->bean.get())) {
            bean->snellVersion = peer["version"].toInt();
            bean->psk = peer["psk"].toString();
            bean->userkey = peer["userkey"].toString();
            bean->reuse = peer["reuse"].toBool();
            bean->obfsMode = peer["obfsMode"].toString();
            bean->obfsHost = peer["obfsHost"].toString();
            bean->mode = peer["mode"].toString();
            error = bean->Validate();
        } else if (auto bean = dynamic_cast<NekoGui_fmt::QUICBean *>(node->bean.get()); bean && type == "hysteria2") {
            bean->forceExternal = false;
            bean->password = peer["password"].toString();
            bean->sni = peer["sni"].toString();
            bean->caText = peer["caText"].toString();
            bean->allowInsecure = false;
            bean->uploadMbps = 0;
            bean->downloadMbps = 0;
            bean->obfsType = peer["obfsType"].toString();
            bean->obfsPassword = peer["obfsPassword"].toString();
            bean->bbrProfile = peer["bbrProfile"].toString();
            bean->disableChromeParrot = peer["disableChromeParrot"].toBool();
            bean->geckoMinPacketSize = peer["geckoMinPacketSize"].toInt(512);
            bean->geckoMaxPacketSize = peer["geckoMaxPacketSize"].toInt(1200);
            QStringList ports;
            for (const auto &port : peer["serverPorts"].toArray()) ports.append(port.toString());
            bean->hopPort = ports.join(',');
            bean->hopIntervalMin = peer["hopInterval"].toString();
            bean->hopIntervalMax = peer["hopIntervalMax"].toString();
            error = bean->Validate();
        } else error = "unexpected protocol in fixture manifest";
        return error.isEmpty() ? node : nullptr;
    }
} // namespace ProtocolGuiRuntimeAcceptance

void GuiRegressionTest::newProtocolPeerRuntimeAcceptance() {
    using namespace ProtocolGuiRuntimeAcceptance;
    using namespace NekoGui;
    const auto manifestPath = qEnvironmentVariable("NEKORAY_PROTOCOL_GUI_FIXTURE");
    if (manifestPath.isEmpty()) QSKIP("opt-in protocol interoperability; start TestProtocolGuiPeers and set its fresh manifest path");
    const QFileInfo manifestInfo(manifestPath);
    QVERIFY2(manifestInfo.isAbsolute() && manifestInfo.isFile(), "protocol fixture requires an existing absolute manifest path");
    QVERIFY2(!QFileInfo::exists(manifestPath + ".stop"), "protocol fixture has already received its stop signal");
    QVERIFY(manifestInfo.size() > 0 && manifestInfo.size() <= 1024 * 1024);
    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const auto manifestDocument = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    manifestFile.close();
    QVERIFY2(parseError.error == QJsonParseError::NoError && manifestDocument.isObject(), "protocol fixture manifest is not a JSON object");
    const auto manifest = manifestDocument.object();
    QCOMPARE(manifest["schema"].toInt(-1), 1);
    const auto expires = QDateTime::fromString(manifest["expiresAt"].toString(), Qt::ISODate).toUTC();
    const auto now = QDateTime::currentDateTimeUtc();
    QVERIFY2(expires.isValid() && now.secsTo(expires) > 45 && now.secsTo(expires) <= 240,
             "protocol fixture manifest is expired or not fresh; start a new Go fixture");
    QVERIFY2(manifestInfo.lastModified().toUTC().secsTo(now) >= -5 &&
                 manifestInfo.lastModified().toUTC().secsTo(now) <= 210, "protocol fixture manifest file is stale");
    const auto peers = manifest["peers"].toArray();
    QCOMPARE(peers.size(), 7);
    const QMap<QString, QString> expectedTypes{
        {"anytls", "anytls"}, {"snell4", "snell"}, {"snell4-http", "snell"}, {"snell4-tls", "snell"},
        {"snell6", "snell"}, {"hysteria2-salamander", "hysteria2"}, {"hysteria2-gecko", "hysteria2"},
    };
    QSet<QString> seen;
    for (const auto &entry : peers) {
        QVERIFY(entry.isObject());
        const auto peer = entry.toObject();
        const auto name = peer["name"].toString();
        QVERIFY(expectedTypes.contains(name) && !seen.contains(name));
        seen.insert(name);
        QCOMPARE(peer["type"].toString(), expectedTypes[name]);
        QCOMPARE(peer["server"].toString(), QString("127.0.0.1"));
        QVERIFY(peer["port"].isDouble() && peer["port"].toInt() > 0 && peer["port"].toInt() <= 65535 &&
                peer["port"].toDouble() == peer["port"].toInt());
        if (peer["type"] == "snell") {
            QCOMPARE(peer["version"].toInt(), name == "snell6" ? 6 : 4);
            QCOMPARE(peer["serverVersion"].toInt(), name == "snell6" ? 6 : 5);
        } else {
            QCOMPARE(peer["sni"].toString(), QString("localhost"));
            QVERIFY(!peer["caText"].toString().isEmpty());
        }
    }
    QVERIFY2(!dataStore->spmode_vpn && !dataStore->spmode_system_proxy, "protocol-only acceptance requires system modes to remain off");
    QVERIFY2(stopActiveRuntime(), "cannot prepare a clean test runtime");
    QVERIFY(!dataStore->inbound_auth->NeedAuth());
    SubscriptionMetadataRegression::GroupFixture drafts;
    Targets targets;
    const auto runId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
    const auto urlPath = QByteArray("/url/") + runId;
    QScopedValueRollback<QString> testUrl(dataStore->test_latency_url,
        QString("http://127.0.0.1:%1%2").arg(targets.http.serverPort()).arg(QString::fromLatin1(urlPath)));
    QScopedValueRollback<int> testTimeout(dataStore->test_latency_timeout, 5000);
    QScopedValueRollback<int> testConcurrency(dataStore->test_concurrent, 4);
    QScopedValueRollback<bool> skipCertificate(dataStore->skip_cert, false);
    QScopedValueRollback<QString> rawRoutes(dataStore->custom_route_global, "{\"rules\": []}");
    QScopedValueRollback<QString> rawInbounds(dataStore->custom_inbound, "{\"inbounds\": []}");
    QScopedValueRollback<QString> underlyingDns(dataStore->core_box_underlying_dns, "udp://127.0.0.1:1");
    auto oldRouting = std::move(dataStore->routing);
    dataStore->routing = std::make_unique<Routing>();
    dataStore->routing->save_control_no_save = true;
    dataStore->routing->proxy_ip = "127.0.0.0/8";
    dataStore->routing->def_outbound = "proxy";
    dataStore->routing->sniffing_mode = SniffingMode::DISABLE;
    dataStore->routing->remote_dns = "udp://127.0.0.1:1";
    dataStore->routing->direct_dns = "udp://127.0.0.1:1";
    auto restore = qScopeGuard([&] {
        if (!stopActiveRuntime()) qWarning("protocol acceptance runtime did not stop during cleanup");
        dataStore->routing = std::move(oldRouting);
        window->show_group(baseGroup);
    });

    QList<std::shared_ptr<ProxyEntity>> nodes;
    QMap<QString, std::shared_ptr<ProxyEntity>> byName;
    for (const auto &entry : peers) {
        const auto peer = entry.toObject();
        const auto name = peer["name"].toString();
        QString error;
        auto original = NodeFromManifest(peer, error);
        QVERIFY2(original != nullptr, qPrintable(name + ": " + error));
        QVERIFY(profileManager->AddProfile(original, drafts.group->id));
        const auto originalProtocol = original->bean->ToJson({"name"});
        {
            DialogEditProfile editor("", original->id, window);
            editor.show();
            child<QLineEdit>(&editor, "name")->setText("GUI saved " + name);
            ModalGuard modal;
            editor.accept();
            QCOMPARE(editor.result(), int(QDialog::Accepted));
        }
        QVERIFY2(original->bean->ToJson({"name"}) == originalProtocol,
                 qPrintable(name + ": editing/saving changed protocol options"));
        QCOMPARE(original->bean->name, "GUI saved " + name);
        NekoGui_sub::RawUpdater imported;
        imported.stage_only = true;
        imported.gid_add_to = baseGroup;
        imported.update(original->bean->ToShareLink());
        QCOMPARE(imported.rejected_count, 0);
        QCOMPARE(imported.updated_order.size(), 1);
        auto node = imported.updated_order.first();
        QVERIFY2(node->bean->ToJson() == original->bean->ToJson(), qPrintable(name + ": sharing/importing changed protocol options"));
        QCOMPARE(node->core_preference, 0);
        QVERIFY(profileManager->AddProfile(node, baseGroup));
        QCOMPARE(ResolveCore(node, CoreType::V2RAY).targetCore, int(CoreType::SING_BOX));
        nodes.append(node);
        byName.insert(name, node);
    }
    window->refresh_groups();
    for (int index = 0; index < nodes.size(); ++index) {
        const auto &node = nodes[index];
        const auto peer = peers[index].toObject();
        const auto name = peer["name"].toString();
        QVERIFY2(QDateTime::currentDateTimeUtc().secsTo(expires) > 10, "protocol fixture expired during GUI acceptance");
        window->neko_start(node->id);
        QVERIFY2(QTest::qWaitFor([&] {
                    return !window->core_switch_busy && window->core_session && window->core_session->profile == node;
                }, 12000), qPrintable(name + ": core start failed: " + window->core_notice));
        QCOMPARE(window->active_core(), int(CoreType::SING_BOX));
        QVERIFY(window->core_session->built->extRs.empty());
        QVERIFY(!dataStore->spmode_vpn && !dataStore->spmode_system_proxy);
        const auto config = window->core_session->built->coreConfig;
        for (const auto &inbound : config["inbounds"].toArray()) QVERIFY(inbound.toObject()["type"] != "tun");
        QJsonObject outbound;
        for (const auto &value : config["outbounds"].toArray()) {
            if (value.toObject()["tag"] == "proxy") outbound = value.toObject();
        }
        QCOMPARE(outbound["type"].toString(), peer["type"].toString());
        QCOMPARE(outbound["server"].toString(), QString("127.0.0.1"));
        QCOMPARE(config["route"].toObject()["final"].toString(), QString("proxy"));
        bool routesLoopbackThroughProxy = false;
        for (const auto &rule : config["route"].toObject()["rules"].toArray()) {
            const auto object = rule.toObject();
            routesLoopbackThroughProxy |= object["outbound"] == "proxy" &&
                object["ip_cidr"].toArray().contains("127.0.0.0/8");
        }
        QVERIFY(routesLoopbackThroughProxy);
        const auto path = QByteArray("/body/") + runId + '/' + name.toLatin1();
        auto error = HttpViaSocks(dataStore->inbound_socks_port, targets.http.serverPort(), path);
        QVERIFY2(error.isEmpty(), qPrintable(name + ": " + error));
        QCOMPARE(targets.httpRequests.value(path), 1);
        const int payloadSize = peer["type"] == "snell" && peer["version"].toInt() == 4 ? 1200
                                : peer["type"] == "hysteria2" ? 4096 : 8192;
        QByteArray payload = "nekoray-udp-" + runId + '-' + name.toLatin1() + ':';
        while (payload.size() < payloadSize) payload += char(payload.size() % 251);
        const int before = targets.udpPayloads.size();
        error = UdpViaSocks(dataStore->inbound_socks_port, targets.udp.localPort(), payload);
        QVERIFY2(error.isEmpty(), qPrintable(name + ": " + error));
        QCOMPARE(targets.udpPayloads.size(), before + 1);
        QCOMPARE(targets.udpPayloads.last(), payload);
        QVERIFY2(targets.udpError.isEmpty(), qPrintable(targets.udpError));
        qInfo("PASS GUI protocol peer %s: edit/share/import, SOCKS TCP and %d-byte UDP", qPrintable(name), payloadSize);
    }

    auto udpOnly = ProfileManager::NewProxyEntity("snell");
    udpOnly->bean->FromJson(byName["snell6"]->bean->ToJson());
    dynamic_cast<NekoGui_fmt::SnellBean *>(udpOnly->bean.get())->network = "udp";
    udpOnly->bean->name = "UDP-only Snell URL exclusion";
    QVERIFY(profileManager->AddProfile(udpOnly, baseGroup));
    QList<std::shared_ptr<ProxyEntity>> wrongSecrets;
    for (const auto &name : {QString("anytls"), QString("snell6"), QString("hysteria2-salamander")}) {
        const auto &valid = byName[name];
        auto invalid = ProfileManager::NewProxyEntity(valid->type);
        invalid->bean->FromJson(valid->bean->ToJson());
        invalid->bean->name = "Wrong synthetic credential " + name;
        if (auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(invalid->bean.get())) bean->password.prepend("incorrect-");
        if (auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(invalid->bean.get())) bean->psk.prepend("incorrect-");
        if (auto bean = dynamic_cast<NekoGui_fmt::QUICBean *>(invalid->bean.get())) bean->password.prepend("incorrect-");
        QVERIFY(profileManager->AddProfile(invalid, baseGroup));
        wrongSecrets.append(invalid);
    }
    // URL probes use the default outbound directly. Also exercise ordinary
    // SOCKS routing with invalid credentials, so an accidental direct rule
    // cannot make the successful protocol checks above pass undetected.
    const auto validProfile = window->core_session->profile;
    for (const auto &node : wrongSecrets) {
        window->neko_start(node->id);
        QVERIFY2(QTest::qWaitFor([&] {
                    return !window->core_switch_busy && window->core_session && window->core_session->profile == node;
                }, 12000), "cannot start the synthetic negative-control configuration");
        const auto path = QByteArray("/body/") + runId + "/reject-" + node->type.toLatin1();
        const auto error = HttpViaSocks(dataStore->inbound_socks_port, targets.http.serverPort(), path);
        QVERIFY2(!error.isEmpty(), "wrong protocol credentials unexpectedly reached the HTTP target");
        QCOMPARE(targets.httpRequests.value(path), 0);
        if (node->type == "snell") {
            const int before = targets.udpPayloads.size();
            const auto udpError = UdpViaSocks(dataStore->inbound_socks_port, targets.udp.localPort(),
                                              "reject-wrong-snell-credential-" + runId);
            QVERIFY2(!udpError.isEmpty(), "wrong Snell credentials unexpectedly reached the UDP target");
            QCOMPARE(targets.udpPayloads.size(), before);
        }
    }
    window->neko_start(validProfile->id);
    QVERIFY2(QTest::qWaitFor([&] {
                return !window->core_switch_busy && window->core_session && window->core_session->profile == validProfile;
            }, 12000), "cannot restore the valid node after protocol negative controls");
    const auto recoveryPath = QByteArray("/body/") + runId + "/recovered";
    const auto recoveryError = HttpViaSocks(dataStore->inbound_socks_port, targets.http.serverPort(), recoveryPath);
    QVERIFY2(recoveryError.isEmpty(), qPrintable(recoveryError));
    QCOMPARE(targets.httpRequests.value(recoveryPath), 1);
    qInfo("PASS GUI protocol negative controls: wrong credentials block SOCKS TCP for all three protocols and Snell UDP; valid node recovered");
    const auto running = window->core_session;
    QVERIFY2(QDateTime::currentDateTimeUtc().secsTo(expires) > 15, "restart the fixture before running the URL matrix");
    startBatch();
    QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch, 25000);
    QCOMPARE(window->core_session, running);
    for (const auto &node : nodes) {
        const auto result = node->LastTestResult(CoreType::SING_BOX);
        QCOMPARE(result.status, TestResultStatus::Success);
        QVERIFY(result.error.isEmpty());
        QVERIFY(result.latency_ms > 0);
        QVERIFY(result.timestamp_ms > 0);
        QCOMPARE(result.build_id, window->core_manager->Identity(CoreType::SING_BOX));
        QVERIFY(node->HasValidTestResult(CoreType::SING_BOX, result.build_id, window->test_fingerprint(node, CoreType::SING_BOX)));
        QVERIFY(!node->IsRemovalCandidate(CoreType::SING_BOX, result.build_id, window->test_fingerprint(node, CoreType::SING_BOX)));
        QCOMPARE(node->core_preference, 0);
    }
    const auto unsupported = udpOnly->LastTestResult(CoreType::SING_BOX);
    QCOMPARE(unsupported.status, TestResultStatus::Unsupported);
    QVERIFY(unsupported.timestamp_ms > 0 && !unsupported.error.isEmpty());
    QVERIFY(!udpOnly->IsRemovalCandidate(CoreType::SING_BOX, unsupported.build_id,
                                           window->test_fingerprint(udpOnly, CoreType::SING_BOX)));
    for (const auto &node : wrongSecrets) {
        const auto result = node->LastTestResult(CoreType::SING_BOX);
        QCOMPARE(result.status, TestResultStatus::ConnectionFailure);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(node->IsRemovalCandidate(CoreType::SING_BOX, result.build_id,
                                            window->test_fingerprint(node, CoreType::SING_BOX)));
    }
    // startupRetryTransport may repeat a GET once after sing-box's initial
    // local interface callback resets a connection. The original timeout
    // still applies; exact request counts would make this acceptance flaky.
    const int urlRequests = targets.httpRequests.value(urlPath);
    QVERIFY2(urlRequests >= nodes.size() && urlRequests <= 2 * nodes.size(),
             "URL requests exceeded the successful nodes and their single local-startup retry budget");
    qInfo("GUI URL target received %d requests for %lld successful nodes (at most one local-startup retry each)",
          urlRequests, static_cast<long long>(nodes.size()));
    QCOMPARE(targets.malformedRequests, 0);
    QVERIFY(!dataStore->spmode_vpn && !dataStore->spmode_system_proxy);
    QVERIFY2(stopActiveRuntime(), "protocol acceptance runtime did not stop");
    qInfo("PASS GUI URL matrix: 7 successful protocols, UDP-only Snell unsupported, 3 remote credential failures; concurrency 4");
}
