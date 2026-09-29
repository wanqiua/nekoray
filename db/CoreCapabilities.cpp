#include "db/CoreCapabilities.hpp"
#include "db/Database.hpp"
#include "fmt/includes.h"
#include "fmt/Preset.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QReadWriteLock>
#include <QSet>
#include <QStandardPaths>

namespace NekoGui {
    QString CoreDisplayName(int core) {
        return core == CoreType::V2RAY ? QStringLiteral("Xray")
                                      : core == CoreType::SING_BOX ? QStringLiteral("sing-box") : QObject::tr("未选择内核");
    }

    namespace {
        QReadWriteLock runtimeFeaturesLock;
        QMap<int, QStringList> runtimeFeatures;

        bool NonEmpty(const QString &value) { return !value.trimmed().isEmpty(); }
        bool HasHeader(const QString &value) { return NonEmpty(value) && value != "none"; }

        QSet<QString> ConfigFeatures(const QJsonObject &config, int core) {
            const bool box = core == CoreType::SING_BOX;
            QSet<QString> required{"rpc.check"};
            const QSet<QString> protocols{"socks", "http", "shadowsocks", "vmess", "vless", "trojan",
                                           "anytls", "snell", "ssh", "shadowtls", "hysteria", "hysteria2", "tuic"};
            auto boxTls = [&](const QJsonObject &tls) {
                if (!tls["enabled"].toBool()) return;
                required.insert("tls");
                if (tls["insecure"].toBool()) required.insert("tls.insecure");
                if (tls["utls"].toObject()["enabled"].toBool()) required.insert("tls.utls");
                if (tls["reality"].toObject()["enabled"].toBool()) required.insert("tls.reality");
            };
            auto ssMethod = [&](const QString &method) {
                if (method == "rc4-md5" || method == "bf-cfb") required.insert("shadowsocks." + method);
                if (method.startsWith("2022-")) required.insert("shadowsocks.2022");
            };
            for (const auto &entry: config["outbounds"].toArray()) {
                const auto outbound = entry.toObject();
                const auto protocol = outbound[box ? "type" : "protocol"].toString();
                if (protocols.contains(protocol)) required.insert("outbound." + protocol);
                if (protocol == "wireguard") required.insert(box ? "endpoint.wireguard" : "outbound.wireguard");
                if (box && protocol == "hysteria2" && !outbound["tls_certificate_sha256"].toString().isEmpty()) {
                    required.insert("hysteria2.certificate-pin");
                }
                if (protocol == "snell" && (outbound["version"].toInt() == 4 || outbound["version"].toInt() == 6)) {
                    required.insert("snell.v" + QString::number(outbound["version"].toInt()));
                }
                if (protocol == "shadowsocks") {
                    const auto settings = box ? outbound : outbound["settings"].toObject();
                    if (box) ssMethod(settings["method"].toString());
                    else for (const auto &server: settings["servers"].toArray()) ssMethod(server.toObject()["method"].toString());
                    if (!settings["plugin"].toString().isEmpty()) required.insert("shadowsocks.sip003");
                }
                if (box) {
                    const auto transport = outbound["transport"].toObject()["type"].toString();
                    if (!transport.isEmpty()) required.insert("transport." + transport);
                    boxTls(outbound["tls"].toObject());
                } else {
                    const auto stream = outbound["streamSettings"].toObject();
                    auto network = stream["network"].toString();
                    if (network == "h2") network = "http";
                    if (!network.isEmpty()) required.insert("transport." + network);
                    const auto security = stream["security"].toString();
                    if (security == "tls" || security == "reality") {
                        required.insert("tls");
                        const auto tls = stream[security == "reality" ? "realitySettings" : "tlsSettings"].toObject();
                        if (security == "reality") required.insert("tls.reality");
                        if (tls["allowInsecure"].toBool()) required.insert("tls.insecure");
                        if (!tls["fingerprint"].toString().isEmpty()) required.insert("tls.utls");
                    }
                }
            }
            if (box) {
                for (const auto &entry: config["endpoints"].toArray()) {
                    if (entry.toObject()["type"] == "wireguard") required.insert("endpoint.wireguard");
                }
                for (const auto &entry: config["inbounds"].toArray()) {
                    if (entry.toObject()["type"] == "tun") required.insert("tun");
                }
                for (const auto &entry: config["dns"].toObject()["servers"].toArray()) {
                    const auto server = entry.toObject();
                    const auto kind = server["type"].toString();
                    const auto address = entry.isString() ? entry.toString() : server["address"].toString();
                    if (kind == "quic" || address.startsWith("quic://", Qt::CaseInsensitive)) required.insert("dns.quic");
                    if (kind == "h3" || kind == "http3" || address.startsWith("h3://", Qt::CaseInsensitive)) required.insert("dns.http3");
                    boxTls(server["tls"].toObject());
                }
            }
            return required;
        }

        QString FeatureDisplayName(const QString &feature) {
            if (feature == "rpc.check") return QObject::tr("配置预检接口");
            if (feature == "tls.insecure") return QObject::tr("跳过证书验证");
            if (feature == "tls.utls") return QObject::tr("uTLS 指纹");
            if (feature == "tls.reality") return QStringLiteral("REALITY");
            if (feature == "tls") return QStringLiteral("TLS");
            if (feature == "hysteria2.certificate-pin") return QObject::tr("Hysteria2 证书 SHA-256 校验");
            if (feature == "tun") return QStringLiteral("TUN");
            if (feature == "endpoint.wireguard") return QStringLiteral("WireGuard");
            if (feature == "dns.quic") return QStringLiteral("DNS over QUIC");
            if (feature == "dns.http3") return QStringLiteral("DNS over HTTP/3");
            if (feature.startsWith("outbound.")) return feature.mid(9);
            if (feature.startsWith("transport.")) return QObject::tr("%1 传输").arg(feature.mid(10));
            if (feature == "shadowsocks.sip003") return QStringLiteral("Shadowsocks SIP003");
            if (feature.startsWith("shadowsocks.")) return "Shadowsocks " + feature.mid(12);
            if (feature.startsWith("snell.")) return "Snell " + feature.mid(6);
            return feature;
        }

        QString KnownRuntimeFeaturesError(const std::shared_ptr<ProxyEntity> &ent, int core, const DataStore *settings) {
            QStringList features;
            {
                QReadLocker locker(&runtimeFeaturesLock);
                if (!runtimeFeatures.contains(core)) return {};
                features = runtimeFeatures.value(core);
            }
            // Raw overrides can replace an outbound entirely. Their effective
            // requirements are checked against the final built configuration.
            if (NonEmpty(ent->bean->custom_config) || NonEmpty(ent->bean->custom_outbound) ||
                dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get()) != nullptr) return {};
            QJsonObject outbound;
            const auto external = ent->bean->NeedExternal(true, core, settings);
            if (external < 0) return {};
            if (external > 0) {
                outbound[core == CoreType::SING_BOX ? "type" : "protocol"] = "socks";
            } else {
                const auto built = core == CoreType::SING_BOX ? ent->bean->BuildCoreObjSingBox(settings)
                                                            : ent->bean->BuildCoreObjV2Ray(settings);
                if (!built.error.isEmpty()) return built.error;
                outbound = built.outbound;
            }
            return CoreBuildFeaturesError(QJsonObject{{"outbounds", QJsonArray{outbound}}}, core, features);
        }

        bool ExternalCoreExists(const DataStore *settings, const QString &name) {
            const auto path = settings->extraCore->Get(name);
            return !path.isEmpty() && (QFileInfo(path).isFile() || !QStandardPaths::findExecutable(path).isEmpty());
        }

        bool HasCustomObject(const QString &text, const QString &emptyArrayKey = {}) {
            if (text.trimmed().isEmpty()) return false;
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) return true;
            const auto object = doc.object();
            if (object.isEmpty()) return false;
            return emptyArrayKey.isEmpty() || object.size() != 1 || !object[emptyArrayKey].isArray() ||
                   !object[emptyArrayKey].toArray().isEmpty();
        }

        // Raw JSON has no portable semantics. Infer the family only from
        // unambiguous schema keys; ambiguous legacy fragments keep their source.
        int JsonFamily(const QJsonObject &object, int sourceCore) {
            bool ray = false;
            bool box = false;
            for (const auto &key: {"protocol", "settings", "streamSettings", "proxySettings", "mux", "routing", "policy", "stats"}) {
                ray |= object.contains(key);
            }
            for (const auto &key: {"type", "server", "server_port", "transport", "tls", "multiplex", "detour", "route", "endpoints"}) {
                box |= object.contains(key);
            }
            for (const auto &key: {"outbounds", "inbounds"}) {
                for (const auto &entry: object[key].toArray()) {
                    const auto child = entry.toObject();
                    ray |= child.contains("protocol");
                    box |= child.contains("type");
                }
            }
            if (ray && box) return -1;
            return ray ? CoreType::V2RAY : box ? CoreType::SING_BOX : sourceCore;
        }

        QString CheckJsonOwner(const QString &text, const QString &label, int targetCore, int sourceCore) {
            if (!NonEmpty(text)) return {};
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                return QObject::tr("%1不是有效的 JSON 对象").arg(label);
            }
            if (doc.object().isEmpty()) return {};
            const auto owner = JsonFamily(doc.object(), sourceCore);
            if (owner < 0) return QObject::tr("%1混用了两个内核的字段，需要先明确配置归属").arg(label);
            if (owner != targetCore) {
                return QObject::tr("%1属于 %2，不能在切换时丢弃或直接套用").arg(label, CoreDisplayName(owner));
            }
            return {};
        }

        QString CheckStream(NekoGui_fmt::AbstractBean *bean, int core, int sourceCore, const DataStore *settings) {
            const auto stream = NekoGui_fmt::GetStreamSettings(bean);
            if (stream == nullptr) return {};
            const bool box = core == CoreType::SING_BOX;
            const auto network = stream->network;
            if (!QStringList{"tcp", "ws", "http", "grpc", "quic", "kcp", "httpupgrade"}.contains(network)) {
                return QObject::tr("当前内核和界面尚未适配 %1 传输").arg(network);
            }
            if (NonEmpty(stream->security) && stream->security != "none" && stream->security != "tls") {
                return QObject::tr("尚未适配此 TLS/安全类型：%1").arg(stream->security);
            }
            const bool tls = stream->security == "tls";
            const bool reality = tls && NonEmpty(stream->reality_pbk);
            if (box && network == "kcp") return QObject::tr("sing-box 不支持 mKCP 传输");
            if (network == "tcp" && HasHeader(stream->header_type)) {
                if (stream->header_type != "http") return QObject::tr("不支持此 TCP header 类型：%1").arg(stream->header_type);
                if (box) return QObject::tr("sing-box 的 HTTP transport 不能等价替代 Xray TCP HTTP header");
            }
            if (network == "kcp") {
                if (reality) return QObject::tr("mKCP 不支持 REALITY，不能忽略此安全设置");
                if (tls && NonEmpty(stream->utlsFingerprint)) return QObject::tr("当前 Xray mKCP 不能应用 uTLS 指纹");
                if (NonEmpty(stream->host)) return QObject::tr("mKCP 的 Host 字段无法可靠转换，请使用 seed/path");
                if (!QStringList{"", "none", "srtp", "utp", "wechat-video", "dtls", "wireguard"}.contains(stream->header_type)) {
                    return QObject::tr("尚未适配此 mKCP header 类型：%1").arg(stream->header_type);
                }
            }
            if (network == "quic") {
                if (reality) return QObject::tr("QUIC transport 不支持 REALITY");
                if (NonEmpty(stream->utlsFingerprint)) return QObject::tr("QUIC transport 无法应用此 uTLS 指纹");
                if (box && (!tls || NonEmpty(stream->path) || HasHeader(stream->header_type) ||
                            (NonEmpty(stream->host) && stream->host != "none"))) {
                    return QObject::tr("sing-box QUIC 需要 TLS，且不能保留 Xray 的额外密钥、加密或 header 参数");
                }
            }
            if (reality) {
                const QStringList supported = box ? QStringList{"tcp", "http", "grpc", "ws", "httpupgrade"}
                                                   : QStringList{"tcp", "http", "grpc"};
                if (!supported.contains(network)) return QObject::tr("此传输无法完整应用 REALITY");
                if (!box && NonEmpty(stream->alpn)) return QObject::tr("当前 Xray REALITY 无法显式保留 ALPN 设置");
                if (box && NonEmpty(stream->reality_spx)) return QObject::tr("sing-box 无法保留 REALITY spiderX 参数");
                if (stream->reality_sid.contains(',')) return QObject::tr("REALITY 客户端需要一个明确的 short ID");
                if (NonEmpty(stream->certificate)) return QObject::tr("REALITY 与自定义证书验证不能在此适配器中同时应用");
            }
            if (const auto error = stream->CertificatePinError(box); !error.isEmpty()) return error;
            if (network == "ws" && stream->ws_early_data_length > 0 && !box &&
                stream->ws_early_data_name != "Sec-WebSocket-Protocol") {
                return QObject::tr("当前 Xray 的 WS early data 仅支持 Sec-WebSocket-Protocol header");
            }
            const auto tv = dynamic_cast<NekoGui_fmt::TrojanVLESSBean *>(bean);
            const bool vlessXudp = tv != nullptr && tv->proxy_type == NekoGui_fmt::TrojanVLESSBean::proxy_VLESS &&
                                   stream->packet_encoding == "xudp";
            if (NonEmpty(stream->packet_encoding) && !box && !vlessXudp) {
                return QObject::tr("Xray 适配器无法显式保留 packet_encoding=%1").arg(stream->packet_encoding);
            }

            const auto socks = dynamic_cast<NekoGui_fmt::SocksHttpBean *>(bean);
            if (socks != nullptr) {
                if (!QList<int>{NekoGui_fmt::SocksHttpBean::type_HTTP, NekoGui_fmt::SocksHttpBean::type_Socks4,
                                NekoGui_fmt::SocksHttpBean::type_Socks5}.contains(socks->socks_http_type)) {
                    return QObject::tr("界面尚未适配此 SOCKS 版本");
                }
                if (socks->socks_http_type == NekoGui_fmt::SocksHttpBean::type_Socks4 && !socks->password.isEmpty()) {
                    return QObject::tr("SOCKS4 不支持密码认证，请核对节点版本");
                }
                if (box && socks->socks_http_type != NekoGui_fmt::SocksHttpBean::type_HTTP && tls) {
                    return QObject::tr("sing-box SOCKS 出站没有原生 TLS/REALITY 字段");
                }
                if (box && (network != "tcp" || HasHeader(stream->header_type))) {
                    return QObject::tr("sing-box SOCKS/HTTP 出站不支持 V2Ray transport");
                }
            }
            const auto ss = dynamic_cast<NekoGui_fmt::ShadowSocksBean *>(bean);
            if (ss != nullptr && box && (tls || network != "tcp" || HasHeader(stream->header_type))) {
                return QObject::tr("sing-box Shadowsocks 不能直接套用 TLS 或 V2Ray transport，请使用协议明确的插件配置");
            }
            if (tv != nullptr && tv->proxy_type == NekoGui_fmt::TrojanVLESSBean::proxy_VLESS) {
                const auto flow = tv->flow == "none" ? QString() : tv->flow;
                const auto allowed = box ? QStringList{"", "xtls-rprx-vision"}
                                         : QStringList{"", "xtls-rprx-vision", "xtls-rprx-vision-udp443"};
                if (!allowed.contains(flow)) return QObject::tr("%1 无法完整保留 VLESS flow=%2").arg(CoreDisplayName(core), flow);
                if (!flow.isEmpty() && (network != "tcp" || !tls || HasHeader(stream->header_type))) {
                    return QObject::tr("Vision flow 需要 TCP + TLS/REALITY，不能用于此传输组合");
                }
            }
            const bool muxNode = dynamic_cast<NekoGui_fmt::VMessBean *>(bean) != nullptr || tv != nullptr;
            const bool hasFlow = tv != nullptr && tv->proxy_type == NekoGui_fmt::TrojanVLESSBean::proxy_VLESS &&
                                 !tv->flow.isEmpty() && tv->flow != "none";
            const bool muxTransport = network != "grpc" && network != "quic" && !(box && network == "http" && tls) && !hasFlow;
            if (stream->multiplex_status == 1 && (!muxNode || !muxTransport || settings->mux_concurrency <= 0)) {
                return QObject::tr("此协议/传输组合无法保留手动开启的复用设置");
            }
            const bool muxEnabled = muxNode && muxTransport && settings->mux_concurrency > 0 &&
                (stream->multiplex_status == 1 || (stream->multiplex_status == 0 && settings->mux_default_on));
            if (muxEnabled && core != sourceCore) {
                return QObject::tr("此节点启用了 %1 的复用协议，不能在切核时静默改为另一种复用").arg(CoreDisplayName(sourceCore));
            }
            return {};
        }

        QString CheckGraph(const std::shared_ptr<ProxyEntity> &ent, int core, int sourceCore,
                           const DataStore *settings, QSet<int> &chainStack) {
            if (!ent || !ent->bean) return QObject::tr("节点不存在或已被删除");
            if (ent->type != "chain") {
                const auto error = CoreCompatibilityError(ent, core, sourceCore, settings);
                return error.isEmpty() ? KnownRuntimeFeaturesError(ent, core, settings) : error;
            }
            if (chainStack.contains(ent->id)) return QObject::tr("链式代理存在循环引用");
            chainStack.insert(ent->id);
            const auto ids = ent->ChainBean()->list;
            if (ids.isEmpty()) return QObject::tr("链式代理没有节点");
            for (auto id: ids) {
                const auto child = profileManager != nullptr ? profileManager->GetProfile(id) : nullptr;
                if (!child) return QObject::tr("链式代理中的节点 %1 不存在").arg(id);
                if (child->type == "chain") return QObject::tr("暂不支持链式代理嵌套");
                const auto error = CheckGraph(child, core, sourceCore, settings, chainStack);
                if (!error.isEmpty()) return QObject::tr("链式代理：%1").arg(error);
            }
            chainStack.remove(ent->id);
            return CoreCompatibilityError(ent, core, sourceCore, settings);
        }
    } // namespace

    QString CoreBuildFeaturesError(const QJsonObject &config, int targetCore,
                                  const QStringList &advertisedFeatures) {
        if (targetCore != CoreType::V2RAY && targetCore != CoreType::SING_BOX) return QObject::tr("未知内核");
        auto required = ConfigFeatures(config, targetCore).values();
        required.sort();
        QStringList missing;
        for (const auto &feature: required) {
            if (!advertisedFeatures.contains(feature)) missing += FeatureDisplayName(feature);
        }
        if (missing.isEmpty()) return {};
        return QObject::tr("当前 %1 构建缺少此配置需要的功能：%2").arg(CoreDisplayName(targetCore), missing.join("、"));
    }

    void SetCoreRuntimeFeatures(int core, const QStringList &features) {
        if (core != CoreType::V2RAY && core != CoreType::SING_BOX) return;
        QWriteLocker locker(&runtimeFeaturesLock);
        runtimeFeatures[core] = features;
    }

    void ClearCoreRuntimeFeatures(int core) {
        QWriteLocker locker(&runtimeFeaturesLock);
        runtimeFeatures.remove(core);
    }

    QString CoreSettingsCompatibilityError(int targetCore, int sourceCore, const DataStore *settings, bool forTest) {
        if (settings == nullptr || settings->routing == nullptr) return QObject::tr("当前设置尚未载入");
        if (!forTest && targetCore == CoreType::V2RAY && settings->spmode_system_proxy &&
            !IsValidPort(settings->inbound_http_port) && IsValidPort(settings->inbound_socks_port)) {
            return QObject::tr("系统代理正在使用混合端口；启用独立 HTTP 端口后可切换 Xray");
        }
        if (!forTest && targetCore == CoreType::V2RAY && IsValidPort(settings->inbound_http_port) &&
            settings->inbound_http_port == settings->inbound_socks_port) {
            return QObject::tr("Xray 的 HTTP 和 SOCKS 入站需要不同端口，请调整 HTTP 端口后再切换");
        }
        for (const auto &entry: {qMakePair(settings->custom_inbound, QObject::tr("自定义入站")),
                                 qMakePair(settings->custom_route_global, QObject::tr("全局自定义路由")),
                                 qMakePair(settings->routing->custom, QObject::tr("自定义路由")),
                                 qMakePair(settings->routing->use_dns_object ? settings->routing->dns_object : QString(), QObject::tr("自定义 DNS"))}) {
            if (!NonEmpty(entry.first)) continue;
            if (!QJsonDocument::fromJson(entry.first.toUtf8()).isObject()) return QObject::tr("%1不是有效的 JSON 对象").arg(entry.second);
        }
        if (settings->routing->use_dns_object && !NonEmpty(settings->routing->dns_object)) return QObject::tr("自定义 DNS 对象为空");
        if (targetCore == CoreType::V2RAY &&
            !QStringList{"", "AsIs", "UseIP", "UseIPv4", "UseIPv6", "ipv4_only", "ipv6_only"}.contains(settings->routing->outbound_domain_strategy)) {
            return QObject::tr("当前 Xray 无法保留出站地址优先级策略 %1").arg(settings->routing->outbound_domain_strategy);
        }
        if (targetCore == sourceCore) return {};
        if (sourceCore == CoreType::SING_BOX && targetCore == CoreType::V2RAY && NonEmpty(settings->core_box_underlying_dns)) {
            return QObject::tr("当前 sing-box 的底层 DNS 覆盖无法在 Xray 中等价保留");
        }
        if (HasCustomObject(settings->custom_inbound, "inbounds")) return QObject::tr("自定义入站属于 %1，切换前需要适配其字段").arg(CoreDisplayName(sourceCore));
        if (HasCustomObject(settings->custom_route_global, "rules") || HasCustomObject(settings->routing->custom, "rules")) {
            return QObject::tr("自定义路由属于 %1，切换前需要适配其规则").arg(CoreDisplayName(sourceCore));
        }
        if (settings->routing->use_dns_object) return QObject::tr("自定义 DNS 对象属于 %1，不能自动跨内核转换").arg(CoreDisplayName(sourceCore));
        if (settings->enable_js_hook > 0 && QFileInfo::exists(QString("hook.%1.js").arg(software_asset_name))) {
            return QObject::tr("hook.js 没有声明跨内核语义，当前配置保留在 %1").arg(CoreDisplayName(sourceCore));
        }
        const auto strategy = settings->routing->domain_strategy;
        if (NonEmpty(strategy) && strategy != "AsIs") {
            return QObject::tr("当前域名路由策略 %1 不能在两核间等价转换").arg(strategy);
        }
        if (settings->core_ray_direct_dns && sourceCore == CoreType::V2RAY) return QObject::tr("当前启用了 Xray 专属直接 DNS 模式");
        if (targetCore == CoreType::SING_BOX && !QStringList{"", "AsIs", "UseIPv4", "UseIPv6", "UseIP"}.contains(settings->core_ray_freedom_domainStrategy)) {
            return QObject::tr("当前 Freedom 域名策略无法转换");
        }
        const QStringList dnsCommon{"", "use_ip", "use_ip4", "use_ip6", "UseIP", "UseIPv4", "UseIPv6", "ipv4_only", "ipv6_only"};
        if (!dnsCommon.contains(settings->routing->direct_dns_strategy) || !dnsCommon.contains(settings->routing->remote_dns_strategy)) {
            return QObject::tr("当前 DNS 地址优先级策略不能在两核间等价转换");
        }
        return {};
    }

    QString CoreCompatibilityError(const std::shared_ptr<ProxyEntity> &ent, int targetCore,
                                   int sourceCore, const DataStore *settings) {
        if (!ent || !ent->bean) return QObject::tr("节点不存在或已被删除");
        if (targetCore != CoreType::V2RAY && targetCore != CoreType::SING_BOX) return QObject::tr("未知内核");
        if (settings == nullptr) return QObject::tr("当前设置尚未载入");
        for (const auto &custom: {qMakePair(ent->bean->custom_config, QObject::tr("节点自定义配置")),
                                 qMakePair(ent->bean->custom_outbound, QObject::tr("节点自定义出站"))}) {
            const auto error = CheckJsonOwner(custom.first, custom.second, targetCore, sourceCore);
            if (!error.isEmpty()) return error;
        }
        const auto custom = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
        if (custom != nullptr) {
            if (custom->core == "internal" || custom->core == "internal-full") {
                if (!NonEmpty(custom->config_simple) || QString2QJsonObject(custom->config_simple).isEmpty()) {
                    return QObject::tr("自定义内核 JSON 为空或无效");
                }
                return CheckJsonOwner(custom->config_simple, QObject::tr("自定义内核 JSON"), targetCore, sourceCore);
            }
            if (!ExternalCoreExists(settings, custom->core)) return QObject::tr("缺少此节点需要的外部核心：%1").arg(custom->core);
            return {};
        }
        if (auto anytls = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(ent->bean.get())) {
            if (targetCore != CoreType::SING_BOX) return QObject::tr("AnyTLS 由 sing-box 支持");
            return anytls->Validate();
        }
        if (auto snell = dynamic_cast<NekoGui_fmt::SnellBean *>(ent->bean.get())) {
            if (targetCore != CoreType::SING_BOX) return QObject::tr("Snell 由 sing-box 支持");
            return snell->Validate();
        }
        const auto quic = dynamic_cast<NekoGui_fmt::QUICBean *>(ent->bean.get());
        if (quic != nullptr) {
            const auto validation = quic->Validate();
            if (!validation.isEmpty()) return validation;
            const bool native = !quic->forceExternal && (quic->proxy_type != NekoGui_fmt::QUICBean::proxy_Hysteria ||
                                                       quic->hyProtocol == NekoGui_fmt::QUICBean::hysteria_protocol_udp);
            if (native) {
                if (targetCore != CoreType::SING_BOX) return QObject::tr("此协议由 sing-box 原生支持");
                return {};
            }
            const auto name = quic->proxy_type == NekoGui_fmt::QUICBean::proxy_TUIC ? QStringLiteral("tuic")
                            : quic->proxy_type == NekoGui_fmt::QUICBean::proxy_Hysteria2 ? QStringLiteral("hysteria2") : QStringLiteral("hysteria");
            if (!ExternalCoreExists(settings, name)) return QObject::tr("此模式需要配置外部 %1 核心").arg(name);
            return {};
        }
        if (dynamic_cast<NekoGui_fmt::NaiveBean *>(ent->bean.get()) != nullptr) {
            if (!ExternalCoreExists(settings, "naive")) return QObject::tr("此节点需要配置外部 NaiveProxy 核心");
            return {};
        }
        const auto ss = dynamic_cast<NekoGui_fmt::ShadowSocksBean *>(ent->bean.get());
        if (ss != nullptr) {
            const auto &methods = targetCore == CoreType::SING_BOX ? Preset::SingBox::ShadowsocksMethods : Preset::Xray::ShadowsocksMethods;
            if (!methods.contains(ss->method)) return QObject::tr("%1 不支持 Shadowsocks 加密方式 %2").arg(CoreDisplayName(targetCore), ss->method);
            if (targetCore == CoreType::V2RAY && ss->uot > 0 && !ss->method.startsWith("2022-")) {
                return QObject::tr("当前 Xray 的旧版 Shadowsocks 不能保留 UDP over TCP 参数");
            }
            if (targetCore == CoreType::V2RAY && ss->method.startsWith("2022-") && NonEmpty(ss->plugin)) {
                return QObject::tr("当前 Xray 的 Shadowsocks 2022 无法保留插件配置");
            }
            if (targetCore == CoreType::SING_BOX && NonEmpty(ss->plugin) &&
                !QStringList{"obfs-local", "v2ray-plugin"}.contains(SubStrBefore(ss->plugin, ";"))) {
                return QObject::tr("sing-box 未适配此 Shadowsocks 插件");
            }
        }
        if (ent->type != "chain" && ss == nullptr && dynamic_cast<NekoGui_fmt::SocksHttpBean *>(ent->bean.get()) == nullptr &&
            dynamic_cast<NekoGui_fmt::VMessBean *>(ent->bean.get()) == nullptr && dynamic_cast<NekoGui_fmt::TrojanVLESSBean *>(ent->bean.get()) == nullptr) {
            return QObject::tr("界面尚未适配此节点类型：%1").arg(ent->type);
        }
        return CheckStream(ent->bean.get(), targetCore, sourceCore, settings);
    }

    CoreResolution ResolveCore(const std::shared_ptr<ProxyEntity> &ent, int preferredDefault) {
        return ResolveCore(ent, preferredDefault, coreType, dataStore);
    }

    CoreResolution ResolveCore(const std::shared_ptr<ProxyEntity> &ent, int preferredDefault,
                               int sourceCore, const DataStore *settings) {
        CoreResolution result;
        auto check = [&](int core) {
            const auto settingsError = CoreSettingsCompatibilityError(core, sourceCore, settings);
            if (!settingsError.isEmpty()) return settingsError;
            QSet<int> stack;
            auto error = CheckGraph(ent, core, sourceCore, settings, stack);
            if (!error.isEmpty()) return error;
            const auto group = ent && profileManager != nullptr ? profileManager->GetGroup(ent->gid) : nullptr;
            if (group && group->front_proxy_id >= 0) {
                const auto front = profileManager->GetProfile(group->front_proxy_id);
                error = CheckGraph(front, core, sourceCore, settings, stack);
                if (!error.isEmpty()) return QObject::tr("分组前置代理：%1").arg(error);
            }
            return QString();
        };
        result.xrayReason = check(CoreType::V2RAY);
        result.singBoxReason = check(CoreType::SING_BOX);
        result.xraySupported = result.xrayReason.isEmpty();
        result.singBoxSupported = result.singBoxReason.isEmpty();
        const auto preference = ent ? ent->core_preference : 0;
        const auto desired = preference == 1 ? CoreType::V2RAY : preference == 2 ? CoreType::SING_BOX
                                           : preferredDefault == CoreType::SING_BOX ? CoreType::SING_BOX : CoreType::V2RAY;
        if (result.Supports(desired)) {
            result.targetCore = desired;
            result.reason = preference == 0 ? QObject::tr("自动 → %1").arg(CoreDisplayName(desired))
                                            : QObject::tr("手动选择 %1").arg(CoreDisplayName(desired));
        } else if (result.xraySupported || result.singBoxSupported) {
            result.targetCore = result.xraySupported ? CoreType::V2RAY : CoreType::SING_BOX;
            result.preferenceFallback = preference != 0;
            result.reason = QObject::tr("%1；将使用 %2%3")
                                .arg(desired == CoreType::V2RAY ? result.xrayReason : result.singBoxReason,
                                     CoreDisplayName(result.targetCore), result.preferenceFallback ? QObject::tr("（保留原偏好）") : QString());
        } else {
            result.reason = QObject::tr("暂不支持：Xray：%1；sing-box：%2").arg(result.xrayReason, result.singBoxReason);
        }
        return result;
    }
} // namespace NekoGui
