#pragma once

// Included after GuiRegressionTest. These cases use the suite's temporary
// database, synthetic credentials, localhost resolution and owned loopback
// listeners. They do not change system proxy, certificate trust or TUN state.
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QUrl>

#include "db/CoreCapabilities.hpp"
#include "fmt/AnyTLSBean.hpp"
#include "fmt/QUICBean.hpp"
#include "fmt/SnellBean.hpp"
#include "tests/subscription_metadata_test.hpp"
#include "ui/edit/dialog_edit_profile.h"

namespace NewProtocolRegression {
    // Public self-signed fixture certificate; its private key was discarded.
    // This is only parsed/stored in an isolated profile, never trusted by the OS.
    inline QString Certificate() {
        return QStringLiteral(
            "-----BEGIN CERTIFICATE-----\n"
            "MIICqjCCAZKgAwIBAgIITLeneta4PTIwDQYJKoZIhvcNAQELBQAwFDESMBAGA1UE\n"
            "AxMJbG9jYWxob3N0MCAXDTIwMDEwMTAwMDAwMFoYDzIwOTkwMTAxMDAwMDAwWjAU\n"
            "MRIwEAYDVQQDEwlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK\n"
            "AoIBAQC/ZtXoSBFtoYGNbBof0YYbnXWr0K5/Pvk4iupuZ1yoEN/vG8inh8E/HLB9\n"
            "gNOCkbuFxkVsN7pMQ82IoSygf2zkzbMVPNlRCk+wLioVmF8CaMgUaMA1Ln629S1Y\n"
            "7F2X25zPylc1uqXZekmnyYFYtjSAaoKH7fXVqoz1uzuDTUx86HpcuXL/d4By1Qwv\n"
            "64K5FmvaZcyCXzfqRU/js1XhvWFi/RaIexuWvxeguqkypctHNT2jsotsbPA6WpNk\n"
            "434pSZD/7oewTNG1hayMEYPHjoACQLQDaWv0wvPZbScNy23qi9GQbPG4YqZgKdL5\n"
            "tpqjVHiYd7Xd7RBpPqH4AqTcD0AZAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABQ3\n"
            "mxM3GM5GWIKVoAP87tPU1yhIO4KNGdsdRsBH8cKXvBtoQFgJWCc7PQFPFpAJCRoy\n"
            "CdC64gQYEiOje2W/twGk/A47t5bcA2IL9CBZkyIeyj3PhL+UYCgccsA0DHTOd2Fu\n"
            "gPVlgNmFZ3d28qTu8zIX4YDFqF74UZ4Luse19HeXPyCg9zKOfTpIScFoIhgxjC6Q\n"
            "At2VZSkuKtZXqKDWMpwTDxZiQLihcOVx1DvvEL9KFdHFXbYIGMYx0xD7yLz7ULRR\n"
            "XkliiQ9WH+zY9c0GOU0OZkhGAveA9VarOqhIpVW5s+fJ1ayM9Rd9XohDPiEbjwVT\n"
            "mwz/w11vKztyglCAURc=\n"
            "-----END CERTIFICATE-----\n");
    }

    inline std::shared_ptr<NekoGui::ProxyEntity> AnyTls(bool advanced = false) {
        auto node = NekoGui::ProfileManager::NewProxyEntity("anytls");
        auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(node->bean.get());
        if (!bean) qFatal("AnyTLS factory is not registered");
        bean->name = "synthetic AnyTLS";
        bean->serverAddress = "127.0.0.1";
        bean->serverPort = 10443;
        bean->password = "synthetic:anytls@password";
        bean->sni = "localhost";
        if (advanced) {
            bean->caText = Certificate();
            bean->alpn = "h2,http/1.1";
            bean->utlsFingerprint = "chrome";
            bean->idleSessionCheckInterval = "17s";
            bean->idleSessionTimeout = "1m10s";
            bean->minIdleSession = 3;
            bean->clientMetadata = QString::fromUtf8("synthetic GUI 客户端");
        }
        return node;
    }

    inline std::shared_ptr<NekoGui::ProxyEntity> Snell(int version = 4, bool advanced = false) {
        auto node = NekoGui::ProfileManager::NewProxyEntity("snell");
        auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(node->bean.get());
        if (!bean) qFatal("Snell factory is not registered");
        bean->name = QString("synthetic Snell %1").arg(version);
        bean->serverAddress = "127.0.0.1";
        bean->serverPort = version == 4 ? 10444 : 10446;
        bean->snellVersion = version;
        bean->psk = "synthetic-snell-password";
        if (advanced) {
            bean->userkey = "synthetic-user";
            bean->reuse = true;
            bean->network = "tcp";
            if (version == 4) {
                bean->obfsMode = "tls";
                bean->obfsHost = "localhost";
            } else {
                bean->mode = "unshaped";
            }
        }
        return node;
    }

    inline QString FullLink(const QString &type, const QJsonObject &object) {
        QUrl link;
        link.setScheme("nekoray");
        link.setHost(type);
        link.setFragment(QString::fromLatin1(QJsonDocument(object).toJson(QJsonDocument::Compact)
                                                .toBase64(QByteArray::Base64UrlEncoding)));
        return link.toString();
    }

    inline QByteArray SavedFile(const std::shared_ptr<NekoGui::ProxyEntity> &node) {
        QFile file(node->fn);
        if (!file.open(QIODevice::ReadOnly)) qFatal("cannot read isolated profile fixture");
        return file.readAll();
    }

    struct RejectingPeer {
        QTcpServer server;
        int connections = 0;

        RejectingPeer() {
            server.setProxy(QNetworkProxy::NoProxy);
            if (!server.listen(QHostAddress::LocalHost, 0)) qFatal("cannot create rejecting loopback fixture");
            QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
                while (server.hasPendingConnections()) {
                    auto socket = server.nextPendingConnection();
                    ++connections;
                    socket->abort();
                    socket->deleteLater();
                }
            });
        }
    };
} // namespace NewProtocolRegression

void GuiRegressionTest::newProtocolsImportNativeAndFullLinks() {
    using namespace NewProtocolRegression;
    SubscriptionMetadataRegression::GroupFixture fixture;
    const auto native = fixture.stage(
        "anytls://synthetic%3Apass%40word@127.0.0.1:10443?sni=localhost&insecure=1#AnyTLS%20local\n"
        "snell://synthetic-snell-password@127.0.0.1:10444?version=4&obfs_mode=http&obfs_host=localhost#Snell%204\n"
        "snell://synthetic-snell-password@127.0.0.1:10446?version=6&reuse=1&network=tcp&userkey=local-user&mode=unshaped#Snell%206");
    QCOMPARE(native.rejected_count, 0);
    QCOMPARE(native.updated_order.size(), 3);
    auto anytls = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(native.updated_order[0]->bean.get());
    auto snell4 = dynamic_cast<NekoGui_fmt::SnellBean *>(native.updated_order[1]->bean.get());
    auto snell6 = dynamic_cast<NekoGui_fmt::SnellBean *>(native.updated_order[2]->bean.get());
    QVERIFY(anytls && snell4 && snell6);
    QCOMPARE(anytls->password, QString("synthetic:pass@word"));
    QCOMPARE(anytls->name, QString("AnyTLS local"));
    QCOMPARE(anytls->sni, QString("localhost"));
    QVERIFY(anytls->allowInsecure);
    QCOMPARE(snell4->snellVersion, 4);
    QCOMPARE(snell4->obfsMode, QString("http"));
    QCOMPARE(snell4->obfsHost, QString("localhost"));
    QCOMPARE(snell6->snellVersion, 6);
    QCOMPARE(snell6->mode, QString("unshaped"));
    QCOMPARE(snell6->network, QString("tcp"));
    QCOMPARE(snell6->userkey, QString("local-user"));
    QVERIFY(snell6->reuse);
    QVERIFY(fixture.group->Profiles().isEmpty());

    for (const auto &node : {AnyTls(true), Snell(4, true), Snell(6, true)}) {
        node->core_preference = 1;
        node->RecordTestResult(SubscriptionMetadataRegression::Result(NekoGui::TestResultStatus::Success, 1));
        const auto original = node->ToJson();
        const auto link = node->bean->ToShareLink();
        QVERIFY(link.startsWith("nekoray://" + node->type + "#"));
        const auto payload = QJsonDocument::fromJson(QByteArray::fromBase64(
            QUrl(link).fragment(QUrl::FullyDecoded).toLatin1(), QByteArray::Base64UrlEncoding)).object();
        QCOMPARE(payload, node->bean->ToJson());
        QVERIFY(!payload.contains("core_preference"));
        QVERIFY(!payload.contains("test_results"));
        const auto imported = fixture.stage(link);
        QCOMPARE(imported.rejected_count, 0);
        QCOMPARE(imported.updated_order.size(), 1);
        const auto copy = imported.updated_order.first();
        QCOMPARE(copy->type, node->type);
        QCOMPARE(copy->bean->ToJson(), node->bean->ToJson());
        QCOMPARE(copy->core_preference, 0);
        // Fresh entities carry the migration marker, but no latest result or
        // history. Require the exact empty-result schema so a leaked record
        // (or any other local metadata) still fails this sharing regression.
        QCOMPARE(copy->test_results, (QJsonObject{{"legacy_migrated", true}}));
        QCOMPARE(copy->latency, 0);
        QVERIFY(copy->full_test_report.isEmpty());
        QCOMPARE(copy->id, -1);
        QCOMPARE(node->ToJson(), original);
    }

    // Sharing a legacy inferred Salamander setting must preserve its identity,
    // so a later subscription refresh can still match local metadata.
    for (const auto &obfs : {QString{}, QString("salamander")}) {
        auto node = NekoGui::ProfileManager::NewProxyEntity("hysteria2");
        auto bean = node->QUICBean();
        bean->serverAddress = "127.0.0.1";
        bean->serverPort = 10448;
        bean->password = "synthetic-hysteria-password";
        bean->obfsPassword = "synthetic-salamander-password";
        bean->obfsType = obfs;
        const auto original = bean->ToJson();
        const auto imported = fixture.stage(bean->ToShareLink());
        QCOMPARE(imported.rejected_count, 0);
        QCOMPARE(imported.updated_order.size(), 1);
        QCOMPARE(imported.updated_order.first()->bean->ToJson(), original);
    }
}

void GuiRegressionTest::newProtocolsClashImportPreservesOptions() {
#ifdef NKR_NO_YAML
    QSKIP("This build intentionally excludes Clash/YAML import support");
#else
    using namespace NewProtocolRegression;
    using namespace NekoGui;
    SubscriptionMetadataRegression::GroupFixture fixture;
    QString yaml =
        "proxies:\n"
        "  - name: AnyTLS YAML fixture\n"
        "    type: anytls\n"
        "    server: 127.0.0.1\n"
        "    port: 10443\n"
        "    password: 'synthetic:yaml@password'\n"
        "    sni: localhost\n"
        "    skip-cert-verify: true\n"
        "    alpn: [h2, http/1.1]\n"
        "    client-fingerprint: chrome\n"
        "    idle-session-check-interval: 12\n"
        "    idle-session-timeout: 1m30s\n"
        "    min-idle-session: 2\n"
        "    ca-str: |\n";
    for (const auto &line : Certificate().trimmed().split('\n')) yaml += "      " + line + '\n';
    yaml +=
        "  - name: Snell 4 YAML fixture\n"
        "    type: snell\n"
        "    server: 127.0.0.1\n"
        "    port: 10444\n"
        "    version: 4\n"
        "    psk: synthetic-snell-four\n"
        "    udp: false\n"
        "    obfs-opts: {mode: http, host: localhost}\n"
        "  - name: Snell 6 YAML fixture\n"
        "    type: snell\n"
        "    server: 127.0.0.1\n"
        "    port: 10446\n"
        "    version: 6\n"
        "    psk: synthetic-snell-six\n"
        "    network: udp\n"
        "    udp: true\n"
        "    userkey: synthetic-user\n"
        "    reuse: true\n"
        "    mode: unshaped\n"
        "  - name: Hysteria2 YAML fixture\n"
        "    type: hysteria2\n"
        "    server: 127.0.0.1\n"
        "    port: 10448\n"
        "    password: synthetic-hysteria-password\n"
        "    sni: localhost\n"
        "    obfs: gecko\n"
        "    obfs-password: synthetic-gecko-password\n"
        "    bbr-profile: aggressive\n"
        "    gecko-min-packet-size: 600\n"
        "    gecko-max-packet-size: 1300\n"
        "    disable-chrome-parrot: true\n"
        "    server-ports: ['10448:10448', '10450:10451']\n"
        "    hop-interval-min: 5s\n"
        "    hop-interval-max: 6s\n"
        "    up: 37 Mbps\n"
        "    down: 83\n"
        "    ca-str: |\n";
    for (const auto &line : Certificate().trimmed().split('\n')) yaml += "      " + line + '\n';
    const auto parsed = fixture.stage(yaml);
    QVERIFY2(parsed.parse_error.isEmpty(), qPrintable(parsed.parse_error));
    QCOMPARE(parsed.rejected_count, 0);
    QCOMPARE(parsed.updated_order.size(), 4);
    const auto anytls = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(parsed.updated_order[0]->bean.get());
    const auto snell4 = dynamic_cast<NekoGui_fmt::SnellBean *>(parsed.updated_order[1]->bean.get());
    const auto snell6 = dynamic_cast<NekoGui_fmt::SnellBean *>(parsed.updated_order[2]->bean.get());
    const auto hysteria = dynamic_cast<NekoGui_fmt::QUICBean *>(parsed.updated_order[3]->bean.get());
    QVERIFY(anytls && snell4 && snell6 && hysteria);
    QCOMPARE(anytls->password, QString("synthetic:yaml@password"));
    QCOMPARE(anytls->sni, QString("localhost"));
    QVERIFY(anytls->allowInsecure);
    QCOMPARE(anytls->alpn, QString("h2,http/1.1"));
    QCOMPARE(anytls->utlsFingerprint, QString("chrome"));
    QCOMPARE(anytls->caText, Certificate());
    QCOMPARE(anytls->idleSessionCheckInterval, QString("12s"));
    QCOMPARE(anytls->idleSessionTimeout, QString("1m30s"));
    QCOMPARE(anytls->minIdleSession, 2);
    QCOMPARE(snell4->snellVersion, 4);
    QCOMPARE(snell4->network, QString("tcp"));
    QCOMPARE(snell4->obfsMode, QString("http"));
    QCOMPARE(snell4->obfsHost, QString("localhost"));
    QCOMPARE(snell6->snellVersion, 6);
    QCOMPARE(snell6->network, QString("udp"));
    QCOMPARE(snell6->userkey, QString("synthetic-user"));
    QVERIFY(snell6->reuse);
    QCOMPARE(snell6->mode, QString("unshaped"));
    QCOMPARE(hysteria->proxy_type, NekoGui_fmt::QUICBean::proxy_Hysteria2);
    QCOMPARE(hysteria->sni, QString("localhost"));
    QCOMPARE(hysteria->caText, Certificate());
    QCOMPARE(hysteria->obfsType, QString("gecko"));
    QCOMPARE(hysteria->obfsPassword, QString("synthetic-gecko-password"));
    QCOMPARE(hysteria->bbrProfile, QString("aggressive"));
    QCOMPARE(hysteria->geckoMinPacketSize, 600);
    QCOMPARE(hysteria->geckoMaxPacketSize, 1300);
    QVERIFY(hysteria->disableChromeParrot);
    QCOMPARE(hysteria->Hysteria2ServerPorts(), (QStringList{"10448:10448", "10450:10451"}));
    QCOMPARE(hysteria->hopIntervalMin, QString("5s"));
    QCOMPARE(hysteria->hopIntervalMax, QString("6s"));
    QCOMPARE(hysteria->uploadMbps, 37);
    QCOMPARE(hysteria->downloadMbps, 83);
    for (const auto &node : parsed.updated_order) {
        const auto roundTrip = fixture.stage(node->bean->ToShareLink());
        QCOMPARE(roundTrip.rejected_count, 0);
        QCOMPARE(roundTrip.updated_order.size(), 1);
        QCOMPARE(roundTrip.updated_order.first()->bean->ToJson(), node->bean->ToJson());
    }
    const auto committed = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, "validated YAML nodes");
    QVERIFY2(committed.applied, qPrintable(committed.error));
    QCOMPARE(committed.added, 4);
    const auto beforeGroup = fixture.group->ToJson();
    const auto nodes = fixture.group->Profiles();
    QList<QJsonObject> beforeNodes;
    QList<QByteArray> beforeFiles;
    for (const auto &node : nodes) {
        beforeNodes.append(node->ToJson());
        beforeFiles.append(SavedFile(node));
    }
    dataStore->sub_clear = true;
    const QString anytlsBase = "type: anytls, server: 127.0.0.1, port: 10443, password: synthetic-password";
    const QString snellBase = "type: snell, server: 127.0.0.1, port: 10446, psk: synthetic-snell-password";
    const QString hysteriaBase = "type: hysteria2, server: 127.0.0.1, port: 10448, password: synthetic-hysteria-password";
    const QStringList invalidEntries{
        snellBase, // A missing version must not be silently assumed to be v4.
        snellBase + ", version: 6, network: udp, udp: false",
        anytlsBase + ", sni: localhost, servername: different.localhost",
        anytlsBase + ", tfo: true",
        anytlsBase + ", dialer-proxy: another-node",
        anytlsBase + ", alpn: {bad: value}",
        anytlsBase + ", client-metadata: \"before\\0after\"",
        snellBase + ", version: 4, obfs-opts: {mode: http, host: localhost, path: /unsupported}",
        hysteriaBase + ", up: garbage",
        hysteriaBase + ", down: 1.25",
        hysteriaBase + ", up: -1",
        hysteriaBase + ", down: 83 KBps",
        hysteriaBase + ", up: 2147483648",
        hysteriaBase + ", sni: \"localhost\\0unexpected\"",
        "type: hysteria2, server: 127.0.0.1, ports: 443, port: nope",
        "type: hysteria2, server: 127.0.0.1, ports: 443, port: 2147483648",
        "type: hysteria2, server: 127.0.0.1, ports: 443, port: 443.5",
        hysteriaBase + ", bbr-profile: standard, bbr-profile: invalid",
        anytlsBase + ", password: silently-replaced-password",
        snellBase + ", version: 4, version: 6",
    };
    for (const auto &entry : invalidEntries) {
        const auto invalid = fixture.stage("proxies:\n  - {" + entry + "}\n");
        QVERIFY2(invalid.updated_order.isEmpty(), qPrintable(entry));
        QVERIFY(invalid.rejected_count > 0);
        const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, invalid, "must not replace valid nodes");
        QVERIFY(!result.applied);
        QCOMPARE(fixture.group->ToJson(), beforeGroup);
        for (int index = 0; index < nodes.size(); ++index) {
            QVERIFY(profileManager->GetProfile(nodes[index]->id) == nodes[index]);
            QCOMPARE(nodes[index]->ToJson(), beforeNodes[index]);
            QCOMPARE(SavedFile(nodes[index]), beforeFiles[index]);
        }
    }
#endif
}

void GuiRegressionTest::newProtocolsRejectInvalidSubscription() {
    using namespace NewProtocolRegression;
    SubscriptionMetadataRegression::GroupFixture fixture;
    auto anytls = AnyTls(true);
    auto snell = Snell(6, true);
    anytls->core_preference = 1;
    snell->core_preference = 2;
    QVERIFY(NekoGui::profileManager->AddProfile(anytls, fixture.group->id));
    QVERIFY(NekoGui::profileManager->AddProfile(snell, fixture.group->id));
    for (const auto &node : {anytls, snell}) {
        node->RecordTestResult(SubscriptionMetadataRegression::Result(NekoGui::TestResultStatus::Success, 1));
        node->Save();
    }
    fixture.group->info = "existing subscription information";
    fixture.group->sub_last_update = 42;
    fixture.group->Save();
    const auto groupBefore = fixture.group->ToJson();
    const auto anytlsBefore = anytls->ToJson();
    const auto snellBefore = snell->ToJson();
    const auto anytlsFile = SavedFile(anytls);
    const auto snellFile = SavedFile(snell);
    auto invalidAnyTls = anytls->bean->ToJson();
    invalidAnyTls["password"] = 42;
    auto invalidSnell = snell->bean->ToJson();
    invalidSnell["version"] = 5;
    const QStringList invalidLinks{
        "anytls://@127.0.0.1:10443", "anytls://fixture@127.0.0.1:10443?insecure=maybe",
        "snell://synthetic-snell-password@127.0.0.1:10446",
        "snell://synthetic-snell-password@127.0.0.1:10446?version=6&obfs_mode=http",
        FullLink("anytls", invalidAnyTls), FullLink("snell", invalidSnell),
    };
    for (bool clear : {false, true}) {
        NekoGui::dataStore->sub_clear = clear;
        for (const auto &link : invalidLinks) {
            const auto parsed = fixture.stage(link);
            QVERIFY2(parsed.updated_order.isEmpty(), qPrintable(link));
            QVERIFY(parsed.rejected_count > 0);
            const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, "must not replace metadata");
            QVERIFY(!result.applied);
            QVERIFY(!result.error.isEmpty());
            QCOMPARE(fixture.group->ToJson(), groupBefore);
            QCOMPARE(fixture.group->Profiles().size(), 2);
            QVERIFY(NekoGui::profileManager->GetProfile(anytls->id) == anytls);
            QVERIFY(NekoGui::profileManager->GetProfile(snell->id) == snell);
            QCOMPARE(anytls->ToJson(), anytlsBefore);
            QCOMPARE(snell->ToJson(), snellBefore);
            QCOMPARE(SavedFile(anytls), anytlsFile);
            QCOMPARE(SavedFile(snell), snellFile);
        }
        // A valid sibling must not make a partially invalid refresh delete
        // the rejected node, its saved core choice or its result history.
        const auto mixed = fixture.stage(anytls->bean->ToShareLink() + '\n' + invalidLinks[3]);
        QCOMPARE(mixed.updated_order.size(), 1);
        QCOMPARE(mixed.rejected_count, 1);
        const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, mixed, "partial refresh must not commit");
        QVERIFY(!result.applied);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(fixture.group->ToJson(), groupBefore);
        QCOMPARE(fixture.group->Profiles().size(), 2);
        QVERIFY(NekoGui::profileManager->GetProfile(anytls->id) == anytls);
        QVERIFY(NekoGui::profileManager->GetProfile(snell->id) == snell);
        QCOMPARE(anytls->ToJson(), anytlsBefore);
        QCOMPARE(snell->ToJson(), snellBefore);
        QCOMPARE(SavedFile(anytls), anytlsFile);
        QCOMPARE(SavedFile(snell), snellFile);
    }
    // Ordinary paste/import uses the same validation without a staged commit.
    NekoGui_sub::RawUpdater direct;
    direct.gid_add_to = fixture.group->id;
    direct.update(invalidLinks.join('\n'));
    QVERIFY(direct.updated_order.isEmpty());
    QCOMPARE(direct.rejected_count, invalidLinks.size());
    QCOMPARE(fixture.group->ToJson(), groupBefore);
    QCOMPARE(SavedFile(anytls), anytlsFile);
    QCOMPARE(SavedFile(snell), snellFile);

    NekoGui_sub::RawUpdater mixedPaste;
    mixedPaste.gid_add_to = fixture.group->id;
    mixedPaste.update(anytls->bean->ToShareLink() + '\n' + invalidLinks[3]);
    QCOMPARE(mixedPaste.updated_order.size(), 1);
    QCOMPARE(mixedPaste.rejected_count, 1);
    QCOMPARE(fixture.group->Profiles().size(), 3);
    QVERIFY(mixedPaste.updated_order.first() != anytls);
    QCOMPARE(mixedPaste.updated_order.first()->bean->ToJson(), anytls->bean->ToJson());
    QCOMPARE(mixedPaste.updated_order.first()->core_preference, 0);
    QCOMPARE(anytls->ToJson(), anytlsBefore);
    QCOMPARE(snell->ToJson(), snellBefore);
    QCOMPARE(SavedFile(anytls), anytlsFile);
    QCOMPARE(SavedFile(snell), snellFile);
}

void GuiRegressionTest::newProtocolsResolveCoreAndEditorChoices() {
    using namespace NewProtocolRegression;
    using namespace NekoGui;
    SubscriptionMetadataRegression::GroupFixture fixture;
    const auto runningBefore = dataStore->started_id;
    for (const auto &node : {AnyTls(), Snell(4), Snell(6)}) {
        for (int preference : {0, 1, 2}) {
            node->core_preference = preference;
            for (int defaultCore : {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
                const auto resolution = ResolveCore(node, defaultCore);
                QVERIFY(!resolution.xraySupported);
                QVERIFY2(resolution.singBoxSupported, qPrintable(resolution.singBoxReason));
                QCOMPARE(resolution.targetCore, int(CoreType::SING_BOX));
                QCOMPARE(resolution.preferenceFallback, preference == 1);
                QCOMPARE(node->core_preference, preference);
            }
        }
        node->core_preference = 1;
        QVERIFY(profileManager->AddProfile(node, fixture.group->id));
        const auto originalBean = node->bean->ToJson();
        DialogEditProfile editor("", node->id, window);
        editor.show();
        auto xray = child<QRadioButton>(&editor, "edit_core_xray");
        QVERIFY(!xray->isEnabled());
        QVERIFY(xray->isChecked());
        QVERIFY(!xray->toolTip().isEmpty());
        QVERIFY(child<QRadioButton>(&editor, "edit_core_singbox")->isEnabled());
        QVERIFY(child<QRadioButton>(&editor, "edit_core_auto")->isEnabled());
        QVERIFY(child<QLabel>(&editor, "edit_core_reason")->text().contains("sing-box"));
        editor.accept();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
        QCOMPARE(node->core_preference, 1);
        QCOMPARE(node->bean->ToJson(), originalBean);
    }
    {
        DialogEditProfile editor("anytls", fixture.group->id, window);
        editor.show();
        auto types = child<QComboBox>(&editor, "type");
        QVERIFY(types->findData("anytls") >= 0);
        QVERIFY(types->findData("snell") >= 0);
        QCOMPARE(types->currentData().toString(), QString("anytls"));
        types->setCurrentIndex(types->findData("snell"));
        child<QLineEdit>(&editor, "snellPsk")->setText("synthetic-new-node-password");
        QVERIFY(child<QRadioButton>(&editor, "edit_core_auto")->isChecked());
        QVERIFY(child<QRadioButton>(&editor, "edit_core_singbox")->isEnabled());
        editor.accept();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
    }
    QCOMPARE(fixture.group->Profiles().size(), 4);
    const auto created = fixture.group->Profiles().last();
    QCOMPARE(created->type, QString("snell"));
    QCOMPARE(created->core_preference, 0);
    QCOMPARE(ResolveCore(created, CoreType::V2RAY).targetCore, int(CoreType::SING_BOX));
    QCOMPARE(dataStore->started_id, runningBefore);
}

void GuiRegressionTest::anyTlsEditorDraftAndAdvancedSave() {
    using namespace NewProtocolRegression;
    SubscriptionMetadataRegression::GroupFixture fixture;
    auto node = AnyTls(true);
    node->core_preference = 1;
    QVERIFY(NekoGui::profileManager->AddProfile(node, fixture.group->id));
    const auto original = node->ToJson();
    {
        DialogEditProfile editor("", node->id, window);
        editor.show();
        QVERIFY(!child<QToolButton>(&editor, "anytlsAdvancedToggle")->isChecked());
        QVERIFY(child<QWidget>(&editor, "anytlsAdvanced")->isHidden());
        QCOMPARE(child<QPlainTextEdit>(&editor, "anytlsCertificate")->toPlainText(), Certificate());
        child<QLineEdit>(&editor, "name")->setText("discarded AnyTLS edit");
        child<QLineEdit>(&editor, "anytlsPassword")->setText("discarded password");
        child<QLineEdit>(&editor, "anytlsMetadata")->setText("discarded metadata");
        child<QRadioButton>(&editor, "edit_core_auto")->click();
        QCOMPARE(node->ToJson(), original);
        editor.reject();
    }
    QCOMPARE(node->ToJson(), original);
    {
        DialogEditProfile editor("", node->id, window);
        editor.show();
        child<QToolButton>(&editor, "anytlsAdvancedToggle")->click();
        QVERIFY(!child<QWidget>(&editor, "anytlsAdvanced")->isHidden());
        child<QLineEdit>(&editor, "name")->setText("saved AnyTLS edit");
        child<QLineEdit>(&editor, "anytlsPassword")->setText("saved:synthetic@password");
        child<QLineEdit>(&editor, "anytlsSni")->setText("localhost");
        child<QCheckBox>(&editor, "anytlsInsecure")->setChecked(true);
        child<QLineEdit>(&editor, "anytlsAlpn")->setText("h2,http/1.1");
        child<QComboBox>(&editor, "anytlsFingerprint")->setCurrentText("firefox");
        child<QPlainTextEdit>(&editor, "anytlsCertificate")->setPlainText(Certificate());
        child<QLineEdit>(&editor, "anytlsCheckInterval")->setText("21s");
        child<QLineEdit>(&editor, "anytlsIdleTimeout")->setText("2m10s");
        child<QLineEdit>(&editor, "anytlsMinIdleSession")->setText("5");
        child<QLineEdit>(&editor, "anytlsMetadata")->setText(QString::fromUtf8("保存的测试客户端"));
        child<QRadioButton>(&editor, "edit_core_singbox")->click();
        node->RecordTestResult(SubscriptionMetadataRegression::Result(NekoGui::TestResultStatus::Success, 1));
        const auto history = node->test_results;
        editor.accept();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
        QCOMPARE(node->test_results, history);
    }
    auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(node->bean.get());
    QVERIFY(bean);
    QCOMPARE(node->core_preference, 2);
    QCOMPARE(bean->name, QString("saved AnyTLS edit"));
    QCOMPARE(bean->password, QString("saved:synthetic@password"));
    QCOMPARE(bean->sni, QString("localhost"));
    QVERIFY(bean->allowInsecure);
    QCOMPARE(bean->alpn, QString("h2,http/1.1"));
    QCOMPARE(bean->utlsFingerprint, QString("firefox"));
    QCOMPARE(bean->caText, Certificate());
    QCOMPARE(bean->idleSessionCheckInterval, QString("21s"));
    QCOMPARE(bean->idleSessionTimeout, QString("2m10s"));
    QCOMPARE(bean->minIdleSession, 5);
    QCOMPARE(bean->clientMetadata, QString::fromUtf8("保存的测试客户端"));
    QVERIFY2(bean->Validate().isEmpty(), qPrintable(bean->Validate()));
    const auto built = bean->BuildCoreObjSingBox();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QCOMPARE(built.outbound["tls"].toObject()["certificate"].toArray(), QJsonArray{Certificate()});
    QCOMPARE(built.outbound["min_idle_session"].toInt(), 5);
    auto loaded = NekoGui::ProfileManager::NewProxyEntity("anytls");
    loaded->fn = node->fn;
    QVERIFY(loaded->Load());
    QCOMPARE(loaded->bean->ToJson(), bean->ToJson());
    QCOMPARE(loaded->core_preference, 2);
    {
        DialogEditProfile editor("", node->id, window);
        editor.show();
        QVERIFY(!child<QToolButton>(&editor, "anytlsAdvancedToggle")->isChecked());
        QVERIFY(child<QWidget>(&editor, "anytlsAdvanced")->isHidden());
        QCOMPARE(child<QLineEdit>(&editor, "anytlsMetadata")->text(), bean->clientMetadata);
        editor.reject();
    }

    auto longNode = AnyTls(true);
    auto longBean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(longNode->bean.get());
    longBean->clientMetadata = QString(40000, 'm');
    longBean->clientMetadata.replace(39992, 8, "end-data");
    const auto longMetadata = longBean->clientMetadata;
    const auto crlfCertificate = Certificate().replace("\n", "\r\n");
    longBean->caText = crlfCertificate;
    QVERIFY(NekoGui::profileManager->AddProfile(longNode, fixture.group->id));
    const auto beforeLongEdit = longBean->ToJson({"name"});
    {
        DialogEditProfile editor("", longNode->id, window);
        editor.show();
        auto metadata = child<QLineEdit>(&editor, "anytlsMetadata");
        QCOMPARE(metadata->text().size(), 40000);
        QVERIFY(metadata->text() == longMetadata);
        QCOMPARE(child<QPlainTextEdit>(&editor, "anytlsCertificate")->toPlainText(), Certificate());
        child<QLineEdit>(&editor, "name")->setText("long metadata remains intact");
        editor.accept();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
    }
    QVERIFY(longNode->bean->ToJson({"name"}) == beforeLongEdit);
    auto loadedLong = NekoGui::ProfileManager::NewProxyEntity("anytls");
    loadedLong->fn = longNode->fn;
    QVERIFY(loadedLong->Load());
    const auto restoredLong = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(loadedLong->bean.get());
    QVERIFY(restoredLong);
    QCOMPARE(restoredLong->clientMetadata.size(), 40000);
    QVERIFY(restoredLong->clientMetadata == longMetadata);
    QCOMPARE(restoredLong->caText, crlfCertificate);
}

void GuiRegressionTest::snellEditorVersionDraftsAndSave() {
    using namespace NewProtocolRegression;
    SubscriptionMetadataRegression::GroupFixture fixture;
    auto node = Snell(4, true);
    QVERIFY(NekoGui::profileManager->AddProfile(node, fixture.group->id));
    const auto original = node->ToJson();
    {
        DialogEditProfile editor("", node->id, window);
        editor.show();
        auto toggle = child<QToolButton>(&editor, "snellAdvancedToggle");
        QVERIFY(!toggle->isChecked());
        QVERIFY(child<QWidget>(&editor, "snellAdvanced")->isHidden());
        toggle->click();
        auto version = child<QComboBox>(&editor, "snellVersion");
        auto obfs = child<QComboBox>(&editor, "snellObfsMode");
        auto host = child<QLineEdit>(&editor, "snellObfsHost");
        auto mode = child<QComboBox>(&editor, "snellMode");
        obfs->setCurrentIndex(obfs->findData("http"));
        host->setText("draft.localhost");
        version->setCurrentIndex(version->findData(6));
        QVERIFY(obfs->isHidden());
        QVERIFY(host->isHidden());
        QVERIFY(!mode->isHidden());
        mode->setCurrentIndex(mode->findData("unshaped"));
        version->setCurrentIndex(version->findData(4));
        QCOMPARE(obfs->currentData().toString(), QString("http"));
        QCOMPARE(host->text(), QString("draft.localhost"));
        QVERIFY(mode->isHidden());
        version->setCurrentIndex(version->findData(6));
        QCOMPARE(mode->currentData().toString(), QString("unshaped"));
        QCOMPARE(node->ToJson(), original);
        editor.reject();
    }
    QCOMPARE(node->ToJson(), original);
    {
        DialogEditProfile editor("", node->id, window);
        editor.show();
        child<QToolButton>(&editor, "snellAdvancedToggle")->click();
        auto version = child<QComboBox>(&editor, "snellVersion");
        version->setCurrentIndex(version->findData(6));
        child<QLineEdit>(&editor, "snellPsk")->setText("saved-snell-six-password");
        child<QLineEdit>(&editor, "snellUserkey")->setText(QString::fromUtf8("synthetic 用户"));
        child<QCheckBox>(&editor, "snellReuse")->setChecked(true);
        auto network = child<QComboBox>(&editor, "snellNetwork");
        network->setCurrentIndex(network->findData("udp"));
        auto mode = child<QComboBox>(&editor, "snellMode");
        mode->setCurrentIndex(mode->findData("unshaped"));
        editor.accept();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
    }
    auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(node->bean.get());
    QVERIFY(bean);
    QCOMPARE(bean->snellVersion, 6);
    QCOMPARE(bean->psk, QString("saved-snell-six-password"));
    QCOMPARE(bean->userkey, QString::fromUtf8("synthetic 用户"));
    QCOMPARE(bean->network, QString("udp"));
    QVERIFY(bean->reuse);
    QCOMPARE(bean->mode, QString("unshaped"));
    QVERIFY(bean->obfsMode.isEmpty());
    QVERIFY(bean->obfsHost.isEmpty());
    QVERIFY2(bean->Validate().isEmpty(), qPrintable(bean->Validate()));
    const auto v6 = bean->BuildCoreObjSingBox();
    QVERIFY2(v6.error.isEmpty(), qPrintable(v6.error));
    QCOMPARE(v6.outbound["mode"].toString(), QString("unshaped"));
    QVERIFY(!v6.outbound.contains("obfs_mode"));
    QVERIFY(!v6.outbound.contains("obfs_host"));
    {
        DialogEditProfile editor("", node->id, window);
        editor.show();
        QVERIFY(child<QWidget>(&editor, "snellAdvanced")->isHidden());
        child<QToolButton>(&editor, "snellAdvancedToggle")->click();
        auto version = child<QComboBox>(&editor, "snellVersion");
        version->setCurrentIndex(version->findData(4));
        auto obfs = child<QComboBox>(&editor, "snellObfsMode");
        obfs->setCurrentIndex(obfs->findData("tls"));
        child<QLineEdit>(&editor, "snellObfsHost")->setText("localhost");
        auto network = child<QComboBox>(&editor, "snellNetwork");
        network->setCurrentIndex(network->findData("tcp"));
        QVERIFY(child<QComboBox>(&editor, "snellMode")->isHidden());
        editor.accept();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
    }
    bean = dynamic_cast<NekoGui_fmt::SnellBean *>(node->bean.get());
    QCOMPARE(bean->snellVersion, 4);
    QCOMPARE(bean->obfsMode, QString("tls"));
    QCOMPARE(bean->obfsHost, QString("localhost"));
    QVERIFY(bean->mode.isEmpty());
    QCOMPARE(bean->network, QString("tcp"));
    QCOMPARE(bean->userkey, QString::fromUtf8("synthetic 用户"));
    QVERIFY(bean->reuse);
    QVERIFY2(bean->Validate().isEmpty(), qPrintable(bean->Validate()));
    auto loaded = NekoGui::ProfileManager::NewProxyEntity("snell");
    loaded->fn = node->fn;
    QVERIFY(loaded->Load());
    QCOMPARE(loaded->bean->ToJson(), bean->ToJson());
}

void GuiRegressionTest::newProtocolsSubscriptionKeepsMetadata() {
    using namespace NewProtocolRegression;
    using namespace NekoGui;
    for (bool clear : {false, true}) {
        SubscriptionMetadataRegression::GroupFixture fixture;
        dataStore->sub_clear = clear;
        const QList<std::shared_ptr<ProxyEntity>> nodes{AnyTls(true), Snell(4, true), Snell(6, true)};
        QList<QJsonObject> savedBeans;
        QList<QJsonObject> histories;
        for (int index = 0; index < nodes.size(); ++index) {
            const auto &node = nodes[index];
            node->core_preference = index == 0 ? 1 : index == 1 ? 2 : 0;
            QVERIFY(profileManager->AddProfile(node, fixture.group->id));
            node->RecordTestResult(SubscriptionMetadataRegression::Result(TestResultStatus::Success, 1));
            node->Save();
            savedBeans.append(node->bean->ToJson());
            histories.append(node->test_results);
        }
        QStringList incoming;
        for (int index : {2, 0, 1}) {
            auto copy = ProfileManager::NewProxyEntity(nodes[index]->type);
            copy->bean->FromJson(savedBeans[index]);
            copy->bean->name = QString("renamed subscription node %1").arg(index);
            incoming.append(copy->bean->ToShareLink());
        }
        const auto parsed = fixture.stage(incoming.join('\n'));
        QCOMPARE(parsed.rejected_count, 0);
        QCOMPARE(parsed.updated_order.size(), 3);
        for (const auto &node : parsed.updated_order) {
            QCOMPARE(node->id, -1);
            QCOMPARE(node->core_preference, 0);
        }
        const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, "new subscription metadata");
        QVERIFY2(result.applied, qPrintable(result.error));
        QCOMPARE(result.preserved, 3);
        QCOMPARE(result.added, 0);
        QCOMPARE(result.removed, 0);
        QCOMPARE(fixture.group->order, (QList<int>{nodes[2]->id, nodes[0]->id, nodes[1]->id}));
        for (int index = 0; index < nodes.size(); ++index) {
            const auto &node = nodes[index];
            QVERIFY(profileManager->GetProfile(node->id) == node);
            QCOMPARE(node->core_preference, index == 0 ? 1 : index == 1 ? 2 : 0);
            auto expected = savedBeans[index];
            expected["name"] = QString("renamed subscription node %1").arg(index);
            QCOMPARE(node->bean->ToJson(), expected);
            QCOMPARE(node->test_results, histories[index]);
            auto loaded = ProfileManager::NewProxyEntity(node->type);
            loaded->fn = node->fn;
            QVERIFY(loaded->Load());
            QCOMPARE(loaded->id, node->id);
            QCOMPARE(loaded->bean->ToJson(), expected);
            QCOMPARE(loaded->core_preference, node->core_preference);
            QCOMPARE(loaded->test_results, histories[index]);
        }
    }
}

void GuiRegressionTest::anyTlsResolveDomainPreservesSni() {
    using namespace NewProtocolRegression;
    for (const auto &explicitSni : {QString{}, QString("peer.localhost")}) {
        auto node = AnyTls();
        auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(node->bean.get());
        bean->serverAddress = "localhost";
        bean->sni = explicitSni;
        auto finished = std::make_shared<bool>(false);
        // Keep the bean and completion flag alive even if a lookup exceeds
        // the test's deadline; the asynchronous production API retains them.
        bean->ResolveDomainToIP([finished, node] { *finished = true; });
        QTRY_VERIFY_WITH_TIMEOUT(*finished, 10000);
        QHostAddress address;
        QVERIFY(address.setAddress(bean->serverAddress));
        QVERIFY(address.isLoopback());
        QCOMPARE(bean->sni, explicitSni.isEmpty() ? QString("localhost") : explicitSni);
        const auto built = bean->BuildCoreObjSingBox();
        QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
        QCOMPARE(built.outbound["tls"].toObject()["server_name"].toString(), bean->sni);
        SubscriptionMetadataRegression::GroupFixture fixture;
        const auto shared = fixture.stage(bean->ToShareLink());
        QCOMPARE(shared.rejected_count, 0);
        QCOMPARE(shared.updated_order.size(), 1);
        QCOMPARE(shared.updated_order.first()->bean->ToJson(), bean->ToJson());
    }
    auto literal = AnyTls();
    auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(literal->bean.get());
    bean->sni.clear();
    const auto original = bean->ToJson();
    bool finished = false;
    bean->ResolveDomainToIP([&finished] { finished = true; });
    QVERIFY(finished);
    QCOMPARE(bean->ToJson(), original);
}

void GuiRegressionTest::snellUrlBatchRespectsNetwork() {
    using namespace NewProtocolRegression;
    using namespace NekoGui;
    QScopedValueRollback<int> timeout(dataStore->test_latency_timeout, 1000);
    QScopedValueRollback<QString> url(dataStore->test_latency_url, "http://127.0.0.1:1/generate_204");
    std::vector<std::unique_ptr<RejectingPeer>> peers;
    QList<std::shared_ptr<ProxyEntity>> nodes;
    for (int version : {4, 6}) {
        for (const auto &network : {QString{}, QString("tcp"), QString("udp")}) {
            peers.push_back(std::make_unique<RejectingPeer>());
            auto node = Snell(version);
            auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(node->bean.get());
            bean->serverPort = peers.back()->server.serverPort();
            bean->network = network;
            node->core_preference = 1; // A saved Xray choice must remain intact.
            QVERIFY(profileManager->AddProfile(node, baseGroup));
            nodes.append(node);
        }
    }
    startBatch();
    QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch, 15000);
    QVERIFY(window->core_manager->Ready(CoreType::SING_BOX));
    for (int index = 0; index < nodes.size(); ++index) {
        const auto &node = nodes[index];
        const auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(node->bean.get());
        const auto result = node->LastTestResult(CoreType::SING_BOX);
        QCOMPARE(node->core_preference, 1);
        QCOMPARE(result.core, int(CoreType::SING_BOX));
        QCOMPARE(result.mode, QString("url"));
        QVERIFY(result.timestamp_ms > 0);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(result.build_id, window->core_manager->Identity(CoreType::SING_BOX));
        QCOMPARE(node->LastTestResult(CoreType::V2RAY).timestamp_ms, qint64(0));
        if (bean->network == "udp") {
            QCOMPARE(result.status, TestResultStatus::Unsupported);
            QCOMPARE(peers[index]->connections, 0);
            QVERIFY(!node->IsRemovalCandidate(CoreType::SING_BOX, result.build_id,
                                               window->test_fingerprint(node, CoreType::SING_BOX)));
        } else {
            // The synthetic peer closes every connection. Reaching it proves
            // TCP-only and dual-network nodes remain eligible for URL tests.
            QCOMPARE(result.status, TestResultStatus::ConnectionFailure);
            QVERIFY(peers[index]->connections > 0);
            QVERIFY(node->IsRemovalCandidate(CoreType::SING_BOX, result.build_id,
                                              window->test_fingerprint(node, CoreType::SING_BOX)));
        }
    }
}
