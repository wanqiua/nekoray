#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <future>

#include "db/ConfigBuilder.hpp"
#include "db/CoreCapabilities.hpp"
#include "db/Database.hpp"
#include "fmt/includes.h"
#include "sub/GroupUpdater.hpp"

using namespace NekoGui;
using namespace NekoGui_fmt;

class CoreCapabilitiesTest : public QObject {
    Q_OBJECT
    DataStore *originalSettings = nullptr;
    ProfileManager *originalProfiles = nullptr;
    int originalCore = CoreType::V2RAY;
    QString originalDirectory;
    std::unique_ptr<QTemporaryDir> directory;
    int nextId = 1;

    std::shared_ptr<ProxyEntity> node(const QString &type) {
        auto ent = ProfileManager::NewProxyEntity(type);
        ent->id = nextId++;
        ent->gid = 1;
        ent->bean->serverAddress = "example.invalid";
        ent->bean->serverPort = 443;
        profileManager->profiles[ent->id] = ent;
        return ent;
    }

    static QJsonObject outbound(const QJsonObject &config, const QString &tag = "proxy") {
        for (const auto &entry: config["outbounds"].toArray()) if (entry.toObject()["tag"] == tag) return entry.toObject();
        return {};
    }

    static QJsonObject inbound(const QJsonObject &config, const QString &tag) {
        for (const auto &entry: config["inbounds"].toArray()) if (entry.toObject()["tag"] == tag) return entry.toObject();
        return {};
    }

private slots:
    void certificatePinClashImport() {
#ifdef NKR_NO_YAML
        QSKIP("Clash import is disabled");
#else
        for (const auto &type: {"vless", "trojan", "vmess"}) {
            for (const auto &value: {QString(64, 'a'), QString("chrome"), QString("not-a-pin")}) {
                NekoGui_sub::RawUpdater updater;
                updater.stage_only = true;
                updater.updateClash(QString("proxies:\n  - name: pin fixture\n    type: %1\n    server: example.invalid\n    port: 443\n    uuid: 75f2a015-3611-4c7f-bf7c-a948276b179f\n    password: synthetic\n    tls: true\n    fingerprint: '%2'\n").arg(type, value));
                if (value == "not-a-pin") {
                    QCOMPARE(updater.rejected_count, 1);
                    QVERIFY(updater.updated_order.isEmpty());
                } else {
                    QCOMPARE(updater.rejected_count, 0);
                    QCOMPARE(updater.updated_order.size(), 1);
                    QCOMPARE(GetStreamSettings(updater.updated_order.first()->bean.get())->certificate_pin,
                             value == "chrome" ? QString() : value);
                }
            }
        }
#endif
    }

    void certificatePinParsingAndSharing() {
        const QString pin(64, 'a');
        QStringList pins;
        QVERIFY(V2rayStreamSettings::ParseCertificatePins(QString("AA:").repeated(31) + "AA", &pins));
        QCOMPARE(pins, QStringList{pin});
        for (const auto &bad: {QString(63, 'a'), pin + ",", "zz" + pin, QString("aa:").repeated(32)})
            QVERIFY(!V2rayStreamSettings::ParseCertificatePins(bad));
        for (const auto &type: {"vless", "trojan", "vmess"}) {
            for (const auto &value: {"1", "true", "0", "false"}) {
                auto ent = node(type);
                const auto link = QString("%1://75f2a015-3611-4c7f-bf7c-a948276b179f@example.invalid:443?security=tls&insecure=%2&pcs=%3").arg(type, value, pin);
                QString error;
                const bool ok = QString(type) == "vmess" ? ent->VMessBean()->TryParseLink(link, &error)
                                                         : ent->TrojanVLESSBean()->TryParseLink(link, &error);
                QVERIFY2(ok, qPrintable(error));
                const auto stream = GetStreamSettings(ent->bean.get());
                QCOMPARE(stream->allow_insecure, QString(value) == "1" || QString(value) == "true");
                QCOMPARE(stream->certificate_pin, pin);
                for (bool legacy: {false, true}) {
                    dataStore->old_share_link_format = legacy;
                    auto copy = node(type);
                    const auto share = ent->bean->ToShareLink();
                    QVERIFY(QString(type) == "vmess" ? copy->VMessBean()->TryParseLink(share, &error)
                                                     : copy->TrojanVLESSBean()->TryParseLink(share, &error));
                    QCOMPARE(GetStreamSettings(copy->bean.get())->certificate_pin, pin);
                    QCOMPARE(GetStreamSettings(copy->bean.get())->allow_insecure, stream->allow_insecure);
                }
                const auto conflict = link + "&allowInsecure=0";
                QVERIFY(QString(type) == "vmess" ? ent->VMessBean()->TryParseLink(conflict, &error)
                                                 : ent->TrojanVLESSBean()->TryParseLink(conflict, &error));
                QVERIFY(!stream->allow_insecure);
                const auto malformed = QString(link).replace(pin, "bad-pin");
                QVERIFY(!(QString(type) == "vmess" ? ent->VMessBean()->TryParseLink(malformed, &error)
                                                   : ent->TrojanVLESSBean()->TryParseLink(malformed, &error)));
                QVERIFY(error.contains("pcs"));
            }
        }
    }

    void certificatePinSelectsXrayAndRejectsInvalidSettings() {
        for (const auto &type: {"vless", "trojan", "vmess"}) {
            auto ent = node(type);
            const auto stream = GetStreamSettings(ent->bean.get());
            stream->security = "tls";
            stream->certificate_pin = QString(64, 'a');
            const auto resolution = ResolveCore(ent, CoreType::SING_BOX);
            QVERIFY(resolution.xraySupported);
            QVERIFY(!resolution.singBoxSupported);
            QCOMPARE(resolution.targetCore, int(CoreType::V2RAY));
            QVERIFY(!ent->bean->BuildCoreObjSingBox(dataStore).error.isEmpty());
            const auto tls = ent->bean->BuildCoreObjV2Ray(dataStore).outbound["streamSettings"].toObject()["tlsSettings"].toObject();
            QVERIFY(tls["allowInsecure"].toBool());
            QCOMPARE(tls["pinnedPeerCertificateChainSha256"].toArray().first().toString(),
                     QString::fromLatin1(QByteArray::fromHex(QByteArray(64, 'a')).toBase64()));
            stream->certificate_pin = "invalid";
            QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, -1);
            QVERIFY(!ent->bean->BuildCoreObjV2Ray(dataStore).error.isEmpty());
            stream->certificate_pin = QString(64, 'a');
            stream->security = "";
            QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, -1);
            stream->security = "tls";
            stream->reality_pbk = "synthetic-public-key";
            QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, -1);
        }
    }

    void initTestCase() {
        originalSettings = dataStore;
        originalProfiles = profileManager;
        originalCore = coreType;
        originalDirectory = QDir::currentPath();
        directory = std::make_unique<QTemporaryDir>();
        QVERIFY(directory->isValid());
        QVERIFY(QDir::setCurrent(directory->path()));
    }

    void init() {
        ClearCoreRuntimeFeatures(CoreType::V2RAY);
        ClearCoreRuntimeFeatures(CoreType::SING_BOX);
        coreType = CoreType::V2RAY;
        dataStore = new DataStore;
        dataStore->routing = std::make_unique<Routing>();
        dataStore->save_control_no_save = true;
        profileManager = new ProfileManager;
        auto group = ProfileManager::NewGroup();
        group->id = 1;
        profileManager->groups[1] = group;
        nextId = 1;
    }

    void cleanup() {
        ClearCoreRuntimeFeatures(CoreType::V2RAY);
        ClearCoreRuntimeFeatures(CoreType::SING_BOX);
        delete dataStore->inbound_auth;
        delete dataStore->extraCore;
        delete dataStore;
        delete profileManager;
        dataStore = originalSettings;
        profileManager = originalProfiles;
        coreType = originalCore;
    }

    void cleanupTestCase() {
        QVERIFY(QDir::setCurrent(originalDirectory));
        directory.reset();
    }

    void preferenceAndProtocolIntersection() {
        auto ss = node("shadowsocks");
        ss->ShadowSocksBean()->method = "rc4-md5";
        auto resolution = ResolveCore(ss, CoreType::SING_BOX);
        QVERIFY(resolution.xraySupported && resolution.singBoxSupported);
        QCOMPARE(resolution.targetCore, int(CoreType::SING_BOX));
        ss->core_preference = 1;
        QCOMPARE(ResolveCore(ss, CoreType::SING_BOX).targetCore, int(CoreType::V2RAY));
        ss->ShadowSocksBean()->method = "bf-cfb";
        ss->core_preference = 2;
        resolution = ResolveCore(ss, CoreType::SING_BOX);
        QCOMPARE(resolution.targetCore, int(CoreType::V2RAY));
        QVERIFY(resolution.preferenceFallback);
        QCOMPARE(ss->core_preference, 2);

        auto hy = node("hysteria2");
        QCOMPARE(ResolveCore(hy, CoreType::V2RAY).targetCore, int(CoreType::SING_BOX));
        auto chain = node("chain");
        chain->ChainBean()->list = {ss->id, hy->id};
        resolution = ResolveCore(chain, CoreType::V2RAY);
        QCOMPARE(resolution.targetCore, -1);
        QVERIFY(!resolution.reason.isEmpty());
        profileManager->groups[1]->front_proxy_id = hy->id;
        QCOMPARE(ResolveCore(ss, CoreType::V2RAY).targetCore, -1);
    }

    void transportAndSecurityBoundaries() {
        auto ent = node("vless");
        auto stream = ent->TrojanVLESSBean()->stream;
        stream->security = "tls";
        stream->allow_insecure = true;
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
        stream->network = "httpupgrade";
        QCOMPARE(ResolveCore(ent, CoreType::V2RAY).targetCore, int(CoreType::SING_BOX));
        stream->network = "xhttp";
        QCOMPARE(ResolveCore(ent, CoreType::V2RAY).targetCore, -1);
        stream->network = "kcp";
        stream->path = "synthetic-seed";
        stream->header_type = "srtp";
        QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, int(CoreType::V2RAY));
        const auto kcp = ent->bean->BuildCoreObjV2Ray(dataStore).outbound["streamSettings"].toObject()["kcpSettings"].toObject();
        QCOMPARE(kcp["seed"].toString(), QString("synthetic-seed"));
        QCOMPARE(kcp["header"].toObject()["type"].toString(), QString("srtp"));
        stream->reality_pbk = "synthetic-public-key";
        QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, -1);
        stream->reality_pbk.clear();
        stream->network = "quic";
        stream->path = "synthetic-key";
        stream->host = "aes-128-gcm";
        stream->header_type = "none";
        QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, int(CoreType::V2RAY));
        stream->path.clear();
        stream->host.clear();
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
        stream->network = "tcp";
        ent->TrojanVLESSBean()->flow = "xtls-rprx-vision-udp443";
        QVERIFY(!ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
    }

    void socksVersionAndTransportAreNotDropped() {
        auto ent = node("socks");
        auto bean = ent->SocksHTTPBean();
        bean->socks_http_type = SocksHttpBean::type_Socks4;
        bean->username = "synthetic-user";
        const auto ray = bean->BuildCoreObjV2Ray(dataStore).outbound;
        QCOMPARE(ray["settings"].toObject()["version"].toString(), QString("4"));
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
        bean->stream->security = "tls";
        QVERIFY(!ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
        QVERIFY(!bean->BuildCoreObjSingBox(dataStore).error.isEmpty());
        bean->socks_http_type = SocksHttpBean::type_HTTP;
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
        bean->stream->network = "ws";
        QVERIFY(!ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
    }

    void snapshotAndParallelBuildsKeepTlsAndCoreIsolated() {
        auto ent = node("vless");
        ent->TrojanVLESSBean()->stream->security = "tls";
        const auto rayContext = CaptureBuildConfigContext(CoreType::V2RAY);
        const auto boxContext = CaptureBuildConfigContext(CoreType::SING_BOX);
        dataStore->skip_cert = true;
        dataStore->routing->direct_dns = "192.0.2.99";
        auto rayTask = std::async(std::launch::async, [&] { return BuildConfig(ent, true, false, rayContext); });
        auto boxTask = std::async(std::launch::async, [&] { return BuildConfig(ent, true, false, boxContext); });
        const auto ray = rayTask.get();
        const auto box = boxTask.get();
        QVERIFY2(ray->error.isEmpty(), qPrintable(ray->error));
        QVERIFY2(box->error.isEmpty(), qPrintable(box->error));
        QCOMPARE(coreType, int(CoreType::V2RAY));
        QCOMPARE(outbound(ray->coreConfig)["protocol"].toString(), QString("vless"));
        QCOMPARE(outbound(box->coreConfig)["type"].toString(), QString("vless"));
        QVERIFY(!outbound(ray->coreConfig)["streamSettings"].toObject()["tlsSettings"].toObject()["allowInsecure"].toBool());
        QVERIFY(!outbound(box->coreConfig)["tls"].toObject()["insecure"].toBool());
        QVERIFY(ray->Fingerprint() != box->Fingerprint());
        QCOMPARE(rayContext.settings->routing->direct_dns, QString("localhost"));
        const auto hash = boxContext.Fingerprint();
        boxContext.settings->splitter_state = "changed-ui-only";
        QCOMPARE(hash, boxContext.Fingerprint());
        boxContext.settings->skip_cert = true;
        QVERIFY(hash != boxContext.Fingerprint());
    }

    void preparingAChainDoesNotRetagTheRunningNode() {
        const auto running = node("http");
        const auto other = node("http");
        const auto chain = node("chain");
        chain->ChainBean()->list = {running->id, other->id};
        running->traffic_data->id = running->id;
        running->traffic_data->tag = "proxy";
        other->traffic_data->id = -1;
        other->traffic_data->tag = "unstarted-other";
        chain->traffic_data->id = -1;
        chain->traffic_data->tag = "unstarted-chain";
        const auto bindingTag = [](const std::shared_ptr<BuildConfigResult> &result, int id) {
            for (const auto &binding: result->trafficBindings) {
                if (binding.id == id) return QString::fromStdString(binding.tag);
            }
            return QString();
        };
        for (int target: {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
            const auto context = CaptureBuildConfigContext(target);
            const auto original = BuildConfig(running, false, false, context);
            QVERIFY2(original->error.isEmpty(), qPrintable(original->error));
            QCOMPARE(bindingTag(original, running->id), QString("proxy"));
            for (bool probe: {false, true}) {
                const auto candidate = BuildConfig(chain, probe, false, context);
                QVERIFY2(candidate->error.isEmpty(), qPrintable(candidate->error));
                QCOMPARE(running->traffic_data->id, running->id);
                QCOMPARE(QString::fromStdString(running->traffic_data->tag), QString("proxy"));
                QCOMPARE(other->traffic_data->id, -1);
                QCOMPARE(QString::fromStdString(other->traffic_data->tag), QString("unstarted-other"));
                QCOMPARE(chain->traffic_data->id, -1);
                QCOMPARE(QString::fromStdString(chain->traffic_data->tag), QString("unstarted-chain"));
                QCOMPARE(bindingTag(original, running->id), QString("proxy"));
                if (probe) {
                    QVERIFY(candidate->trafficBindings.isEmpty());
                    QVERIFY(candidate->outboundStats.isEmpty());
                } else {
                    QCOMPARE(candidate->trafficBindings.size(), 3);
                    QCOMPARE(bindingTag(candidate, running->id), "g-" + QString::number(running->id));
                    QCOMPARE(bindingTag(candidate, other->id), QString("proxy"));
                    QCOMPARE(bindingTag(candidate, chain->id), QString("proxy"));
                }
            }
        }
    }

    void defaultSettingsRemainPortable_data() {
        QTest::addColumn<int>("sourceCore");
        QTest::addColumn<QString>("protocol");
        for (int source: {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
            for (const auto &protocol: {QString("http"), QString("vless"), QString("shadowsocks")}) {
                const auto name = QString("%1-%2").arg(source).arg(protocol).toLatin1();
                QTest::newRow(name.constData()) << source << protocol;
            }
        }
    }

    void defaultSettingsRemainPortable() {
        QFETCH(int, sourceCore);
        QFETCH(QString, protocol);
        coreType = sourceCore;
        dataStore->routing = std::make_unique<Routing>();
        dataStore->skip_cert = true;
        auto ent = node(protocol);
        if (protocol == "shadowsocks") {
            ent->ShadowSocksBean()->method = "rc4-md5";
            ent->ShadowSocksBean()->password = "synthetic-password";
        } else {
            GetStreamSettings(ent->bean.get())->security = "tls";
            if (protocol == "vless") ent->TrojanVLESSBean()->password = "11111111-1111-4111-8111-111111111111";
            else {
                ent->SocksHTTPBean()->username = "synthetic-user";
                ent->SocksHTTPBean()->password = "synthetic-password";
            }
        }
        const auto resolution = ResolveCore(ent, 1 - sourceCore);
        QVERIFY2(resolution.xraySupported, qPrintable(resolution.xrayReason));
        QVERIFY2(resolution.singBoxSupported, qPrintable(resolution.singBoxReason));
        QCOMPARE(resolution.targetCore, 1 - sourceCore);

        const auto originalRouting = dataStore->routing->ToJson();
        for (int target: {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
            const auto context = CaptureBuildConfigContext(target);
            QCOMPARE(context.settings->routing->domain_strategy, dataStore->routing->domain_strategy);
            QCOMPARE(context.settings->routing->outbound_domain_strategy, dataStore->routing->outbound_domain_strategy);
            const auto config = BuildConfig(ent, false, true, context);
            QVERIFY2(config->error.isEmpty(), qPrintable(config->error));
            const auto proxy = outbound(config->coreConfig);
            QCOMPARE(proxy[target == CoreType::SING_BOX ? "type" : "protocol"].toString(), protocol);
            if (protocol == "shadowsocks") {
                const auto method = target == CoreType::SING_BOX ? proxy["method"].toString()
                    : proxy["settings"].toObject()["servers"].toArray().first().toObject()["method"].toString();
                QCOMPARE(method, QString("rc4-md5"));
            } else if (target == CoreType::SING_BOX) {
                QVERIFY(proxy["tls"].toObject()["insecure"].toBool());
            } else {
                QVERIFY(proxy["streamSettings"].toObject()["tlsSettings"].toObject()["allowInsecure"].toBool());
            }
            QCOMPARE(coreType, sourceCore);
            QCOMPARE(dataStore->routing->ToJson(), originalRouting);

            // Optional integration fixture export for exercising the exact
            // GUI output through each real core's side-effect-free RPC Check.
            const auto fixtureDirectory = qEnvironmentVariable("NEKORAY_TEST_CONFIG_DIR");
            if (!fixtureDirectory.isEmpty()) {
                QVERIFY(QDir().mkpath(fixtureDirectory));
                for (bool probe: {false, true}) {
                    auto runtimeContext = context;
                    runtimeContext.externalTun = true;
                    runtimeContext.tunProxyPort = 31919;
                    const auto fixture = BuildConfig(ent, probe, false, runtimeContext);
                    QVERIFY2(fixture->error.isEmpty(), qPrintable(fixture->error));
                    const auto name = QString("source-%1-target-%2-%3-%4.json").arg(sourceCore).arg(target)
                        .arg(protocol, probe ? "probe" : "runtime");
                    QFile output(QDir(fixtureDirectory).filePath(name));
                    QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
                    const auto json = QJsonDocument(fixture->coreConfig).toJson();
                    QCOMPARE(output.write(json), qint64(json.size()));
                }
            }
        }
    }

    void testFingerprintSurvivesListenerAndTunAllocationChanges() {
        auto ent = node("vless");
        for (int target: {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
            auto first = CaptureBuildConfigContext(target, true);
            first.tunProxyPort = 31919;
            first.tunBypassProcessPaths = QStringList{"C:/old-location/nekoray_core.exe"};
            auto second = CaptureBuildConfigContext(target, true);
            second.tunProxyPort = 32929;
            second.tunBypassProcessPaths = QStringList{"C:/new-location/nekoray_core.exe"};
            second.settings->inbound_address = "127.0.0.2";
            second.settings->inbound_socks_port = 2180;
            second.settings->inbound_http_port = 2181;
            second.settings->inbound_auth->username = "synthetic-user";
            second.settings->inbound_auth->password = "synthetic-password";
            second.settings->fake_dns = true;
            second.settings->vpn_mtu = 1500;
            second.settings->vpn_ipv6 = true;
            second.settings->vpn_rule_process = "synthetic-process.exe";
            second.settings->vpn_rule_cidr = "192.0.2.0/24";
            second.settings->core_box_clash_api = 39090;
            QVERIFY(first.Fingerprint() != second.Fingerprint());
            QCOMPARE(first.Fingerprint(true), second.Fingerprint(true));
            const auto firstConfig = BuildConfig(ent, true, false, first);
            const auto secondConfig = BuildConfig(ent, true, false, second);
            QVERIFY2(firstConfig->error.isEmpty(), qPrintable(firstConfig->error));
            QVERIFY2(secondConfig->error.isEmpty(), qPrintable(secondConfig->error));
            QCOMPARE(firstConfig->Fingerprint(), secondConfig->Fingerprint());
            second.settings->spmode_vpn = true;
            QVERIFY(first.Fingerprint(true) != second.Fingerprint(true));
            second.settings->spmode_vpn = false;
            second.settings->routing->direct_dns = "192.0.2.53";
            QVERIFY(first.Fingerprint(true) != second.Fingerprint(true));
        }
    }

    void mixedOnlySystemProxyKeepsExistingBoxBehavior() {
        coreType = CoreType::SING_BOX;
        dataStore->routing = std::make_unique<Routing>();
        dataStore->inbound_http_port = -1;
        dataStore->spmode_system_proxy = true;
        const auto ent = node("http");
        const auto resolution = ResolveCore(ent, CoreType::V2RAY);
        QVERIFY(!resolution.xraySupported);
        QVERIFY(resolution.singBoxSupported);
        QCOMPARE(resolution.targetCore, int(CoreType::SING_BOX));
        const auto box = BuildConfig(ent, false, true, CaptureBuildConfigContext(CoreType::SING_BOX));
        QVERIFY2(box->error.isEmpty(), qPrintable(box->error));
        QCOMPARE(inbound(box->coreConfig, "mixed-in")["listen_port"].toInt(), dataStore->inbound_socks_port);
        QVERIFY(inbound(box->coreConfig, "http-in").isEmpty());
        QVERIFY(!BuildConfig(ent, false, true, CaptureBuildConfigContext(CoreType::V2RAY))->error.isEmpty());
        QVERIFY(BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::V2RAY))->error.isEmpty());
        dataStore->spmode_system_proxy = false;
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).xraySupported);
        dataStore->spmode_system_proxy = true;
        dataStore->inbound_http_port = 2081;
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).xraySupported);
        dataStore->inbound_http_port = dataStore->inbound_socks_port;
        QVERIFY(!ResolveCore(ent, CoreType::V2RAY).xraySupported);
        QVERIFY(ResolveCore(ent, CoreType::SING_BOX).singBoxSupported);
        QVERIFY(BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::V2RAY))->error.isEmpty());
    }

    void runtimeBuildCapabilitiesRestrictPreferencesWithoutChangingThem() {
        auto ent = node("vless");
        auto stream = GetStreamSettings(ent->bean.get());
        stream->security = "tls";
        stream->network = "ws";
        stream->utlsFingerprint = "chrome";
        stream->allow_insecure = true;
        ent->core_preference = 2;
        QStringList features{"rpc.check", "outbound.vless", "transport.ws", "tls", "tls.insecure"};
        for (int target: {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
            const auto built = BuildConfig(ent, true, false, CaptureBuildConfigContext(target));
            QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
            QVERIFY(CoreBuildFeaturesError(built->coreConfig, target, features).contains("uTLS"));
            QVERIFY(CoreBuildFeaturesError(built->coreConfig, target, features + QStringList{"tls.utls"}).isEmpty());
        }
        SetCoreRuntimeFeatures(CoreType::SING_BOX, features);
        auto resolution = ResolveCore(ent, CoreType::SING_BOX);
        QCOMPARE(resolution.targetCore, int(CoreType::V2RAY));
        QVERIFY(resolution.preferenceFallback);
        QCOMPARE(ent->core_preference, 2);
        QVERIFY(resolution.singBoxReason.contains("uTLS"));
        SetCoreRuntimeFeatures(CoreType::SING_BOX, features + QStringList{"tls.utls"});
        QCOMPARE(ResolveCore(ent, CoreType::V2RAY).targetCore, int(CoreType::SING_BOX));
        ClearCoreRuntimeFeatures(CoreType::SING_BOX);
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).singBoxSupported);
    }

    void runtimeBuildCapabilitiesCoverChainsAndFrontProxies() {
        const auto ent = node("vless");
        const auto hy = node("hysteria2");
        const auto chain = node("chain");
        chain->ChainBean()->list = {ent->id, hy->id};
        QStringList features{"rpc.check", "outbound.vless", "tls"};
        SetCoreRuntimeFeatures(CoreType::SING_BOX, features);
        QCOMPARE(ResolveCore(chain, CoreType::SING_BOX).targetCore, -1);
        profileManager->groups[1]->front_proxy_id = hy->id;
        QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, -1);
        SetCoreRuntimeFeatures(CoreType::SING_BOX, features + QStringList{"outbound.hysteria2"});
        QCOMPARE(ResolveCore(ent, CoreType::SING_BOX).targetCore, int(CoreType::SING_BOX));
        profileManager->groups[1]->front_proxy_id = -1;
        QCOMPARE(ResolveCore(chain, CoreType::SING_BOX).targetCore, int(CoreType::SING_BOX));
    }

    void finalConfigCapabilitiesIncludeDnsTunLegacyWireGuardAndSs() {
        const QJsonObject box{
            {"outbounds", QJsonArray{QJsonObject{{"type", "socks"}}}},
            {"endpoints", QJsonArray{QJsonObject{{"type", "wireguard"}}}},
            {"inbounds", QJsonArray{QJsonObject{{"type", "tun"}}}},
            {"dns", QJsonObject{{"servers", QJsonArray{QJsonObject{{"address", "quic://192.0.2.1"}},
                                                         QJsonObject{{"type", "h3"}, {"server", "192.0.2.2"}}}}}}};
        const QStringList features{"rpc.check", "outbound.socks", "endpoint.wireguard", "tun", "dns.quic", "dns.http3"};
        QVERIFY(CoreBuildFeaturesError(box, CoreType::SING_BOX, features).isEmpty());
        for (const auto &feature: features) {
            auto withoutFeature = features;
            withoutFeature.removeAll(feature);
            QVERIFY2(!CoreBuildFeaturesError(box, CoreType::SING_BOX, withoutFeature).isEmpty(), qPrintable(feature));
        }
        const QJsonObject legacyWireGuard{{"outbounds", QJsonArray{QJsonObject{{"type", "wireguard"}}}}};
        QVERIFY(CoreBuildFeaturesError(legacyWireGuard, CoreType::SING_BOX, {"rpc.check", "endpoint.wireguard"}).isEmpty());
        QVERIFY(!CoreBuildFeaturesError(legacyWireGuard, CoreType::SING_BOX, {"rpc.check"}).isEmpty());

        auto ss = node("shadowsocks");
        ss->ShadowSocksBean()->method = "rc4-md5";
        const auto ray = BuildConfig(ss, true, false, CaptureBuildConfigContext(CoreType::V2RAY));
        QVERIFY2(ray->error.isEmpty(), qPrintable(ray->error));
        const QStringList rayFeatures{"rpc.check", "outbound.shadowsocks", "transport.tcp", "shadowsocks.rc4-md5"};
        QVERIFY(CoreBuildFeaturesError(ray->coreConfig, CoreType::V2RAY, rayFeatures).isEmpty());
        QVERIFY(CoreBuildFeaturesError(ray->coreConfig, CoreType::V2RAY, {"rpc.check", "outbound.shadowsocks", "transport.tcp"}).contains("rc4-md5"));
    }

    void rawConfigOwnershipAndDnsStrategy() {
        auto ent = node("vless");
        ent->bean->custom_outbound = "{\"streamSettings\":{\"sockopt\":{\"tcpFastOpen\":true}}}";
        QVERIFY(!ResolveCore(ent, CoreType::SING_BOX).singBoxSupported);
        ent->bean->custom_outbound.clear();
        dataStore->routing->direct_dns_strategy = "use_ip4";
        dataStore->routing->outbound_domain_strategy = "UseIPv6";
        auto context = CaptureBuildConfigContext(CoreType::SING_BOX);
        auto generated = BuildConfig(ent, false, true, context);
        QVERIFY2(generated->error.isEmpty(), qPrintable(generated->error));
        QCOMPARE(outbound(generated->coreConfig)["domain_strategy"].toString(), QString("ipv6_only"));
        bool found = false;
        for (const auto &server: generated->coreConfig["dns"].toObject()["servers"].toArray()) {
            if (server.toObject()["tag"] == "dns-direct") {
                QCOMPARE(server.toObject()["strategy"].toString(), QString("ipv4_only"));
                found = true;
            }
        }
        QVERIFY(found);
        generated = BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::V2RAY));
        QCOMPARE(outbound(generated->coreConfig)["streamSettings"].toObject()["sockopt"].toObject()["domainStrategy"].toString(), QString("UseIPv6"));
        dataStore->routing->outbound_domain_strategy = "PreferIPv4";
        QVERIFY(!ResolveCore(ent, CoreType::V2RAY).xraySupported);
    }

    void domainNodeProbeKeepsItsConfiguredDnsResolver() {
        const auto ent = node("http");
        dataStore->routing->direct_dns = "192.0.2.53";
        dataStore->routing->direct_dns_strategy = "use_ip4";
        dataStore->core_box_underlying_dns = "192.0.2.54";
        for (bool probe: {false, true}) {
            const auto built = BuildConfig(ent, probe, false, CaptureBuildConfigContext(CoreType::SING_BOX));
            QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
            const auto dns = built->coreConfig["dns"].toObject();
            QSet<QString> servers;
            for (const auto &entry: dns["servers"].toArray()) {
                const auto server = entry.toObject();
                servers.insert(server["tag"].toString());
                if (server["tag"] == "dns-direct") {
                    QCOMPARE(server["address"].toString(), QString("192.0.2.53"));
                    QCOMPARE(server["strategy"].toString(), QString("ipv4_only"));
                }
            }
            QVERIFY(servers.contains("dns-direct"));
            const auto nodeResolver = outbound(built->coreConfig)["domain_resolver"].toObject();
            QCOMPARE(nodeResolver["server"].toString(), QString("dns-direct"));
            QCOMPARE(nodeResolver["strategy"].toString(), QString("ipv4_only"));
            for (const auto &entry: dns["rules"].toArray()) {
                const auto resolver = entry.toObject()["server"].toString();
                QVERIFY2(resolver.isEmpty() || servers.contains(resolver), qPrintable(resolver));
            }
        }
        dataStore->routing->outbound_domain_strategy = "UseIPv6";
        const auto explicitStrategy = BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::SING_BOX));
        QVERIFY2(explicitStrategy->error.isEmpty(), qPrintable(explicitStrategy->error));
        QCOMPARE(outbound(explicitStrategy->coreConfig)["domain_resolver"].toObject()["strategy"].toString(), QString("ipv6_only"));

        coreType = CoreType::SING_BOX;
        dataStore->routing->use_dns_object = true;
        dataStore->routing->dns_object = "{\"servers\":[{\"type\":\"local\",\"tag\":\"custom-dns\"}]}";
        const auto customDns = BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::SING_BOX));
        QVERIFY2(customDns->error.isEmpty(), qPrintable(customDns->error));
        QVERIFY(!outbound(customDns->coreConfig).contains("domain_resolver"));
    }

    void boxBootstrapDnsOverrideIsNotDroppedWhenSelectingXray() {
        coreType = CoreType::SING_BOX;
        dataStore->routing = std::make_unique<Routing>();
        dataStore->core_box_underlying_dns = "192.0.2.53";
        const auto ent = node("http");
        const auto resolution = ResolveCore(ent, CoreType::V2RAY);
        QVERIFY(!resolution.xraySupported);
        QVERIFY(resolution.xrayReason.contains("DNS"));
        QCOMPARE(resolution.targetCore, int(CoreType::SING_BOX));
        QVERIFY(BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::SING_BOX))->error.isEmpty());
        QVERIFY(!BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::V2RAY))->error.isEmpty());
        QCOMPARE(dataStore->core_box_underlying_dns, QString("192.0.2.53"));
        dataStore->core_box_underlying_dns.clear();
        QVERIFY(ResolveCore(ent, CoreType::V2RAY).xraySupported);
    }

    void xrayPreservesUdpDnsPortsAndLiteralAddresses() {
        const auto ent = node("http");
        dataStore->routing->remote_dns = "udp://127.0.0.1:5301";
        dataStore->routing->direct_dns = "udp://[2001:db8::53]:5302";
        dataStore->routing->dns_final_out = "proxy";
        auto built = BuildConfig(ent, false, false, CaptureBuildConfigContext(CoreType::V2RAY));
        QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
        const auto servers = built->coreConfig["dns"].toObject()["servers"].toArray();
        QCOMPARE(servers[0].toObject()["address"].toString(), QString("127.0.0.1"));
        QCOMPARE(servers[0].toObject()["port"].toInt(), 5301);
        QCOMPARE(servers[1].toObject()["address"].toString(), QString("2001:db8::53"));
        QCOMPARE(servers[1].toObject()["port"].toInt(), 5302);
        bool literalRoute = false;
        for (const auto &entry: built->coreConfig["routing"].toObject()["rules"].toArray()) {
            const auto rule = entry.toObject();
            literalRoute |= rule["outboundTag"] == "direct" && rule["ip"].toArray().contains("2001:db8::53");
        }
        QVERIFY(literalRoute);
        QCOMPARE(dataStore->routing->direct_dns, QString("udp://[2001:db8::53]:5302"));
        dataStore->routing->direct_dns = "127.0.0.1:5303";
        built = BuildConfig(ent, true, false, CaptureBuildConfigContext(CoreType::V2RAY));
        const auto direct = built->coreConfig["dns"].toObject()["servers"].toArray().first().toObject();
        QCOMPARE(direct["address"].toString(), QString("127.0.0.1"));
        QCOMPARE(direct["port"].toInt(), 5303);
    }

    void tunPreservesPortsDnsRulesAuthAndFakeIp() {
        auto ent = node("vless");
        dataStore->spmode_vpn = true;
        dataStore->inbound_auth->username = "synthetic-user";
        dataStore->inbound_auth->password = "synthetic-password";
        dataStore->routing->remote_dns = "https://192.0.2.42/dns-query";
        dataStore->routing->direct_dns = "192.0.2.43";
        dataStore->routing->block_domain = "regexp:^CaseSensitive\\.invalid$";
        dataStore->vpn_rule_process = "synthetic.exe";
        dataStore->vpn_ipv6 = true;
        dataStore->fake_dns = true;
        auto context = CaptureBuildConfigContext(CoreType::SING_BOX, true);
        context.tunProxyPort = 31919;
        const auto helper = BuildVPNSingBoxConfig(context);
        QVERIFY2(helper->error.isEmpty(), qPrintable(helper->error));
        const auto tun = inbound(helper->coreConfig, "tun-in");
        QVERIFY(tun.contains("inet6_address"));
        QVERIFY(helper->coreConfig["dns"].toObject()["fakeip"].toObject()["enabled"].toBool());
        QCOMPARE(helper->coreConfig["dns"].toObject()["servers"].toArray().first().toObject()["address"].toString(), dataStore->routing->remote_dns);
        QCOMPARE(outbound(helper->coreConfig)["server_port"].toInt(), context.tunProxyPort);
        QCOMPARE(outbound(helper->coreConfig)["password"].toString(), dataStore->inbound_auth->password);
        QVERIFY(QJsonDocument(helper->coreConfig).toJson().contains("CaseSensitive"));
        QVERIFY(QJsonDocument(helper->coreConfig).toJson().contains("synthetic.exe"));

        for (int target: {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
            context.targetCore = target;
            const auto config = BuildConfig(ent, false, false, context);
            QVERIFY2(config->error.isEmpty(), qPrintable(config->error));
            const auto privateInbound = inbound(config->coreConfig, "tun-proxy-in");
            QCOMPARE(privateInbound[target == CoreType::SING_BOX ? "listen_port" : "port"].toInt(), context.tunProxyPort);
            QCOMPARE(inbound(config->coreConfig, "http-in")[target == CoreType::SING_BOX ? "listen_port" : "port"].toInt(), dataStore->inbound_http_port);
            QVERIFY(inbound(config->coreConfig, "tun-in").isEmpty());
        }
        dataStore->routing->use_dns_object = true;
        QVERIFY(!BuildVPNSingBoxConfig(CaptureBuildConfigContext(CoreType::SING_BOX, true))->error.isEmpty());
    }

    void tunDnsRespectsExplicitScopeBeforeHijack() {
        dataStore->spmode_vpn = true;
        dataStore->vpn_internal_tun = false;
        dataStore->vpn_rule_cidr = "203.0.113.0/24";
        dataStore->vpn_rule_process.clear();
        dataStore->fake_dns = false;
        for (bool whitelist: {false, true}) {
            dataStore->vpn_rule_white = whitelist;
            auto context = CaptureBuildConfigContext(CoreType::SING_BOX, true);
            context.tunProxyPort = 31919;
            const auto built = BuildVPNSingBoxConfig(context);
            QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
            const auto rules = built->coreConfig["route"].toObject()["rules"].toArray();
            int scope = -1, hijack = -1;
            for (int i = 0; i < rules.size(); ++i) {
                const auto rule = rules[i].toObject();
                if (rule["outbound"] == "bypass" && (rule.contains("ip_cidr") || rule["type"] == "logical")) scope = i;
                if (hijack < 0 && rule["outbound"] == "dns-out") hijack = i;
            }
            QVERIFY(scope >= 0 && hijack > scope);
        }
        dataStore->vpn_rule_cidr.clear();
        dataStore->fake_dns = true;
        auto context = CaptureBuildConfigContext(CoreType::SING_BOX, true);
        context.tunProxyPort = 31919;
        const auto bypassAll = BuildVPNSingBoxConfig(context);
        QVERIFY2(bypassAll->error.isEmpty(), qPrintable(bypassAll->error));
        const auto route = bypassAll->coreConfig["route"].toObject();
        QCOMPARE(route["final"].toString(), QString("bypass"));
        for (const auto &entry: route["rules"].toArray()) QVERIFY(entry.toObject()["outbound"] != "dns-out");
    }

    void tunFakeIpRejectsUnresolvedCidrScope_data() {
        QTest::addColumn<int>("sourceCore");
        QTest::addColumn<bool>("whitelist");
        QTest::addColumn<bool>("mixedProcesses");
        QTest::addColumn<QString>("strategy");
        for (int source: {int(CoreType::V2RAY), int(CoreType::SING_BOX)}) {
            for (bool whitelist: {false, true}) {
                for (bool mixed: {false, true}) {
                    for (const auto &strategy: {QString("AsIs"), QString()}) {
                        const auto name = QString("%1-%2-%3-%4").arg(source)
                            .arg(whitelist ? "whitelist" : "blacklist")
                            .arg(mixed ? "mixed" : "cidr-only").arg(strategy.isEmpty() ? "empty" : strategy).toLatin1();
                        QTest::newRow(name.constData()) << source << whitelist << mixed << strategy;
                    }
                }
            }
        }
        QTest::newRow("box-native-as-is") << int(CoreType::SING_BOX) << true << false << QString("as_is");
    }

    void tunFakeIpRejectsUnresolvedCidrScope() {
        QFETCH(int, sourceCore);
        QFETCH(bool, whitelist);
        QFETCH(bool, mixedProcesses);
        QFETCH(QString, strategy);
        coreType = sourceCore;
        dataStore->spmode_vpn = true;
        // The legacy internal flag never exempts Xray's external helper.
        dataStore->vpn_internal_tun = sourceCore == CoreType::V2RAY;
        dataStore->fake_dns = true;
        dataStore->vpn_rule_white = whitelist;
        dataStore->vpn_rule_cidr = "0.0.0.0/0\n::/0";
        dataStore->vpn_rule_process = mixedProcesses ? "synthetic.exe" : "";
        dataStore->routing->domain_strategy = strategy;
        const auto savedSettings = dataStore->ToJson();
        const auto savedRouting = dataStore->routing->ToJson();
        auto context = CaptureBuildConfigContext(CoreType::SING_BOX, true);
        context.tunProxyPort = 31919;
        const auto capturedSettings = context.settings->ToJson();
        const auto capturedRouting = context.settings->routing->ToJson();

        const auto built = BuildVPNSingBoxConfig(context);
        QVERIFY(built->error.contains("FakeIP"));
        QVERIFY(built->error.contains(QString::fromUtf8("IP 范围")));
        QVERIFY(built->error.contains(QString::fromUtf8("进程范围")));
        QVERIFY(built->coreConfig.isEmpty());
        QCOMPARE(dataStore->ToJson(), savedSettings);
        QCOMPARE(dataStore->routing->ToJson(), savedRouting);
        QCOMPARE(context.settings->ToJson(), capturedSettings);
        QCOMPARE(context.settings->routing->ToJson(), capturedRouting);
        QCOMPARE(dataStore->routing->domain_strategy, strategy);
        QCOMPARE(context.settings->routing->domain_strategy, strategy);
        QVERIFY(dataStore->spmode_vpn && context.settings->spmode_vpn);
    }

    void tunFakeIpKeepsSupportedScopeSettings() {
        auto build = [&] {
            auto context = CaptureBuildConfigContext(CoreType::SING_BOX, true);
            context.tunProxyPort = 31919;
            return BuildVPNSingBoxConfig(context);
        };
        dataStore->spmode_vpn = true;
        dataStore->vpn_internal_tun = false;
        dataStore->vpn_rule_process = "synthetic.exe";
        for (bool whitelist: {false, true}) {
            dataStore->vpn_rule_white = whitelist;
            dataStore->fake_dns = false;
            dataStore->vpn_rule_cidr = "203.0.113.0/24\n2001:db8::/32";
            auto built = build();
            QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
            QVERIFY(!built->coreConfig["dns"].toObject().contains("fakeip"));

            dataStore->fake_dns = true;
            dataStore->vpn_rule_cidr = "  # No IP ranges\n";
            const auto savedSettings = dataStore->ToJson();
            const auto savedRouting = dataStore->routing->ToJson();
            built = build();
            QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
            QVERIFY(built->coreConfig["dns"].toObject()["fakeip"].toObject()["enabled"].toBool());
            QVERIFY(inbound(built->coreConfig, "tun-in")["domain_strategy"].toString().isEmpty());
            for (const auto &entry: built->coreConfig["route"].toObject()["rules"].toArray())
                QVERIFY(entry.toObject()["action"] != "resolve");
            QCOMPARE(dataStore->ToJson(), savedSettings);
            QCOMPARE(dataStore->routing->ToJson(), savedRouting);
        }

        coreType = CoreType::SING_BOX;
        dataStore->vpn_rule_cidr = "203.0.113.0/24";
        dataStore->routing->domain_strategy = "prefer_ipv4";
        auto built = build();
        QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
        QCOMPARE(inbound(built->coreConfig, "tun-in")["domain_strategy"].toString(), QString("prefer_ipv4"));
        QCOMPARE(dataStore->routing->domain_strategy, QString("prefer_ipv4"));

        // Preserve the old internal-TUN rule ordering and interpretation.
        dataStore->vpn_internal_tun = true;
        dataStore->routing->domain_strategy = "AsIs";
        built = build();
        QVERIFY2(built->error.isEmpty(), qPrintable(built->error));
    }

    void externalFingerprintUsesContentInsteadOfTemporaryNames() {
        auto make = [](int port, const QString &suffix) {
            auto result = std::make_shared<BuildConfigResult>();
            result->targetCore = CoreType::V2RAY;
            result->generatedLocalPorts[port] = "external-0-0-socks";
            result->coreConfig = QJsonObject{{"outbounds", QJsonArray{QJsonObject{{"server_port", port}}}}};
            auto external = std::make_shared<ExternalBuildResult>();
            external->program = "synthetic-helper.exe";
            const auto cert = "/temporary/certificate-" + suffix + ".crt";
            const auto path = "/temporary/config-" + suffix + ".json";
            external->generatedFiles[cert] = "synthetic-certificate";
            external->config_export = QJsonObject2QString(QJsonObject{{"listen", "127.0.0.1:" + QString::number(port)}, {"certificate", cert}}, false);
            external->generatedFiles[path] = external->config_export.toUtf8();
            external->arguments = QStringList{"-c", path};
            result->extRs.push_back(external);
            return result;
        };
        const auto first = make(31001, "one");
        const auto second = make(32002, "two");
        QCOMPARE(first->Fingerprint(), second->Fingerprint());
        second->extRs.front()->config_export += "changed-effective-config";
        QVERIFY(first->Fingerprint() != second->Fingerprint());
    }
};

QTEST_GUILESS_MAIN(CoreCapabilitiesTest)
#include "core_capabilities_test.moc"
