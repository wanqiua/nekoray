#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <future>

#include "db/ConfigBuilder.hpp"
#include "db/CoreCapabilities.hpp"
#include "db/Database.hpp"
#include "fmt/includes.h"

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
