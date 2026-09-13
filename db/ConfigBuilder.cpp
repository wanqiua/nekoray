#include "db/ConfigBuilder.hpp"
#include "db/Database.hpp"
#include "db/CoreCapabilities.hpp"
#include "fmt/includes.h"
#include "fmt/Preset.hpp"
#include "main/QJS.hpp"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QJsonDocument>

#define BOX_UNDERLYING_DNS dataStore->core_box_underlying_dns.isEmpty() ? "underlying://0.0.0.0" : dataStore->core_box_underlying_dns
#define BOX_UNDERLYING_DNS_EXPORT dataStore->core_box_underlying_dns.isEmpty() ? (status->forExport ? "local" : "underlying://0.0.0.0") : dataStore->core_box_underlying_dns

namespace NekoGui {

    namespace {
        QJsonObject ExactStoreJson(JsonStore *store) {
            auto json = store->ToJson();
            // ToJson omits empty strings; applying that to a new object would
            // accidentally resurrect a nonempty default in the snapshot.
            for (const auto &item: store->_map) {
                if (item->type == itemType::string) json[item->name] = *static_cast<QString *>(item->ptr);
                if (item->type == itemType::jsonStore) json[item->name] = ExactStoreJson(static_cast<JsonStore *>(item->ptr));
            }
            return json;
        }

        std::shared_ptr<DataStore> CopySettings(DataStore *source) {
            auto result = std::shared_ptr<DataStore>(new DataStore, [](DataStore *snapshot) {
                delete snapshot->inbound_auth;
                delete snapshot->extraCore;
                delete snapshot;
            });
            result->FromJson(ExactStoreJson(source));
            result->save_control_no_save = true;
            result->routing = std::make_unique<Routing>();
            if (source->routing != nullptr) result->routing->FromJson(ExactStoreJson(source->routing.get()));
            result->routing->save_control_no_save = true;
            result->inbound_auth->save_control_no_save = true;
            result->extraCore->save_control_no_save = true;
            result->spmode_vpn = source->spmode_vpn;
            result->spmode_system_proxy = source->spmode_system_proxy;
            result->appdataDir = source->appdataDir;
            return result;
        }

        QString ConvertStrategies(DataStore *settings, int targetCore) {
            const bool box = targetCore == CoreType::SING_BOX;
            auto dns = [&](QString &value) {
                if (QStringList{"", "use_ip", "UseIP"}.contains(value)) value = "";
                else if (QStringList{"use_ip4", "UseIPv4", "ipv4_only"}.contains(value)) value = box ? "ipv4_only" : "use_ip4";
                else if (QStringList{"use_ip6", "UseIPv6", "ipv6_only"}.contains(value)) value = box ? "ipv6_only" : "use_ip6";
                else if (!box || !QStringList{"prefer_ipv4", "prefer_ipv6"}.contains(value)) return false;
                return true;
            };
            if (!dns(settings->routing->direct_dns_strategy) || !dns(settings->routing->remote_dns_strategy)) {
                return QObject::tr("无法保留当前 DNS 地址策略");
            }
            auto &outbound = settings->routing->outbound_domain_strategy;
            if (outbound.isEmpty() || outbound == "AsIs") outbound = box ? "" : "AsIs";
            else if (outbound == "UseIP") outbound = box ? "" : "UseIP";
            else if (outbound == "UseIPv4" || outbound == "ipv4_only") outbound = box ? "ipv4_only" : "UseIPv4";
            else if (outbound == "UseIPv6" || outbound == "ipv6_only") outbound = box ? "ipv6_only" : "UseIPv6";
            else if (outbound == "PreferIPv4" || outbound == "prefer_ipv4") outbound = box ? "prefer_ipv4" : "PreferIPv4";
            else if (outbound == "PreferIPv6" || outbound == "prefer_ipv6") outbound = box ? "prefer_ipv6" : "PreferIPv6";
            else return QObject::tr("无法保留当前出站域名策略：%1").arg(outbound);
            auto &route = settings->routing->domain_strategy;
            if (route.isEmpty() || route == "AsIs") route = box ? "" : "AsIs";
            return {};
        }

        QJsonObject SingBoxRule(const QStringList &list, bool isIP = false) {
            QJsonObject rule;
            QMap<QString, QJsonArray> fields;
            for (const auto &entry: list) {
                if (isIP) {
                    if (entry.startsWith("geoip:")) fields["geoip"] += entry.mid(6);
                    else fields["ip_cidr"] += entry;
                } else if (entry.startsWith("geosite:")) fields["geosite"] += entry.mid(8);
                else if (entry.startsWith("full:")) fields["domain"] += entry.mid(5).toLower();
                else if (entry.startsWith("domain:")) fields["domain_suffix"] += entry.mid(7).toLower();
                else if (entry.startsWith("regexp:")) fields["domain_regex"] += entry.mid(7);
                else if (entry.startsWith("keyword:")) fields["domain_keyword"] += entry.mid(8);
                else fields["domain_keyword"] += entry;
            }
            for (auto it = fields.cbegin(); it != fields.cend(); ++it) if (!it->isEmpty()) rule[it.key()] = it.value();
            return rule;
        }

        QString FreedomStrategyForBox(const DataStore *settings, int sourceCore) {
            if (sourceCore != CoreType::V2RAY) return {};
            if (settings->core_ray_freedom_domainStrategy == "UseIPv4") return "ipv4_only";
            if (settings->core_ray_freedom_domainStrategy == "UseIPv6") return "ipv6_only";
            return {};
        }
    } // namespace

    BuildConfigContext CaptureBuildConfigContext(int targetCore, bool externalTun) {
        BuildConfigContext context;
        context.targetCore = targetCore;
        context.sourceCore = coreType;
        context.externalTun = externalTun;
        context.settings = CopySettings(dataStore);
        context.hookSource = qjs::ReadHookJS();
        context.geoipPath = FindCoreAsset("geoip.db");
        context.geositePath = FindCoreAsset("geosite.db");
#ifdef Q_OS_WIN
        context.tunBypassProcessPaths = QStringList{QApplication::applicationDirPath() + "/nekoray_core.exe",
                                                   QApplication::applicationDirPath() + "/nekobox_core.exe"};
#else
        context.tunBypassProcessPaths = QStringList{QApplication::applicationDirPath() + "/nekoray_core",
                                                   QApplication::applicationDirPath() + "/nekobox_core"};
#endif
        return context;
    }

    QByteArray BuildConfigContext::Fingerprint(bool forTest) const {
        if (!settings) return {};
        const auto saved = ExactStoreJson(settings.get());
        QJsonObject relevant;
        for (const auto &key: {"custom_inbound",
                               "mux_protocol", "mux_concurrency", "mux_padding", "mux_default_on", "skip_cert", "utlsFingerprint",
                               "custom_route", "extraCore", "core_box_underlying_dns", "enable_js_hook",
                               "core_ray_direct_dns", "core_ray_freedom_domainStrategy", "core_ray_windows_disable_auto_interface"}) {
            relevant[key] = saved[key];
        }
        // Probes do not create the public or private TUN listener. Allocated
        // ports and installation paths must not invalidate persisted results
        // after a restart, nor should unrelated listener/TUN preferences.
        if (!forTest) {
            for (const auto &key: {"inbound_address", "inbound_socks_port", "inbound_http_port", "inbound_auth",
                                   "fakedns", "vpn_internal_tun", "vpn_impl", "vpn_mtu", "vpn_ipv6", "vpn_strict_route",
                                   "vpn_bypass_process", "vpn_bypass_cidr", "vpn_rule_white",
                                   "core_box_clash_api", "core_box_clash_api_secret"}) {
                relevant[key] = saved[key];
            }
        }
        QJsonObject input{{"target", targetCore}, {"source", sourceCore}, {"settings", relevant},
                          {"routing", settings->routing ? ExactStoreJson(settings->routing.get()) : QJsonObject()},
                          {"vpn", settings->spmode_vpn}, {"external_tun", externalTun},
                          {"hook", QString::fromLatin1(QCryptographicHash::hash(hookSource, QCryptographicHash::Sha256).toHex())}};
        if (!forTest) {
            input["tun_proxy_port"] = tunProxyPort;
            input["tun_process_paths"] = QList2QJsonArray(tunBypassProcessPaths);
        }
        return QCryptographicHash::hash(QJsonDocument(input).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    }

    QByteArray BuildConfigResult::Fingerprint() const {
        QMap<QString, QByteArray> files;
        for (const auto &external: extRs) {
            for (auto it = external->generatedFiles.cbegin(); it != external->generatedFiles.cend(); ++it) files[it.key()] = it.value();
        }
        QMap<QString, QString> fileLabels;
        for (auto it = files.cbegin(); it != files.cend(); ++it) fileLabels[it.key()] = "$generated-file";
        auto normalizeString = [&](QString value) {
            for (auto it = fileLabels.cbegin(); it != fileLabels.cend(); ++it) value.replace(it.key(), it.value());
            for (auto it = generatedLocalPorts.cbegin(); it != generatedLocalPorts.cend(); ++it) {
                const auto number = QString::number(it.key());
                const auto marker = "$" + it.value();
                if (value == number) value = marker;
                value.replace("127.0.0.1:" + number, "127.0.0.1:" + marker);
                value.replace("localhost:" + number, "localhost:" + marker);
            }
            return value;
        };
        std::function<QJsonValue(const QJsonValue &, const QString &)> normalize;
        normalize = [&](const QJsonValue &value, const QString &key) -> QJsonValue {
            if (value.isObject()) {
                QJsonObject normalized;
                const auto object = value.toObject();
                for (auto it = object.begin(); it != object.end(); ++it) normalized[it.key()] = normalize(it.value(), it.key());
                return normalized;
            }
            if (value.isArray()) {
                QJsonArray normalized;
                for (const auto &entry: value.toArray()) normalized += normalize(entry, key);
                return normalized;
            }
            if (value.isDouble() && QStringList{"port", "listen_port", "server_port", "mapping_port", "socks_port"}.contains(key) &&
                generatedLocalPorts.contains(value.toInt())) return "$" + generatedLocalPorts[value.toInt()];
            if (value.isString()) return normalizeString(value.toString());
            return value;
        };
        auto normalizeText = [&](const QByteArray &value) {
            const auto doc = QJsonDocument::fromJson(value);
            if (doc.isObject()) return QJsonDocument(normalize(doc.object(), {}).toObject()).toJson(QJsonDocument::Compact);
            if (doc.isArray()) return QJsonDocument(normalize(doc.array(), {}).toArray()).toJson(QJsonDocument::Compact);
            return normalizeString(QString::fromUtf8(value)).toUtf8();
        };
        // A generated JSON file may refer to a generated certificate. Resolve
        // those content hashes without including either random temporary path.
        for (int pass = 0; pass <= files.size(); ++pass) {
            auto labels = fileLabels;
            for (auto it = files.cbegin(); it != files.cend(); ++it) {
                labels[it.key()] = "$generated-" + QString::fromLatin1(QCryptographicHash::hash(normalizeText(it.value()), QCryptographicHash::Sha256).toHex());
            }
            if (labels == fileLabels) break;
            fileLabels = labels;
        }
        QJsonArray externalConfigs;
        for (const auto &external: extRs) {
            QJsonArray hashes;
            for (auto it = external->generatedFiles.cbegin(); it != external->generatedFiles.cend(); ++it) hashes += fileLabels[it.key()];
            auto hashList = QJsonArray2QListString(hashes);
            hashList.sort();
            QJsonObject item{{"program", external->program}, {"arguments", QList2QJsonArray(external->arguments)},
                             {"environment", QList2QJsonArray(external->env)},
                             {"config", QString::fromUtf8(normalizeText(external->config_export.toUtf8()))},
                             {"files", QList2QJsonArray(hashList)}};
            externalConfigs += normalize(item, {});
        }
        const QJsonObject input{{"target", targetCore}, {"config", normalize(coreConfig, {})}, {"external", externalConfigs}};
        return QCryptographicHash::hash(QJsonDocument(input).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    }

    QStringList getAutoBypassExternalProcessPaths(const std::shared_ptr<BuildConfigResult> &result) {
        QStringList paths;
        for (const auto &extR: result->extRs) {
            auto path = extR->program;
            if (path.trimmed().isEmpty()) continue;
            paths << path.replace("\\", "/");
        }
        return paths;
    }

    QString genTunName() {
        auto tun_name = "nekoray-tun";
#ifdef Q_OS_MACOS
        tun_name = "utun9";
#endif
        return tun_name;
    }

    void MergeJson(const QJsonObject &custom, QJsonObject &outbound) {
        // 合并
        if (custom.isEmpty()) return;
        for (const auto &key: custom.keys()) {
            if (outbound.contains(key)) {
                auto v = custom[key];
                auto v_orig = outbound[key];
                if (v.isObject() && v_orig.isObject()) { // isObject 则合并？
                    auto vo = v.toObject();
                    QJsonObject vo_orig = v_orig.toObject();
                    MergeJson(vo, vo_orig);
                    outbound[key] = vo_orig;
                } else {
                    outbound[key] = v;
                }
            } else {
                outbound[key] = custom[key];
            }
        }
    }

    // Common

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport) {
        return BuildConfig(ent, forTest, forExport, CaptureBuildConfigContext(coreType));
    }

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport, int targetCore) {
        return BuildConfig(ent, forTest, forExport, CaptureBuildConfigContext(targetCore));
    }

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport,
                                                 const BuildConfigContext &context) {
        auto result = std::make_shared<BuildConfigResult>();
        result->targetCore = context.targetCore;
        if (!context.settings) {
            result->error = QObject::tr("缺少当前配置快照");
            return result;
        }
        result->error = CoreSettingsCompatibilityError(context.targetCore, context.sourceCore, context.settings.get(), forTest);
        if (!result->error.isEmpty()) return result;
        result->error = CoreCompatibilityError(ent, context.targetCore, context.sourceCore, context.settings.get());
        if (!result->error.isEmpty()) return result;
        if (context.sourceCore != context.targetCore && !context.hookSource.isEmpty()) {
            result->error = QObject::tr("hook.js 属于原内核，不能在切换时直接套用");
            return result;
        }
        auto status = std::make_shared<BuildConfigStatus>();
        status->ent = ent;
        status->result = result;
        status->forTest = forTest;
        status->forExport = forExport;
        status->context = context;
        status->context.settings = CopySettings(context.settings.get());
        result->error = ConvertStrategies(status->context.settings.get(), context.targetCore);
        if (!result->error.isEmpty()) return result;

        auto customBean = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
        if (customBean != nullptr && customBean->core == "internal-full") {
            result->coreConfig = QString2QJsonObject(customBean->config_simple);
        } else {
            if (status->IsSingBox()) {
                BuildConfigSingBox(status);
            } else {
                BuildConfigV2Ray(status);
            }
        }

        // apply custom config
        MergeJson(QString2QJsonObject(ent->bean->custom_config), result->coreConfig);

        // hook.js
        if (result->error.isEmpty() && !forTest) {
            const auto &source = context.hookSource;
            if (!source.isEmpty()) {
                qjs::QJS js(source);
                auto js_result = js.EvalFunction("hook.hook_core_config", QJsonObject2QString(result->coreConfig, true));
                auto js_result_json = QString2QJsonObject(js_result);
                if (!js_result_json.isEmpty() && result->coreConfig != js_result_json) {
                    MW_show_log("hook.js modified your " + CoreDisplayName(context.targetCore) + " json config.");
                    result->coreConfig = js_result_json;
                }
            }
        }
        if (result->error.isEmpty() && !forTest && !forExport && context.externalTun) {
            if (!IsValidPort(context.tunProxyPort)) {
                result->error = QObject::tr("TUN 专用代理端口尚未分配");
            } else {
                bool found = false;
                for (const auto &entry: result->coreConfig["inbounds"].toArray()) {
                    const auto inbound = entry.toObject();
                    const auto port = inbound[context.targetCore == CoreType::SING_BOX ? "listen_port" : "port"].toInt();
                    if (inbound["tag"] == "tun-proxy-in" && port == context.tunProxyPort && inbound["listen"] == "127.0.0.1") found = true;
                }
                if (!found) result->error = QObject::tr("自定义配置覆盖了 TUN 专用入站，不能安全切换");
            }
        }
        return result;
    }

    QString BuildChain(int chainId, const std::shared_ptr<BuildConfigStatus> &status) {
        auto group = profileManager->GetGroup(status->ent->gid);
        if (group == nullptr) {
            status->result->error = QString("This profile is not in any group, your data may be corrupted.");
            return {};
        }

        auto resolveChain = [=](const std::shared_ptr<ProxyEntity> &ent) {
            QList<std::shared_ptr<ProxyEntity>> resolved;
            if (ent->type == "chain") {
                auto list = ent->ChainBean()->list;
                std::reverse(std::begin(list), std::end(list));
                for (auto id: list) {
                    resolved += profileManager->GetProfile(id);
                    if (resolved.last() == nullptr) {
                        status->result->error = QString("chain missing ent: %1").arg(id);
                        break;
                    }
                    if (resolved.last()->type == "chain") {
                        status->result->error = QString("chain in chain is not allowed: %1").arg(id);
                        break;
                    }
                }
            } else {
                resolved += ent;
            };
            return resolved;
        };

        // Make list
        auto ents = resolveChain(status->ent);
        if (!status->result->error.isEmpty()) return {};

        if (group->front_proxy_id >= 0) {
            auto fEnt = profileManager->GetProfile(group->front_proxy_id);
            if (fEnt == nullptr) {
                status->result->error = QString("front proxy ent not found.");
                return {};
            }
            ents += resolveChain(fEnt);
            if (!status->result->error.isEmpty()) return {};
        }

        // BuildChain
        QString chainTagOut = BuildChainInternal(0, ents, status);

        // Chain ent traffic stat
        if (!status->forTest && ents.length() > 1) {
            status->ent->traffic_data->id = status->ent->id;
            status->ent->traffic_data->tag = chainTagOut.toStdString();
            status->result->outboundStats += status->ent->traffic_data;
        }

        return chainTagOut;
    }

#define DOMAIN_USER_RULE                                                             \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->proxy_domain)) {  \
        if (dataStore->routing->dns_routing) status->domainListDNSRemote += line;    \
        status->domainListRemote += line;                                            \
    }                                                                                \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->direct_domain)) { \
        if (dataStore->routing->dns_routing) status->domainListDNSDirect += line;    \
        status->domainListDirect += line;                                            \
    }                                                                                \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->block_domain)) {  \
        status->domainListBlock += line;                                             \
    }

#define IP_USER_RULE                                                             \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->block_ip)) {  \
        status->ipListBlock += line;                                             \
    }                                                                            \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->proxy_ip)) {  \
        status->ipListRemote += line;                                            \
    }                                                                            \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->direct_ip)) { \
        status->ipListDirect += line;                                            \
    }

    // V2Ray

    void BuildConfigV2Ray(const std::shared_ptr<BuildConfigStatus> &status) {
        const auto dataStore = status->context.settings.get();
        // Log
        auto logObj = QJsonObject{{"loglevel", dataStore->log_level}};
        status->result->coreConfig.insert("log", logObj);

        // Inbounds
        QJsonObject sniffing{
            {"destOverride", QJsonArray{"http", "tls", "quic"}},
            {"enabled", true},
            {"metadataOnly", false},
            {"routeOnly", dataStore->routing->sniffing_mode == SniffingMode::FOR_ROUTING},
        };

        // socks-in
        if (IsValidPort(dataStore->inbound_socks_port) && !status->forTest) {
            QJsonObject inboundObj;
            inboundObj["tag"] = "socks-in";
            inboundObj["protocol"] = "socks";
            inboundObj["listen"] = dataStore->inbound_address;
            inboundObj["port"] = dataStore->inbound_socks_port;
            QJsonObject socksSettings = {{"udp", true}};
            if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
                inboundObj["sniffing"] = sniffing;
            }
            if (dataStore->inbound_auth->NeedAuth()) {
                socksSettings["auth"] = "password";
                socksSettings["accounts"] = QJsonArray{
                    QJsonObject{
                        {"user", dataStore->inbound_auth->username},
                        {"pass", dataStore->inbound_auth->password},
                    },
                };
            }
            inboundObj["settings"] = socksSettings;
            status->inbounds += inboundObj;
        }
        // http-in
        if (IsValidPort(dataStore->inbound_http_port) && !status->forTest) {
            QJsonObject inboundObj;
            inboundObj["tag"] = "http-in";
            inboundObj["protocol"] = "http";
            inboundObj["listen"] = dataStore->inbound_address;
            inboundObj["port"] = dataStore->inbound_http_port;
            if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
                inboundObj["sniffing"] = sniffing;
            }
            if (dataStore->inbound_auth->NeedAuth()) {
                inboundObj["settings"] = QJsonObject{
                    {"accounts", QJsonArray{
                                     QJsonObject{
                                         {"user", dataStore->inbound_auth->username},
                                         {"pass", dataStore->inbound_auth->password},
                                     },
                                 }},
                };
            }
            status->inbounds += inboundObj;
        }

        // Outbounds
        auto tagProxy = BuildChain(0, status);
        if (!status->result->error.isEmpty()) return;

        // The TUN helper resolves/routes traffic once. A private inbound avoids
        // re-applying public SOCKS routing and undoing a process-specific rule.
        if (status->context.externalTun && !status->forTest && !status->forExport &&
            IsValidPort(status->context.tunProxyPort)) {
            QJsonObject privateSettings{{"udp", true}};
            if (dataStore->inbound_auth->NeedAuth()) {
                privateSettings["auth"] = "password";
                privateSettings["accounts"] = QJsonArray{QJsonObject{{"user", dataStore->inbound_auth->username},
                                                                     {"pass", dataStore->inbound_auth->password}}};
            }
            status->inbounds += QJsonObject{{"tag", "tun-proxy-in"}, {"protocol", "socks"},
                                            {"listen", "127.0.0.1"}, {"port", status->context.tunProxyPort},
                                            {"settings", privateSettings}};
            status->routingRules += QJsonObject{{"type", "field"}, {"inboundTag", QJsonArray{"tun-proxy-in"}},
                                                {"outboundTag", tagProxy}};
        }

        // direct & bypass & block
        status->outbounds += QJsonObject{
            {"protocol", "freedom"},
            {"settings", QJsonObject{{"domainStrategy", dataStore->core_ray_freedom_domainStrategy}}},
            {"tag", "direct"},
        };
        status->outbounds += QJsonObject{
            {"protocol", "freedom"},
            {"settings", QJsonObject{{"domainStrategy", dataStore->core_ray_freedom_domainStrategy}}},
            {"tag", "bypass"},
        };
        status->outbounds += QJsonObject{
            {"protocol", "blackhole"},
            {"tag", "block"},
        };

        // DNS out
        if (!status->forTest) {
            QJsonObject dnsOut;
            dnsOut["protocol"] = "dns";
            dnsOut["tag"] = "dns-out";
            QJsonObject dnsOut_settings;
            dnsOut_settings["network"] = "tcp";
            dnsOut_settings["port"] = 53;
            dnsOut_settings["address"] = "8.8.8.8";
            dnsOut_settings["userLevel"] = 1;
            dnsOut["settings"] = dnsOut_settings;
            dnsOut["proxySettings"] = QJsonObject{{"tag", tagProxy},
                                                  {"transportLayer", true}};

            status->outbounds += dnsOut;
            status->routingRules += QJsonObject{
                {"type", "field"},
                {"port", "53"},
                {"inboundTag", QJsonArray{"socks-in", "http-in"}},
                {"outboundTag", "dns-out"},
            };
        }

        // custom inbound
        if (!status->forTest) QJSONARRAY_ADD(status->inbounds, QString2QJsonObject(dataStore->custom_inbound)["inbounds"].toArray())

        status->result->coreConfig.insert("inbounds", status->inbounds);
        status->result->coreConfig.insert("outbounds", status->outbounds);

        // user rule
        if (!status->forTest) {
            DOMAIN_USER_RULE
            IP_USER_RULE
        }

        // final add DNS
        QJsonObject dns;
        QJsonArray dnsServers;

        // Remote or FakeDNS
        QJsonObject dnsServerRemote;
        dnsServerRemote["address"] = dataStore->routing->remote_dns;
        dnsServerRemote["domains"] = QList2QJsonArray<QString>(status->domainListDNSRemote);
        dnsServerRemote["queryStrategy"] = dataStore->routing->remote_dns_strategy;
        if (!status->forTest) dnsServers += dnsServerRemote;

        // Direct
        auto directDnsAddress = dataStore->routing->direct_dns;
        if (directDnsAddress.contains("://")) {
            auto directDnsIp = SubStrBefore(SubStrAfter(directDnsAddress, "://"), "/");
            if (IsIpAddress(directDnsIp)) {
                status->routingRules.push_front(QJsonObject{
                    {"type", "field"},
                    {"ip", QJsonArray{directDnsIp}},
                    {"outboundTag", "direct"},
                });
            } else {
                status->routingRules.push_front(QJsonObject{
                    {"type", "field"},
                    {"domain", QJsonArray{directDnsIp}},
                    {"outboundTag", "direct"},
                });
            }
        } else if (directDnsAddress != "localhost") {
            status->routingRules.push_front(QJsonObject{
                {"type", "field"},
                {"ip", QJsonArray{directDnsAddress}},
                {"outboundTag", "direct"},
            });
        }
        QJsonObject directObj{
            {"address", directDnsAddress.replace("https://", "https+local://")},
            {"queryStrategy", dataStore->routing->direct_dns_strategy},
            {"domains", QList2QJsonArray<QString>(status->domainListDNSDirect)},
        };
        if (dataStore->routing->dns_final_out == "bypass") {
            dnsServers.prepend(directObj);
        } else {
            dnsServers.append(directObj);
        }

        dns["disableFallback"] = true;
        dns["servers"] = dnsServers;
        dns["tag"] = "dns";

        if (dataStore->routing->use_dns_object) {
            dns = QString2QJsonObject(dataStore->routing->dns_object);
        }
        status->result->coreConfig.insert("dns", dns);

        // Routing
        QJsonObject routing;
        routing["domainStrategy"] = dataStore->routing->domain_strategy;
        if (status->forTest) routing["domainStrategy"] = "AsIs";

        // final add user rule (block)
        QJsonObject routingRule_tmp;
        routingRule_tmp["type"] = "field";
        routingRule_tmp["outboundTag"] = "block";
        if (!status->ipListBlock.isEmpty()) {
            auto tmp = routingRule_tmp;
            tmp["ip"] = QList2QJsonArray<QString>(status->ipListBlock);
            status->routingRules += tmp;
        }
        if (!status->domainListBlock.isEmpty()) {
            auto tmp = routingRule_tmp;
            tmp["domain"] = QList2QJsonArray<QString>(status->domainListBlock);
            status->routingRules += tmp;
        }

        // final add user rule (proxy)
        routingRule_tmp["outboundTag"] = "proxy";
        if (!status->ipListRemote.isEmpty()) {
            auto tmp = routingRule_tmp;
            tmp["ip"] = QList2QJsonArray<QString>(status->ipListRemote);
            status->routingRules += tmp;
        }
        if (!status->domainListRemote.isEmpty()) {
            auto tmp = routingRule_tmp;
            tmp["domain"] = QList2QJsonArray<QString>(status->domainListRemote);
            status->routingRules += tmp;
        }

        // final add user rule (bypass)
        routingRule_tmp["outboundTag"] = "bypass";
        if (!status->ipListDirect.isEmpty()) {
            auto tmp = routingRule_tmp;
            tmp["ip"] = QList2QJsonArray<QString>(status->ipListDirect);
            status->routingRules += tmp;
        }
        if (!status->domainListDirect.isEmpty()) {
            auto tmp = routingRule_tmp;
            tmp["domain"] = QList2QJsonArray<QString>(status->domainListDirect);
            status->routingRules += tmp;
        }

        // def_outbound
        if (!status->forTest) status->routingRules += QJsonObject{
                                  {"type", "field"},
                                  {"port", "0-65535"},
                                  {"outboundTag", dataStore->routing->def_outbound},
                              };

        // final add routing rule
        auto routingRules = QString2QJsonObject(dataStore->routing->custom)["rules"].toArray();
        if (status->forTest) routingRules = {};
        if (!status->forTest) QJSONARRAY_ADD(routingRules, QString2QJsonObject(dataStore->custom_route_global)["rules"].toArray())
        QJSONARRAY_ADD(routingRules, status->routingRules)
        if (!status->forTest && !status->forExport && status->context.externalTun && IsValidPort(status->context.tunProxyPort)) {
            routingRules.prepend(QJsonObject{{"type", "field"}, {"inboundTag", QJsonArray{"tun-proxy-in"}}, {"outboundTag", tagProxy}});
        }
        routing["rules"] = routingRules;
        status->result->coreConfig.insert("routing", routing);

        // Policy & stats
        QJsonObject policy;
        QJsonObject levels;
        QJsonObject level1;
        level1["connIdle"] = 30;
        levels["1"] = level1;
        policy["levels"] = levels;

        QJsonObject policySystem;
        policySystem["statsOutboundDownlink"] = true;
        policySystem["statsOutboundUplink"] = true;
        policy["system"] = policySystem;
        status->result->coreConfig.insert("policy", policy);
        status->result->coreConfig.insert("stats", QJsonObject());
    }

    QString BuildChainInternal(int chainId, const QList<std::shared_ptr<ProxyEntity>> &ents,
                               const std::shared_ptr<BuildConfigStatus> &status) {
        const auto dataStore = status->context.settings.get();
        QString chainTag = "c-" + Int2String(chainId);
        QString chainTagOut;
        bool muxApplied = false;

        QString pastTag;
        int pastExternalStat = 0;
        int index = 0;

        for (const auto &ent: ents) {
            status->result->error = CoreCompatibilityError(ent, status->context.targetCore, status->context.sourceCore, dataStore);
            if (!status->result->error.isEmpty()) return {};
            // tagOut: v2ray outbound tag for a profile
            // profile2 (in) (global)   tag g-(id)
            // profile1                 tag (chainTag)-(id)
            // profile0 (out)           tag (chainTag)-(id) / single: chainTag=g-(id)
            auto tagOut = chainTag + "-" + Int2String(ent->id);

            // needGlobal: can only contain one?
            bool needGlobal = false;

            // first profile set as global
            auto isFirstProfile = index == ents.length() - 1;
            if (isFirstProfile) {
                needGlobal = true;
                tagOut = "g-" + Int2String(ent->id);
            }

            // last profile set as "proxy"
            if (chainId == 0 && index == 0) {
                needGlobal = false;
                tagOut = "proxy";
            }

            // ignoreConnTag
            if (index != 0) {
                status->result->ignoreConnTag << tagOut;
            }

            if (needGlobal) {
                if (status->globalProfiles.contains(ent->id)) {
                    continue;
                }
                status->globalProfiles += ent->id;
            }

            if (index > 0) {
                // chain rules: past
                if (pastExternalStat == 0) {
                    auto replaced = status->outbounds.last().toObject();
                    if (status->IsSingBox()) {
                        replaced["detour"] = tagOut;
                    } else {
                        replaced["proxySettings"] = QJsonObject{
                            {"tag", tagOut},
                            {"transportLayer", true},
                        };
                    }
                    status->outbounds.removeLast();
                    status->outbounds += replaced;
                } else {
                    if (status->IsSingBox()) {
                        status->routingRules += QJsonObject{
                            {"inbound", QJsonArray{pastTag + "-mapping"}},
                            {"outbound", tagOut},
                        };
                    } else {
                        status->routingRules += QJsonObject{
                            {"type", "field"},
                            {"inboundTag", QJsonArray{pastTag + "-mapping"}},
                            {"outboundTag", tagOut},
                        };
                    }
                }
            } else {
                // index == 0 means last profile in chain / not chain
                chainTagOut = tagOut;
                status->result->outboundStat = ent->traffic_data;
            }

            // chain rules: this
            auto ext_mapping_port = 0;
            auto ext_socks_port = 0;
            auto thisExternalStat = ent->bean->NeedExternal(isFirstProfile, status->context.targetCore, dataStore);
            const auto externalQuic = dynamic_cast<NekoGui_fmt::QUICBean *>(ent->bean.get());
            if (thisExternalStat > 0 && !isFirstProfile && externalQuic != nullptr &&
                externalQuic->proxy_type != NekoGui_fmt::QUICBean::proxy_Hysteria) {
                status->result->error = QObject::tr("外部 Hysteria2/TUIC 的此适配器不能完整保留前置代理链，请使用原生 sing-box 模式");
                return {};
            }
            if (thisExternalStat < 0) {
                status->result->error = "This configuration cannot be set automatically, please try another.";
                return {};
            }

            // determine port
            if (thisExternalStat > 0) {
                if (ent->type == "custom") {
                    auto bean = ent->CustomBean();
                    if (IsValidPort(bean->mapping_port)) {
                        ext_mapping_port = bean->mapping_port;
                    } else {
                        ext_mapping_port = MkPort();
                    }
                    if (IsValidPort(bean->socks_port)) {
                        ext_socks_port = bean->socks_port;
                    } else {
                        ext_socks_port = MkPort();
                    }
                } else {
                    ext_mapping_port = MkPort();
                    ext_socks_port = MkPort();
                }
                // Only normalize ports allocated by the builder; explicit
                // custom ports remain part of the effective configuration.
                const auto custom = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
                const auto key = QString("external-%1-%2").arg(chainId).arg(index);
                if (!custom || !IsValidPort(custom->mapping_port)) status->result->generatedLocalPorts[ext_mapping_port] = key + "-mapping";
                if (!custom || !IsValidPort(custom->socks_port)) status->result->generatedLocalPorts[ext_socks_port] = key + "-socks";
            }
            if (!status->forTest && thisExternalStat == 2) status->result->needKeepVpnOff = true;
            if (thisExternalStat == 1) {
                // mapping
                if (status->IsSingBox()) {
                    status->inbounds += QJsonObject{
                        {"type", "direct"},
                        {"tag", tagOut + "-mapping"},
                        {"listen", "127.0.0.1"},
                        {"listen_port", ext_mapping_port},
                        {"override_address", ent->bean->serverAddress},
                        {"override_port", ent->bean->serverPort},
                    };
                } else {
                    status->inbounds += QJsonObject{
                        {"protocol", "dokodemo-door"},
                        {"tag", tagOut + "-mapping"},
                        {"listen", "127.0.0.1"},
                        {"port", ext_mapping_port},
                        {"settings", QJsonObject{
                                         // to
                                         {"address", ent->bean->serverAddress},
                                         {"port", ent->bean->serverPort},
                                         {"network", "tcp,udp"},
                                     }},
                    };
                }
                // no chain rule and not outbound, so need to set to direct
                if (isFirstProfile) {
                    if (status->IsSingBox()) {
                        status->routingRules += QJsonObject{
                            {"inbound", QJsonArray{tagOut + "-mapping"}},
                            {"outbound", "direct"},
                        };
                    } else {
                        status->routingRules += QJsonObject{
                            {"type", "field"},
                            {"inboundTag", QJsonArray{tagOut + "-mapping"}},
                            {"outboundTag", "direct"},
                        };
                    }
                }
            }

            // Outbound

            QJsonObject outbound;
            auto stream = GetStreamSettings(ent->bean.get());

            if (thisExternalStat > 0) {
                auto extR = ent->bean->BuildExternal(ext_mapping_port, ext_socks_port, thisExternalStat, dataStore);
                if (extR.program.isEmpty()) {
                    status->result->error = QObject::tr("Core not found: %1").arg(ent->bean->DisplayCoreType());
                    return {};
                }
                if (!extR.error.isEmpty()) { // rejected
                    status->result->error = extR.error;
                    return {};
                }
                extR.tag = ent->bean->DisplayType();
                status->result->extRs.emplace_back(std::make_shared<NekoGui_fmt::ExternalBuildResult>(extR));

                // SOCKS OUTBOUND
                if (status->IsSingBox()) {
                    outbound["type"] = "socks";
                    outbound["server"] = "127.0.0.1";
                    outbound["server_port"] = ext_socks_port;
                } else {
                    outbound["protocol"] = "socks";
                    QJsonObject settings;
                    QJsonArray servers;
                    QJsonObject server;
                    server["address"] = "127.0.0.1";
                    server["port"] = ext_socks_port;
                    servers.push_back(server);
                    settings["servers"] = servers;
                    outbound["settings"] = settings;
                }
            } else {
                const auto coreR = status->IsSingBox() ? ent->bean->BuildCoreObjSingBox(dataStore) : ent->bean->BuildCoreObjV2Ray(dataStore);
                if (coreR.outbound.isEmpty()) {
                    status->result->error = "unsupported outbound";
                    return {};
                }
                if (!coreR.error.isEmpty()) { // rejected
                    status->result->error = coreR.error;
                    return {};
                }
                outbound = coreR.outbound;
            }

            // outbound misc
            outbound["tag"] = tagOut;
            if (!status->forTest) {
                ent->traffic_data->id = ent->id;
                ent->traffic_data->tag = tagOut.toStdString();
                status->result->outboundStats += ent->traffic_data;
            }

            // mux common
            auto needMux = ent->type == "vmess" || ent->type == "trojan" || ent->type == "vless";
            needMux &= dataStore->mux_concurrency > 0;

            if (stream != nullptr) {
                if (status->IsSingBox()) {
                    if (stream->network == "grpc" || stream->network == "quic" || (stream->network == "http" && stream->security == "tls")) {
                        needMux = false;
                    }
                } else {
                    if (stream->network == "grpc" || stream->network == "quic") {
                        needMux = false;
                    }
                }
                if (stream->multiplex_status == 0) {
                    if (!dataStore->mux_default_on) needMux = false;
                } else if (stream->multiplex_status == 1) {
                    needMux = true;
                } else if (stream->multiplex_status == 2) {
                    needMux = false;
                }
            }
            if (ent->type == "vless" && !ent->TrojanVLESSBean()->flow.isEmpty() && ent->TrojanVLESSBean()->flow != "none") {
                needMux = false;
            }

            // common
            if (status->IsSingBox()) {
                // apply domain_strategy
                outbound["domain_strategy"] = dataStore->routing->outbound_domain_strategy;
                // apply mux
                if (!muxApplied && needMux) {
                    auto muxObj = QJsonObject{
                        {"enabled", true},
                        {"protocol", dataStore->mux_protocol},
                        {"padding", dataStore->mux_padding},
                        {"max_streams", dataStore->mux_concurrency},
                    };
                    outbound["multiplex"] = muxObj;
                    muxApplied = true;
                }
            } else {
                // apply domain_strategy
                if (dataStore->routing->outbound_domain_strategy != "AsIs" && !dataStore->routing->outbound_domain_strategy.isEmpty()) {
                    auto streamSettings = outbound["streamSettings"].toObject();
                    auto sockopt = streamSettings["sockopt"].toObject();
                    sockopt["domainStrategy"] = dataStore->routing->outbound_domain_strategy;
                    streamSettings["sockopt"] = sockopt;
                    outbound["streamSettings"] = streamSettings;
                }
                // apply mux
                if (!muxApplied && needMux) {
                    auto muxObj = QJsonObject{
                        {"enabled", true},
                        {"concurrency", dataStore->mux_concurrency},
                    };
                    outbound["mux"] = muxObj;
                    muxApplied = true;
                }
            }

            // apply custom outbound settings
            MergeJson(QString2QJsonObject(ent->bean->custom_outbound), outbound);

            // Bypass Lookup for the first profile
            auto serverAddress = ent->bean->serverAddress;

            auto customBean = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
            if (customBean != nullptr && customBean->core == "internal") {
                auto server = QString2QJsonObject(customBean->config_simple)["server"].toString();
                if (!server.isEmpty()) serverAddress = server;
            }

            if (!IsIpAddress(serverAddress)) {
                status->domainListDNSDirect += "full:" + serverAddress;
            }

            status->outbounds += outbound;
            pastTag = tagOut;
            pastExternalStat = thisExternalStat;
            index++;
        }

        return chainTagOut;
    }

    // SingBox

    void BuildConfigSingBox(const std::shared_ptr<BuildConfigStatus> &status) {
        const auto dataStore = status->context.settings.get();
        // Log
        status->result->coreConfig["log"] = QJsonObject{{"level", dataStore->log_level}};

        // Inbounds

        // mixed-in
        if (IsValidPort(dataStore->inbound_socks_port) && !status->forTest) {
            QJsonObject inboundObj;
            inboundObj["tag"] = "mixed-in";
            inboundObj["type"] = "mixed";
            inboundObj["listen"] = dataStore->inbound_address;
            inboundObj["listen_port"] = dataStore->inbound_socks_port;
            if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
                inboundObj["sniff"] = true;
                inboundObj["sniff_override_destination"] = dataStore->routing->sniffing_mode == SniffingMode::FOR_DESTINATION;
            }
            if (dataStore->inbound_auth->NeedAuth()) {
                inboundObj["users"] = QJsonArray{
                    QJsonObject{
                        {"username", dataStore->inbound_auth->username},
                        {"password", dataStore->inbound_auth->password},
                    },
                };
            }
            inboundObj["domain_strategy"] = dataStore->routing->domain_strategy;
            status->inbounds += inboundObj;
        }

        // Keep the user's existing HTTP proxy port when switching from Xray.
        // The mixed listener already covers HTTP if both configured ports match.
        if (IsValidPort(dataStore->inbound_http_port) && dataStore->inbound_http_port != dataStore->inbound_socks_port && !status->forTest) {
            QJsonObject inboundObj{{"tag", "http-in"}, {"type", "http"},
                                   {"listen", dataStore->inbound_address}, {"listen_port", dataStore->inbound_http_port},
                                   {"domain_strategy", dataStore->routing->domain_strategy}};
            if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
                inboundObj["sniff"] = true;
                inboundObj["sniff_override_destination"] = dataStore->routing->sniffing_mode == SniffingMode::FOR_DESTINATION;
            }
            if (dataStore->inbound_auth->NeedAuth()) {
                inboundObj["users"] = QJsonArray{QJsonObject{{"username", dataStore->inbound_auth->username},
                                                            {"password", dataStore->inbound_auth->password}}};
            }
            status->inbounds += inboundObj;
        }

        // tun-in
        if (status->InternalTun() && dataStore->spmode_vpn && !status->forTest) {
            QJsonObject inboundObj;
            inboundObj["tag"] = "tun-in";
            inboundObj["type"] = "tun";
            inboundObj["interface_name"] = genTunName();
            inboundObj["auto_route"] = true;
            inboundObj["endpoint_independent_nat"] = true;
            inboundObj["mtu"] = dataStore->vpn_mtu;
            inboundObj["stack"] = Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation);
            inboundObj["strict_route"] = dataStore->vpn_strict_route;
            inboundObj["inet4_address"] = "172.19.0.1/28";
            if (dataStore->vpn_ipv6) inboundObj["inet6_address"] = "fdfe:dcba:9876::1/126";
            if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
                inboundObj["sniff"] = true;
                inboundObj["sniff_override_destination"] = dataStore->routing->sniffing_mode == SniffingMode::FOR_DESTINATION;
            }
            inboundObj["domain_strategy"] = dataStore->routing->domain_strategy;
            status->inbounds += inboundObj;
        }

        // Outbounds
        auto tagProxy = BuildChain(0, status);
        if (!status->result->error.isEmpty()) return;

        if (status->context.externalTun && !status->forTest && !status->forExport &&
            IsValidPort(status->context.tunProxyPort)) {
            QJsonObject privateInbound{{"tag", "tun-proxy-in"}, {"type", "socks"},
                                        {"listen", "127.0.0.1"}, {"listen_port", status->context.tunProxyPort}};
            if (dataStore->inbound_auth->NeedAuth()) privateInbound["users"] = QJsonArray{QJsonObject{
                {"username", dataStore->inbound_auth->username}, {"password", dataStore->inbound_auth->password}}};
            status->inbounds += privateInbound;
            status->routingRules += QJsonObject{{"inbound", QJsonArray{"tun-proxy-in"}}, {"outbound", tagProxy}};
        }

        // direct & bypass & block
        status->outbounds += QJsonObject{
            {"type", "direct"},
            {"tag", "direct"},
            {"domain_strategy", FreedomStrategyForBox(dataStore, status->context.sourceCore)},
        };
        status->outbounds += QJsonObject{
            {"type", "direct"},
            {"tag", "bypass"},
            {"domain_strategy", FreedomStrategyForBox(dataStore, status->context.sourceCore)},
        };
        status->outbounds += QJsonObject{
            {"type", "block"},
            {"tag", "block"},
        };
        if (!status->forTest) {
            status->outbounds += QJsonObject{
                {"type", "dns"},
                {"tag", "dns-out"},
            };
        }

        // custom inbound
        if (!status->forTest) QJSONARRAY_ADD(status->inbounds, QString2QJsonObject(dataStore->custom_inbound)["inbounds"].toArray())

        status->result->coreConfig.insert("inbounds", status->inbounds);
        status->result->coreConfig.insert("outbounds", status->outbounds);

        // user rule
        if (!status->forTest) {
            DOMAIN_USER_RULE
            IP_USER_RULE
        }

        auto make_rule = [](const QStringList &list, bool isIP = false) { return SingBoxRule(list, isIP); };

        // final add DNS
        QJsonObject dns;
        QJsonArray dnsServers;
        QJsonArray dnsRules;

        // Remote
        if (!status->forTest)
            dnsServers += QJsonObject{
                {"tag", "dns-remote"},
                {"address_resolver", "dns-local"},
                {"strategy", dataStore->routing->remote_dns_strategy},
                {"address", dataStore->routing->remote_dns},
                {"detour", tagProxy},
            };

        // Direct
        auto directDNSAddress = dataStore->routing->direct_dns;
        if (directDNSAddress == "localhost") directDNSAddress = BOX_UNDERLYING_DNS_EXPORT;
        if (!status->forTest) {
            QJsonObject directObj{
                {"tag", "dns-direct"},
                {"address_resolver", "dns-local"},
                {"strategy", dataStore->routing->direct_dns_strategy},
                {"address", directDNSAddress.replace("+local://", "://")},
                {"detour", "direct"},
            };
            if (dataStore->routing->dns_final_out == "bypass") {
                dnsServers.prepend(directObj);
            } else {
                dnsServers.append(directObj);
            }
        }

        // block
        if (!status->forTest)
            dnsServers += QJsonObject{
                {"tag", "dns-block"},
                {"address", "rcode://success"},
            };

        // Fakedns
        if (dataStore->fake_dns && status->InternalTun() && dataStore->spmode_vpn && !status->forTest) {
            dnsServers += QJsonObject{
                {"tag", "dns-fake"},
                {"address", "fakeip"},
            };
            dns["fakeip"] = QJsonObject{
                {"enabled", true},
                {"inet4_range", "198.18.0.0/15"},
                {"inet6_range", "fc00::/18"},
            };
        }

        // Underlying 100% Working DNS
        dnsServers += QJsonObject{
            {"tag", "dns-local"},
            {"address", BOX_UNDERLYING_DNS_EXPORT},
            {"detour", "direct"},
        };

        // sing-box dns rule object
        auto add_rule_dns = [&](const QStringList &list, const QString &server) {
            auto rule = make_rule(list, false);
            if (rule.isEmpty()) return;
            rule["server"] = server;
            dnsRules += rule;
        };
        add_rule_dns(status->domainListDNSRemote, "dns-remote");
        add_rule_dns(status->domainListDNSDirect, "dns-direct");

        // built-in rules
        if (!status->forTest) {
            dnsRules += QJsonObject{
                {"query_type", QJsonArray{32, 33}},
                {"server", "dns-block"},
            };
            dnsRules += QJsonObject{
                {"domain_suffix", ".lan"},
                {"server", "dns-block"},
            };
        }

        // fakedns rule
        if (dataStore->fake_dns && status->InternalTun() && dataStore->spmode_vpn && !status->forTest) {
            dnsRules += QJsonObject{
                {"inbound", "tun-in"},
                {"server", "dns-fake"},
            };
        }

        dns["servers"] = dnsServers;
        dns["rules"] = dnsRules;
        dns["independent_cache"] = true;

        if (dataStore->routing->use_dns_object) {
            dns = QString2QJsonObject(dataStore->routing->dns_object);
        }
        status->result->coreConfig.insert("dns", dns);

        // Routing

        // dns hijack
        if (!status->forTest) {
            status->routingRules += QJsonObject{
                {"protocol", "dns"},
                {"outbound", "dns-out"},
            };
        }

        // sing-box routing rule object
        auto add_rule_route = [&](const QStringList &list, bool isIP, const QString &out) {
            auto rule = make_rule(list, isIP);
            if (rule.isEmpty()) return;
            rule["outbound"] = out;
            status->routingRules += rule;
        };

        // final add user rule
        add_rule_route(status->domainListBlock, false, "block");
        add_rule_route(status->domainListRemote, false, tagProxy);
        add_rule_route(status->domainListDirect, false, "bypass");
        add_rule_route(status->ipListBlock, true, "block");
        add_rule_route(status->ipListRemote, true, tagProxy);
        add_rule_route(status->ipListDirect, true, "bypass");

        // built-in rules
        status->routingRules += QJsonObject{
            {"network", "udp"},
            {"port", QJsonArray{135, 137, 138, 139, 5353}},
            {"outbound", "block"},
        };
        status->routingRules += QJsonObject{
            {"ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}},
            {"outbound", "block"},
        };
        status->routingRules += QJsonObject{
            {"source_ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}},
            {"outbound", "block"},
        };

        // tun user rule
        if (status->InternalTun() && dataStore->spmode_vpn && !status->forTest) {
            auto match_out = dataStore->vpn_rule_white ? "proxy" : "bypass";

            QString process_name_rule = dataStore->vpn_rule_process.trimmed();
            if (!process_name_rule.isEmpty()) {
                auto arr = SplitLinesSkipSharp(process_name_rule);
                QJsonObject rule{{"outbound", match_out},
                                 {"process_name", QList2QJsonArray(arr)}};
                status->routingRules += rule;
            }

            QString cidr_rule = dataStore->vpn_rule_cidr.trimmed();
            if (!cidr_rule.isEmpty()) {
                auto arr = SplitLinesSkipSharp(cidr_rule);
                QJsonObject rule{{"outbound", match_out},
                                 {"ip_cidr", QList2QJsonArray(arr)}};
                status->routingRules += rule;
            }

            auto autoBypassExternalProcessPaths = getAutoBypassExternalProcessPaths(status->result);
            if (!autoBypassExternalProcessPaths.isEmpty()) {
                QJsonObject rule{{"outbound", "bypass"},
                                 {"process_name", QList2QJsonArray(autoBypassExternalProcessPaths)}};
                status->routingRules += rule;
            }
        }

        // final add routing rule
        auto routingRules = QString2QJsonObject(dataStore->routing->custom)["rules"].toArray();
        if (status->forTest) routingRules = {};
        if (!status->forTest) QJSONARRAY_ADD(routingRules, QString2QJsonObject(dataStore->custom_route_global)["rules"].toArray())
        QJSONARRAY_ADD(routingRules, status->routingRules)
        if (!status->forTest && !status->forExport && status->context.externalTun && IsValidPort(status->context.tunProxyPort)) {
            routingRules.prepend(QJsonObject{{"inbound", QJsonArray{"tun-proxy-in"}}, {"outbound", tagProxy}});
        }
        auto routeObj = QJsonObject{
            {"rules", routingRules},
            {"auto_detect_interface", dataStore->spmode_vpn || status->context.externalTun},
        };
        // A node probe has no geolocation routing rules. Requiring the GUI's
        // databases here can fail a working node before the core even tests it.
        if (!status->forTest) {
            const auto geoip = status->context.geoipPath;
            const auto geosite = status->context.geositePath;
            const auto ruleText = QJsonDocument(routingRules).toJson(QJsonDocument::Compact) +
                                  QJsonDocument(dnsRules).toJson(QJsonDocument::Compact);
            if (ruleText.contains("\"geoip\"")) {
                if (geoip.isEmpty() && !status->forExport) status->result->error = "geoip.db not found";
                routeObj["geoip"] = QJsonObject{{"path", geoip}};
            }
            if (ruleText.contains("\"geosite\"")) {
                if (geosite.isEmpty() && !status->forExport) status->result->error = "geosite.db not found";
                routeObj["geosite"] = QJsonObject{{"path", geosite}};
            }
            routeObj["final"] = dataStore->routing->def_outbound;
        }
        if (status->forExport) {
            routeObj.remove("geoip");
            routeObj.remove("geosite");
            routeObj.remove("auto_detect_interface");
        }
        status->result->coreConfig.insert("route", routeObj);

        // experimental
        QJsonObject experimentalObj;

        if (!status->forTest && dataStore->core_box_clash_api > 0) {
            QJsonObject clash_api = {
                {"external_controller", "127.0.0.1:" + Int2String(dataStore->core_box_clash_api)},
                {"secret", dataStore->core_box_clash_api_secret},
                {"external_ui", "dashboard"},
            };
            experimentalObj["clash_api"] = clash_api;
        }

        if (!experimentalObj.isEmpty()) status->result->coreConfig.insert("experimental", experimentalObj);
    }

    std::shared_ptr<BuildConfigResult> BuildVPNSingBoxConfig(const BuildConfigContext &context) {
        auto result = std::make_shared<BuildConfigResult>();
        result->targetCore = CoreType::SING_BOX;
        if (!context.settings || !context.settings->routing) {
            result->error = QObject::tr("缺少 TUN 设置快照");
            return result;
        }
        if (!IsValidPort(context.tunProxyPort) || context.tunProxyPort == context.settings->inbound_socks_port ||
            context.tunProxyPort == context.settings->inbound_http_port) {
            result->error = QObject::tr("TUN 需要独立的本地代理端口");
            return result;
        }
        result->error = CoreSettingsCompatibilityError(CoreType::SING_BOX, context.sourceCore, context.settings.get());
        if (!result->error.isEmpty()) return result;
        auto hasRawRules = [](const QString &text, const QString &key) {
            if (text.trimmed().isEmpty()) return false;
            const auto doc = QJsonDocument::fromJson(text.toUtf8());
            if (!doc.isObject()) return true;
            const auto obj = doc.object();
            return !obj.isEmpty() && (obj.size() != 1 || !obj[key].isArray() || !obj[key].toArray().isEmpty());
        };
        if (context.settings->routing->use_dns_object || hasRawRules(context.settings->routing->custom, "rules") ||
            hasRawRules(context.settings->custom_route_global, "rules") || hasRawRules(context.settings->custom_inbound, "inbounds")) {
            result->error = QObject::tr("自定义 DNS、入站或路由尚不能无损转换到独立 TUN，已保留原配置");
            return result;
        }
        if (!context.hookSource.isEmpty() || QFileInfo::exists("vpn/sing-box-vpn.json")) {
            result->error = QObject::tr("自定义 TUN 模板或 hook.js 需要先适配独立 TUN，不能在切换时忽略");
            return result;
        }
        auto snapshot = CopySettings(context.settings.get());
        auto dataStore = snapshot.get();
        result->error = ConvertStrategies(dataStore, CoreType::SING_BOX);
        if (!result->error.isEmpty()) return result;

        QStringList processPaths;
        for (const auto &path: context.tunBypassProcessPaths) {
            if (!path.isEmpty()) processPaths += QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        }
        if (processPaths.isEmpty()) {
            result->error = QObject::tr("尚未指定本实例 TUN 的核心进程路径");
            return result;
        }
        QJsonObject tun{{"type", "tun"}, {"tag", "tun-in"}, {"interface_name", genTunName()},
                         {"auto_route", true}, {"strict_route", dataStore->vpn_strict_route},
                         {"endpoint_independent_nat", true}, {"mtu", dataStore->vpn_mtu},
                         {"stack", Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation)},
                         {"inet4_address", "172.19.0.1/28"}, {"domain_strategy", dataStore->routing->domain_strategy}};
        if (dataStore->vpn_ipv6) tun["inet6_address"] = "fdfe:dcba:9876::1/126";
        if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
            tun["sniff"] = true;
            tun["sniff_override_destination"] = dataStore->routing->sniffing_mode == SniffingMode::FOR_DESTINATION;
        }

        const auto underlying = QString(BOX_UNDERLYING_DNS);
        auto directAddress = dataStore->routing->direct_dns;
        if (directAddress == "localhost") directAddress = underlying;
        directAddress.replace("+local://", "://");
        QJsonArray dnsServers{
            QJsonObject{{"tag", "dns-remote"}, {"address", dataStore->routing->remote_dns},
                        {"strategy", dataStore->routing->remote_dns_strategy}, {"address_resolver", "dns-local"}, {"detour", "proxy"}},
            QJsonObject{{"tag", "dns-direct"}, {"address", directAddress},
                        {"strategy", dataStore->routing->direct_dns_strategy}, {"address_resolver", "dns-local"}, {"detour", "direct"}},
            QJsonObject{{"tag", "dns-local"}, {"address", underlying}, {"detour", "direct"}},
            QJsonObject{{"tag", "dns-block"}, {"address", "rcode://success"}}};
        QJsonArray dnsRules;
        dnsRules += QJsonObject{{"process_path", QList2QJsonArray(processPaths)}, {"server", "dns-direct"}};
        if (dataStore->routing->dns_routing) {
            for (const auto &pair: {qMakePair(dataStore->routing->proxy_domain, QString("dns-remote")),
                                    qMakePair(dataStore->routing->direct_domain, QString("dns-direct"))}) {
                auto rule = SingBoxRule(SplitLinesSkipSharp(pair.first));
                if (!rule.isEmpty()) {
                    rule["server"] = pair.second;
                    dnsRules += rule;
                }
            }
        }
        dnsRules += QJsonObject{{"query_type", QJsonArray{32, 33}}, {"server", "dns-block"}};
        dnsRules += QJsonObject{{"domain_suffix", QJsonArray{".lan"}}, {"server", "dns-block"}};
        QJsonObject dns{{"final", dataStore->routing->dns_final_out == "bypass" ? "dns-direct" : "dns-remote"},
                        {"independent_cache", true}};
        if (dataStore->fake_dns) {
            dnsServers += QJsonObject{{"tag", "dns-fake"}, {"address", "fakeip"}};
            dnsRules += QJsonObject{{"inbound", QJsonArray{"tun-in"}}, {"server", "dns-fake"}};
            dns["fakeip"] = QJsonObject{{"enabled", true}, {"inet4_range", "198.18.0.0/15"}, {"inet6_range", "fc00::/18"}};
        }
        dns["servers"] = dnsServers;
        dns["rules"] = dnsRules;

        QJsonArray routeRules{
            QJsonObject{{"process_path", QList2QJsonArray(processPaths)}, {"outbound", "direct"}},
            QJsonObject{{"port", QJsonArray{53}}, {"outbound", "dns-out"}},
            QJsonObject{{"protocol", QJsonArray{"dns"}}, {"outbound", "dns-out"}}};
        const bool wasInternal = context.sourceCore == CoreType::SING_BOX && dataStore->vpn_internal_tun;
        const auto processes = SplitLinesSkipSharp(dataStore->vpn_rule_process);
        const auto cidrs = SplitLinesSkipSharp(dataStore->vpn_rule_cidr);
        QJsonArray tunMatches;
        if (!processes.isEmpty()) tunMatches += QJsonObject{{"process_name", QList2QJsonArray(processes)}};
        if (!cidrs.isEmpty()) tunMatches += QJsonObject{{"ip_cidr", QList2QJsonArray(cidrs)}};
        // Preserve the original external helper's whitelist/blacklist scope.
        // Matching traffic then evaluates common DNS/routing exactly once.
        bool bypassAll = false;
        if (!wasInternal) {
            if (dataStore->vpn_rule_white) {
                if (tunMatches.isEmpty()) bypassAll = true;
                else routeRules += QJsonObject{{"type", "logical"}, {"mode", "or"}, {"rules", tunMatches},
                                                {"invert", true}, {"outbound", "bypass"}};
            } else {
                for (const auto &entry: tunMatches) {
                    auto rule = entry.toObject();
                    rule["outbound"] = "bypass";
                    routeRules += rule;
                }
            }
        }
        auto addRule = [&](const QString &list, bool ip, const QString &outbound) {
            auto rule = SingBoxRule(SplitLinesSkipSharp(list), ip);
            if (!rule.isEmpty()) {
                rule["outbound"] = outbound;
                routeRules += rule;
            }
        };
        if (!bypassAll) {
            addRule(dataStore->routing->block_domain, false, "block");
            addRule(dataStore->routing->block_ip, true, "block");
            addRule(dataStore->routing->proxy_domain, false, "proxy");
            addRule(dataStore->routing->proxy_ip, true, "proxy");
            addRule(dataStore->routing->direct_domain, false, "bypass");
            addRule(dataStore->routing->direct_ip, true, "bypass");
        }
        routeRules += QJsonObject{{"network", "udp"}, {"port", QJsonArray{135, 137, 138, 139, 5353}}, {"outbound", "block"}};
        routeRules += QJsonObject{{"ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}}, {"outbound", "block"}};
        routeRules += QJsonObject{{"source_ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}}, {"outbound", "block"}};
        if (wasInternal) {
            for (const auto &entry: tunMatches) {
                auto rule = entry.toObject();
                rule["outbound"] = dataStore->vpn_rule_white ? "proxy" : "bypass";
                routeRules += rule;
            }
        }
        QJsonObject route{{"rules", routeRules}, {"auto_detect_interface", true},
                           {"final", bypassAll ? QString("bypass") : dataStore->routing->def_outbound}};
        const auto routeText = QJsonDocument(routeRules).toJson(QJsonDocument::Compact);
        if (routeText.contains("\"geoip\"")) {
            if (context.geoipPath.isEmpty()) result->error = QObject::tr("TUN 路由需要 geoip.db");
            route["geoip"] = QJsonObject{{"path", context.geoipPath}};
        }
        if (routeText.contains("\"geosite\"")) {
            if (context.geositePath.isEmpty()) result->error = QObject::tr("TUN 路由需要 geosite.db");
            route["geosite"] = QJsonObject{{"path", context.geositePath}};
        }
        const auto dnsText = QJsonDocument(dnsRules).toJson(QJsonDocument::Compact);
        if (dnsText.contains("\"geosite\"")) {
            if (context.geositePath.isEmpty()) result->error = QObject::tr("TUN DNS 规则需要 geosite.db");
            route["geosite"] = QJsonObject{{"path", context.geositePath}};
        }
        QJsonObject proxy{{"type", "socks"}, {"tag", "proxy"}, {"server", "127.0.0.1"},
                          {"server_port", context.tunProxyPort}, {"version", "5"}};
        if (dataStore->inbound_auth->NeedAuth()) {
            proxy["username"] = dataStore->inbound_auth->username;
            proxy["password"] = dataStore->inbound_auth->password;
        }
        QJsonArray outbounds{
            proxy,
            QJsonObject{{"type", "direct"}, {"tag", "direct"}, {"domain_strategy", FreedomStrategyForBox(dataStore, context.sourceCore)}},
            QJsonObject{{"type", "direct"}, {"tag", "bypass"}, {"domain_strategy", FreedomStrategyForBox(dataStore, context.sourceCore)}},
            QJsonObject{{"type", "block"}, {"tag", "block"}},
            QJsonObject{{"type", "dns"}, {"tag", "dns-out"}}};
        result->coreConfig = QJsonObject{{"log", QJsonObject{{"level", dataStore->log_level}}},
                                         {"inbounds", QJsonArray{tun}}, {"outbounds", outbounds}, {"dns", dns}, {"route", route}};
        return result;
    }

    QString WriteVPNSingBoxConfig(const BuildConfigContext &context) {
        const auto config = BuildVPNSingBoxConfig(context);
        if (!config->error.isEmpty()) return {};
        QFile file("sing-box-vpn.json");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        if (file.write(QJsonDocument(config->coreConfig).toJson()) < 0) return {};
        file.close();
        return QFileInfo(file).absoluteFilePath();
    }

    QString WriteVPNSingBoxConfig() {
        // tun user rule
        auto match_out = dataStore->vpn_rule_white ? "nekoray-socks" : "direct";
        auto no_match_out = dataStore->vpn_rule_white ? "direct" : "nekoray-socks";

        QString process_name_rule = dataStore->vpn_rule_process.trimmed();
        if (!process_name_rule.isEmpty()) {
            auto arr = SplitLinesSkipSharp(process_name_rule);
            QJsonObject rule{{"outbound", match_out},
                             {"process_name", QList2QJsonArray(arr)}};
            process_name_rule = "," + QJsonObject2QString(rule, false);
        }

        QString cidr_rule = dataStore->vpn_rule_cidr.trimmed();
        if (!cidr_rule.isEmpty()) {
            auto arr = SplitLinesSkipSharp(cidr_rule);
            QJsonObject rule{{"outbound", match_out},
                             {"ip_cidr", QList2QJsonArray(arr)}};
            cidr_rule = "," + QJsonObject2QString(rule, false);
        }

        // TODO bypass ext core process path?

        // auth
        QString socks_user_pass;
        if (dataStore->inbound_auth->NeedAuth()) {
            socks_user_pass = R"( "username": "%1", "password": "%2", )";
            socks_user_pass = socks_user_pass.arg(dataStore->inbound_auth->username, dataStore->inbound_auth->password);
        }
        // gen config
        auto configFn = ":/neko/vpn/sing-box-vpn.json";
        if (QFile::exists("vpn/sing-box-vpn.json")) configFn = "vpn/sing-box-vpn.json";
        auto config = ReadFileText(configFn)
                          .replace("//%IPV6_ADDRESS%", dataStore->vpn_ipv6 ? R"("inet6_address": "fdfe:dcba:9876::1/126",)" : "")
                          .replace("//%SOCKS_USER_PASS%", socks_user_pass)
                          .replace("//%PROCESS_NAME_RULE%", process_name_rule)
                          .replace("//%CIDR_RULE%", cidr_rule)
                          .replace("%MTU%", Int2String(dataStore->vpn_mtu))
                          .replace("%STACK%", Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation))
                          .replace("%TUN_NAME%", genTunName())
                          .replace("%STRICT_ROUTE%", dataStore->vpn_strict_route ? "true" : "false")
                          .replace("%FINAL_OUT%", no_match_out)
                          .replace("%DNS_ADDRESS%", BOX_UNDERLYING_DNS)
                          .replace("%FAKE_DNS_INBOUND%", dataStore->fake_dns ? "tun-in" : "empty")
                          .replace("%PORT%", Int2String(dataStore->inbound_socks_port));
        // hook.js
        auto source = qjs::ReadHookJS();
        if (!source.isEmpty()) {
            qjs::QJS js(source);
            auto js_result = js.EvalFunction("hook.hook_tun_config", config);
            if (config != js_result) {
                MW_show_log("hook.js modified your Tun config.");
                config = js_result;
            }
        }
        // write config
        QFile file;
        file.setFileName(QFileInfo(configFn).fileName());
        file.open(QIODevice::ReadWrite | QIODevice::Truncate);
        file.write(config.toUtf8());
        file.close();
        return QFileInfo(file).absoluteFilePath();
    }

    QString WriteVPNLinuxScript(const QString &protectPath, const QString &configPath) {
#ifdef Q_OS_WIN
        return {};
#endif
        // gen script
        auto scriptFn = ":/neko/vpn/vpn-run-root.sh";
        if (QFile::exists("vpn/vpn-run-root.sh")) scriptFn = "vpn/vpn-run-root.sh";
        auto script = ReadFileText(scriptFn)
                          .replace("./nekobox_core", QApplication::applicationDirPath() + "/nekobox_core")
                          .replace("$PROTECT_LISTEN_PATH", protectPath)
                          .replace("$CONFIG_PATH", configPath)
                          .replace("$TABLE_FWMARK", "514");
        // hook.js
        auto source = qjs::ReadHookJS();
        if (!source.isEmpty()) {
            qjs::QJS js(source);
            auto js_result = js.EvalFunction("hook.hook_tun_script", script);
            if (script != js_result) {
                MW_show_log("hook.js modified your Tun script.");
                script = js_result;
            }
        }
        // write script
        QFile file2;
        file2.setFileName(QFileInfo(scriptFn).fileName());
        file2.open(QIODevice::ReadWrite | QIODevice::Truncate);
        file2.write(script.toUtf8());
        file2.close();
        return QFileInfo(file2).absoluteFilePath();
    }

} // namespace NekoGui
