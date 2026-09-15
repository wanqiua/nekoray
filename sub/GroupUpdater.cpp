#include "db/ProfileFilter.hpp"
#include "fmt/includes.h"
#include "fmt/Preset.hpp"
#include "main/QJS.hpp"
#include "main/HTTPRequestHelper.hpp"

#include "GroupUpdater.hpp"
#include "ui/UrlImportDialog.hpp"
#include "ui/edit/dialog_edit_group.h"
#include "ui/mainwindow_interface.h"

#include <QThread>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#ifndef NKR_NO_YAML

#include <yaml-cpp/yaml.h>

#endif

namespace NekoGui_sub {

    GroupUpdater *groupUpdater = new GroupUpdater;

    void RawUpdater_FixEnt(const std::shared_ptr<NekoGui::ProxyEntity> &ent) {
        if (ent == nullptr) return;
        auto stream = NekoGui_fmt::GetStreamSettings(ent->bean.get());
        if (stream == nullptr) return;
        // 1. "security"
        if (stream->security == "none" || stream->security == "0" || stream->security == "false") {
            stream->security = "";
        } else if (stream->security == "1" || stream->security == "true") {
            stream->security = "tls";
        }
        // 2. TLS SNI: v2rayN config builder generate sni like this, so set sni here for their format.
        if (stream->security == "tls" && IsIpAddress(ent->bean->serverAddress) && (!stream->host.isEmpty()) && stream->sni.isEmpty()) {
            stream->sni = stream->host;
        }
    }

    void RawUpdater::update(const QString &str) {
        if (str.trimmed().isEmpty() || (!str.contains('\n') && str.trimmed().startsWith('#'))) return;
        // Base64 encoded subscription
        if (auto str2 = DecodeB64IfValid(str); !str2.isEmpty()) {
            update(str2);
            return;
        }

        // Clash
        if (str.contains("proxies:")) {
            updateClash(str);
            return;
        }

        // Multi line
        if (str.count("\n") > 0) {
            auto list = str.split("\n");
            for (const auto &str2: list) {
                update(str2.trimmed());
            }
            return;
        }

        QString error;
        if (!ParseLink(str.trimmed(), &error)) Reject(error.isEmpty() ? QObject::tr("Unrecognized or invalid node") : error);
    }

    void RawUpdater::Reject(const QString &reason) {
        ++rejected_count;
        if (!rejection_reasons.contains(reason)) rejection_reasons.append(reason);
    }

    bool RawUpdater::AddParsedProfile(const std::shared_ptr<NekoGui::ProxyEntity> &profile) {
        if (!profile || !profile->bean || profile->bean->version == -114514) return false;
        const bool hoppingPort = profile->type == "hysteria2" && profile->bean->serverPort == 0 &&
                                 !profile->QUICBean()->hopPort.trimmed().isEmpty() && profile->QUICBean()->Validate().isEmpty();
        if (profile->type != "custom" && profile->type != "chain" &&
            (profile->bean->serverAddress.trimmed().isEmpty() || (profile->bean->serverPort <= 0 && !hoppingPort) ||
             profile->bean->serverPort > 65535)) return false;
        if (!stage_only && !NekoGui::profileManager->AddProfile(profile, gid_add_to)) return false;
        updated_order.append(profile);
        return true;
    }

    bool RawUpdater::ParseLink(const QString &str, QString *error) {
        std::shared_ptr<NekoGui::ProxyEntity> ent;
        bool needFix = true;

        // Nekoray format
        if (str.startsWith("nekoray://")) {
            needFix = false;
            auto link = QUrl(str);
            if (!link.isValid()) return false;
            ent = NekoGui::ProfileManager::NewProxyEntity(link.host());
            if (ent->bean->version == -114514) return false;
            if (ent->type == "anytls") {
                if (!ent->AnyTLSBean()->TryParseLink(str, error)) return false;
                return AddParsedProfile(ent);
            }
            if (ent->type == "snell") {
                if (!ent->SnellBean()->TryParseLink(str, error)) return false;
                return AddParsedProfile(ent);
            }
            if (ent->type == "hysteria2") {
                if (!ent->QUICBean()->TryParseLink(str, error)) return false;
                return AddParsedProfile(ent);
            }
            auto j = DecodeB64IfValid(link.fragment().toUtf8(), QByteArray::Base64UrlEncoding);
            if (j.isEmpty()) return false;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(j, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject() || document.object().isEmpty()) return false;
            if (ent->type != "custom" && ent->type != "chain" &&
                (!document.object().value("addr").isString() || !document.object().value("port").isDouble())) return false;
            ent->bean->FromJson(document.object());
        }

        // SOCKS
        if (str.startsWith("socks5://") || str.startsWith("socks4://") ||
            str.startsWith("socks4a://") || str.startsWith("socks://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("socks");
            auto ok = ent->SocksHTTPBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // HTTP
        if (str.startsWith("http://") || str.startsWith("https://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("http");
            auto ok = ent->SocksHTTPBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // ShadowSocks
        if (str.startsWith("ss://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("shadowsocks");
            auto ok = ent->ShadowSocksBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // VMess
        if (str.startsWith("vmess://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("vmess");
            auto ok = ent->VMessBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // VLESS
        if (str.startsWith("vless://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("vless");
            auto ok = ent->TrojanVLESSBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // Trojan
        if (str.startsWith("trojan://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("trojan");
            auto ok = ent->TrojanVLESSBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // Naive
        if (str.startsWith("naive+")) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("naive");
            auto ok = ent->NaiveBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // Hysteria1
        if (str.startsWith("hysteria://")) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("hysteria");
            auto ok = ent->QUICBean()->TryParseLink(str);
            if (!ok) return false;
        }

        // Hysteria2
        if (str.startsWith("hysteria2://") || str.startsWith("hy2://")) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("hysteria2");
            auto ok = ent->QUICBean()->TryParseLink(str, error);
            if (!ok) return false;
        }

        // TUIC
        if (str.startsWith("tuic://")) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("tuic");
            auto ok = ent->QUICBean()->TryParseLink(str);
            if (!ok) return false;
        }

        if (str.startsWith("anytls://", Qt::CaseInsensitive)) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("anytls");
            if (!ent->AnyTLSBean()->TryParseLink(str, error)) return false;
        }
        if (str.startsWith("snell://", Qt::CaseInsensitive)) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("snell");
            if (!ent->SnellBean()->TryParseLink(str, error)) return false;
        }

        if (ent == nullptr) return false;

        // Fix
        if (needFix) RawUpdater_FixEnt(ent);

        // End
        return AddParsedProfile(ent);
    }

#ifndef NKR_NO_YAML

    QString Node2QString(const YAML::Node &n, const QString &def = "") {
        // yaml-cpp converts a null node to the literal string "null". Missing
        // optional subscription fields must instead keep their default value.
        if (!n.IsDefined() || n.IsNull()) return def;
        try {
            return QString::fromStdString(n.as<std::string>());
        } catch (const YAML::Exception &ex) {
            qDebug() << ex.what();
            return def;
        }
    }

    QStringList Node2QStringList(const YAML::Node &n) {
        try {
            if (n.IsSequence()) {
                QStringList list;
                for (auto item: n) {
                    list << QString::fromStdString(item.as<std::string>());
                }
                return list;
            } else {
                return {};
            }
        } catch (const YAML::Exception &ex) {
            qDebug() << ex.what();
            return {};
        }
    }

    int Node2Int(const YAML::Node &n, const int &def = 0) {
        try {
            return n.as<int>();
        } catch (const YAML::Exception &ex) {
            qDebug() << ex.what();
            return def;
        }
    }

    bool Node2Bool(const YAML::Node &n, const bool &def = false) {
        try {
            return n.as<bool>();
        } catch (const YAML::Exception &ex) {
            try {
                return n.as<int>();
            } catch (const YAML::Exception &ex2) {
                ex2.what();
            }
            qDebug() << ex.what();
            return def;
        }
    }

    // NodeChild returns the first defined children or Null Node
    YAML::Node NodeChild(const YAML::Node &n, const std::list<std::string> &keys) {
        for (const auto &key: keys) {
            auto child = n[key];
            if (child.IsDefined()) return child;
        }
        return {};
    }

#endif

    // https://github.com/Dreamacro/clash/wiki/configuration
    void RawUpdater::updateClash(const QString &str) {
#ifndef NKR_NO_YAML
        try {
            auto proxies = YAML::Load(str.toStdString())["proxies"];
            if (!proxies.IsSequence()) {
                parse_error = QObject::tr("Clash subscription must contain a proxies list");
                Reject(parse_error);
                return;
            }
            for (auto proxy: proxies) {
                if (!proxy.IsMap()) {
                    Reject(QObject::tr("Invalid Clash node"));
                    continue;
                }
                auto type = Node2QString(proxy["type"]).toLower();
                auto type_clash = type;

                if (type == "ss" || type == "ssr") type = "shadowsocks";
                if (type == "socks5") type = "socks";

                auto ent = NekoGui::ProfileManager::NewProxyEntity(type);
                if (ent->bean->version == -114514) {
                    Reject(QObject::tr("Unsupported Clash protocol"));
                    continue;
                }
                bool needFix = false;

                // common
                ent->bean->name = Node2QString(proxy["name"]);
                ent->bean->serverAddress = Node2QString(proxy["server"]);
                ent->bean->serverPort = Node2Int(proxy["port"]);

                // New protocol adapters reject fields they cannot preserve.
                // Silently dropping a TLS or network option can turn a valid
                // subscription entry into a different, misleading profile.
                if (type == "anytls" || type == "snell" || type == "hysteria2") {
                    QSet<QString> fields{"name", "type", "server", "port", "udp"};
                    if (type == "anytls") fields.unite(QSet<QString>{"password", "sni", "servername", "skip-cert-verify",
                        "alpn", "ca-str", "client-fingerprint", "idle-session-check-interval", "idle-session-timeout",
                        "min-idle-session", "client-metadata", "tfo"});
                    else if (type == "snell") fields.unite(QSet<QString>{"version", "psk", "userkey", "reuse", "network", "obfs-opts", "mode"});
                    else fields.unite(QSet<QString>{"password", "ports", "mport", "server-ports", "fingerprint", "sni", "skip-cert-verify", "alpn", "ca-str",
                        "obfs", "obfs-password", "up", "down", "bbr-profile", "gecko-min-packet-size", "gecko-max-packet-size",
                        "disable-chrome-parrot", "hop-interval", "hop-interval-min", "hop-interval-max"});
                    bool unsupported = false;
                    QSet<QString> seenFields;
                    for (const auto &field : proxy) {
                        const auto key = Node2QString(field.first);
                        if (seenFields.contains(key)) {
                            Reject(QObject::tr("%1 订阅参数重复：%2").arg(ent->bean->DisplayType(), key));
                            unsupported = true;
                            break;
                        }
                        seenFields.insert(key);
                        if (!fields.contains(key)) {
                            Reject(QObject::tr("%1 订阅参数尚未适配：%2").arg(ent->bean->DisplayType(), key));
                            unsupported = true;
                            break;
                        }
                    }
                    if (unsupported) continue;
                    const QSet<QString> structured = type == "anytls" ? QSet<QString>{"alpn"}
                                                     : type == "snell" ? QSet<QString>{"obfs-opts"}
                                                                       : QSet<QString>{"alpn", "server-ports"};
                    for (const auto &field : proxy) {
                        const auto key = Node2QString(field.first);
                        if (!structured.contains(key) && !field.second.IsScalar()) {
                            Reject(QObject::tr("%1 订阅参数格式无效：%2").arg(ent->bean->DisplayType(), key));
                            unsupported = true;
                            break;
                        }
                    }
                    if (unsupported) continue;
                    if (proxy["port"].IsDefined()) {
                        bool valid = false;
                        const int port = Node2QString(proxy["port"]).toInt(&valid);
                        if (!valid) {
                            Reject(QObject::tr("%1 端口必须是整数").arg(ent->bean->DisplayType()));
                            continue;
                        }
                        ent->bean->serverPort = port;
                    }
                    if ((type == "anytls" || type == "hysteria2") && proxy["alpn"].IsDefined()) {
                        const auto alpn = proxy["alpn"];
                        bool valid = alpn.IsSequence();
                        if (valid) for (const auto &protocol : alpn) valid &= protocol.IsScalar();
                        if (!valid) { Reject(QObject::tr("%1 ALPN 必须是协议名称列表").arg(ent->bean->DisplayType())); continue; }
                    }
                }

                if (type_clash == "ss") {
                    auto bean = ent->ShadowSocksBean();
                    bean->method = Node2QString(proxy["cipher"]).replace("dummy", "none");
                    bean->password = Node2QString(proxy["password"]);
                    auto plugin_n = proxy["plugin"];
                    auto pluginOpts_n = proxy["plugin-opts"];

                    // UDP over TCP
                    if (Node2Bool(proxy["udp-over-tcp"])) {
                        bean->uot = Node2Int(proxy["udp-over-tcp-version"]);
                        if (bean->uot == 0) bean->uot = 2;
                    }

                    if (plugin_n.IsDefined() && pluginOpts_n.IsDefined()) {
                        QStringList ssPlugin;
                        auto plugin = Node2QString(plugin_n);
                        if (plugin == "obfs") {
                            ssPlugin << "obfs-local";
                            ssPlugin << "obfs=" + Node2QString(pluginOpts_n["mode"]);
                            ssPlugin << "obfs-host=" + Node2QString(pluginOpts_n["host"]);
                        } else if (plugin == "v2ray-plugin") {
                            auto mode = Node2QString(pluginOpts_n["mode"]);
                            auto host = Node2QString(pluginOpts_n["host"]);
                            auto path = Node2QString(pluginOpts_n["path"]);
                            ssPlugin << "v2ray-plugin";
                            if (!mode.isEmpty() && mode != "websocket") ssPlugin << "mode=" + mode;
                            if (Node2Bool(pluginOpts_n["tls"])) ssPlugin << "tls";
                            if (!host.isEmpty()) ssPlugin << "host=" + host;
                            if (!path.isEmpty()) ssPlugin << "path=" + path;
                            // clash only: skip-cert-verify
                            // clash only: headers
                            // clash: mux=?
                        }
                        bean->plugin = ssPlugin.join(";");
                    }

                    // sing-mux
                    auto smux = NodeChild(proxy, {"smux"});
                    if (Node2Bool(smux["enabled"])) bean->stream->multiplex_status = 1;
                } else if (type == "socks" || type == "http") {
                    auto bean = ent->SocksHTTPBean();
                    bean->username = Node2QString(proxy["username"]);
                    bean->password = Node2QString(proxy["password"]);
                    if (Node2Bool(proxy["tls"])) bean->stream->security = "tls";
                    if (Node2Bool(proxy["skip-cert-verify"])) bean->stream->allow_insecure = true;
                } else if (type == "trojan" || type == "vless") {
                    needFix = true;
                    auto bean = ent->TrojanVLESSBean();
                    if (type == "vless") {
                        bean->flow = Node2QString(proxy["flow"]);
                        bean->password = Node2QString(proxy["uuid"]);
                        // meta packet encoding
                        if (Node2Bool(proxy["packet-addr"])) {
                            bean->stream->packet_encoding = "packetaddr";
                        } else {
                            // For VLESS, default to use xudp
                            bean->stream->packet_encoding = "xudp";
                        }
                    } else {
                        bean->password = Node2QString(proxy["password"]);
                    }
                    bean->stream->security = "tls";
                    bean->stream->network = Node2QString(proxy["network"], "tcp");
                    bean->stream->sni = FIRST_OR_SECOND(Node2QString(proxy["sni"]), Node2QString(proxy["servername"]));
                    bean->stream->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    bean->stream->allow_insecure = Node2Bool(proxy["skip-cert-verify"]);
                    bean->stream->utlsFingerprint = Node2QString(proxy["client-fingerprint"]);
                    if (bean->stream->utlsFingerprint.isEmpty()) {
                        bean->stream->utlsFingerprint = NekoGui::dataStore->utlsFingerprint;
                    }

                    // sing-mux
                    auto smux = NodeChild(proxy, {"smux"});
                    if (Node2Bool(smux["enabled"])) bean->stream->multiplex_status = 1;

                    // opts
                    auto ws = NodeChild(proxy, {"ws-opts", "ws-opt"});
                    if (ws.IsMap()) {
                        auto headers = ws["headers"];
                        for (auto header: headers) {
                            if (Node2QString(header.first).toLower() == "host") {
                                bean->stream->host = Node2QString(header.second);
                            }
                        }
                        bean->stream->path = Node2QString(ws["path"]);
                        bean->stream->ws_early_data_length = Node2Int(ws["max-early-data"]);
                        bean->stream->ws_early_data_name = Node2QString(ws["early-data-header-name"]);
                    }

                    auto grpc = NodeChild(proxy, {"grpc-opts", "grpc-opt"});
                    if (grpc.IsMap()) {
                        bean->stream->path = Node2QString(grpc["grpc-service-name"]);
                    }

                    auto reality = NodeChild(proxy, {"reality-opts"});
                    if (reality.IsMap()) {
                        bean->stream->reality_pbk = Node2QString(reality["public-key"]);
                        bean->stream->reality_sid = Node2QString(reality["short-id"]);
                    }
                } else if (type == "vmess") {
                    needFix = true;
                    auto bean = ent->VMessBean();
                    bean->uuid = Node2QString(proxy["uuid"]);
                    bean->aid = Node2Int(proxy["alterId"]);
                    bean->security = Node2QString(proxy["cipher"], bean->security);
                    bean->stream->network = Node2QString(proxy["network"], "tcp").replace("h2", "http");
                    bean->stream->sni = FIRST_OR_SECOND(Node2QString(proxy["sni"]), Node2QString(proxy["servername"]));
                    bean->stream->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    if (Node2Bool(proxy["tls"])) bean->stream->security = "tls";
                    if (Node2Bool(proxy["skip-cert-verify"])) bean->stream->allow_insecure = true;
                    bean->stream->utlsFingerprint = Node2QString(proxy["client-fingerprint"]);
                    bean->stream->utlsFingerprint = Node2QString(proxy["client-fingerprint"]);
                    if (bean->stream->utlsFingerprint.isEmpty()) {
                        bean->stream->utlsFingerprint = NekoGui::dataStore->utlsFingerprint;
                    }

                    // sing-mux
                    auto smux = NodeChild(proxy, {"smux"});
                    if (Node2Bool(smux["enabled"])) bean->stream->multiplex_status = 1;

                    // meta packet encoding
                    if (Node2Bool(proxy["xudp"])) bean->stream->packet_encoding = "xudp";
                    if (Node2Bool(proxy["packet-addr"])) bean->stream->packet_encoding = "packetaddr";

                    // opts
                    auto ws = NodeChild(proxy, {"ws-opts", "ws-opt"});
                    if (ws.IsMap()) {
                        auto headers = ws["headers"];
                        for (auto header: headers) {
                            if (Node2QString(header.first).toLower() == "host") {
                                bean->stream->host = Node2QString(header.second);
                            }
                        }
                        bean->stream->path = Node2QString(ws["path"]);
                        bean->stream->ws_early_data_length = Node2Int(ws["max-early-data"]);
                        bean->stream->ws_early_data_name = Node2QString(ws["early-data-header-name"]);
                        // for Xray
                        if (Node2QString(ws["early-data-header-name"]) == "Sec-WebSocket-Protocol") {
                            bean->stream->path += "?ed=" + Node2QString(ws["max-early-data"]);
                        }
                    }

                    auto grpc = NodeChild(proxy, {"grpc-opts", "grpc-opt"});
                    if (grpc.IsMap()) {
                        bean->stream->path = Node2QString(grpc["grpc-service-name"]);
                    }

                    auto h2 = NodeChild(proxy, {"h2-opts", "h2-opt"});
                    if (h2.IsMap()) {
                        auto hosts = h2["host"];
                        for (auto host: hosts) {
                            bean->stream->host = Node2QString(host);
                            break;
                        }
                        bean->stream->path = Node2QString(h2["path"]);
                    }

                    auto tcp_http = NodeChild(proxy, {"http-opts", "http-opt"});
                    if (tcp_http.IsMap()) {
                        bean->stream->network = "tcp";
                        bean->stream->header_type = "http";
                        auto headers = tcp_http["headers"];
                        for (auto header: headers) {
                            if (Node2QString(header.first).toLower() == "host") {
                                bean->stream->host = Node2QString(header.second[0]);
                            }
                            break;
                        }
                        auto paths = tcp_http["path"];
                        for (auto path: paths) {
                            bean->stream->path = Node2QString(path);
                            break;
                        }
                    }
                } else if (type == "anytls") {
                    auto bean = ent->AnyTLSBean();
                    bean->password = Node2QString(proxy["password"]);
                    bean->sni = Node2QString(NodeChild(proxy, {"sni", "servername"}));
                    if (proxy["sni"].IsDefined() && proxy["servername"].IsDefined() &&
                        Node2QString(proxy["sni"]) != Node2QString(proxy["servername"])) {
                        Reject(QObject::tr("AnyTLS 订阅的 sni 与 servername 冲突"));
                        continue;
                    }
                    bean->allowInsecure = proxy["skip-cert-verify"].IsDefined() && proxy["skip-cert-verify"].as<bool>();
                    bean->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    bean->caText = Node2QString(proxy["ca-str"]);
                    bean->utlsFingerprint = Node2QString(proxy["client-fingerprint"]);
                    auto duration = [&](const char *key) {
                        auto text = Node2QString(proxy[key]);
                        bool numeric = false;
                        text.toDouble(&numeric);
                        return numeric ? text + "s" : text;
                    };
                    bean->idleSessionCheckInterval = duration("idle-session-check-interval");
                    bean->idleSessionTimeout = duration("idle-session-timeout");
                    bean->minIdleSession = proxy["min-idle-session"].IsDefined() ? proxy["min-idle-session"].as<int>() : 0;
                    bean->clientMetadata = Node2QString(proxy["client-metadata"]);
                    if ((proxy["tfo"].IsDefined() && proxy["tfo"].as<bool>()) ||
                        (proxy["udp"].IsDefined() && !proxy["udp"].as<bool>())) {
                        Reject(QObject::tr("AnyTLS 暂不支持订阅中的 TCP Fast Open 或禁用 UDP 设置"));
                        continue;
                    }
                    const auto invalid = bean->Validate();
                    if (!invalid.isEmpty()) { Reject(invalid); continue; }
                } else if (type == "snell") {
                    auto bean = ent->SnellBean();
                    if (!proxy["version"].IsDefined()) {
                        Reject(QObject::tr("Snell 订阅需明确指定 version 为 4 或 6"));
                        continue;
                    }
                    bean->snellVersion = proxy["version"].as<int>();
                    bean->psk = Node2QString(proxy["psk"]);
                    bean->userkey = Node2QString(proxy["userkey"]);
                    bean->reuse = proxy["reuse"].IsDefined() && proxy["reuse"].as<bool>();
                    bean->network = Node2QString(proxy["network"]);
                    if (proxy["udp"].IsDefined()) {
                        const bool udp = proxy["udp"].as<bool>();
                        if ((udp && bean->network == "tcp") || (!udp && bean->network == "udp")) {
                            Reject(QObject::tr("Snell 订阅的 network 与 udp 设置冲突"));
                            continue;
                        }
                        if (!udp) bean->network = "tcp";
                    }
                    const auto obfs = proxy["obfs-opts"];
                    if (obfs.IsDefined()) {
                        bool valid = obfs.IsMap();
                        if (valid) for (const auto &field : obfs) {
                            const auto key = Node2QString(field.first);
                            valid &= (key == "mode" || key == "host") && field.second.IsScalar();
                        }
                        if (!valid) { Reject(QObject::tr("Snell 混淆设置仅支持 mode 和 host")); continue; }
                        bean->obfsMode = Node2QString(obfs["mode"]);
                        bean->obfsHost = Node2QString(obfs["host"]);
                    }
                    bean->mode = Node2QString(proxy["mode"]);
                    const auto invalid = bean->Validate();
                    if (!invalid.isEmpty()) { Reject(invalid); continue; }
                } else if (type == "hysteria") {
                    auto bean = ent->QUICBean();

                    bean->hopPort = Node2QString(proxy["ports"]);

                    bean->allowInsecure = Node2Bool(proxy["skip-cert-verify"]);
                    auto alpn = Node2QStringList(proxy["alpn"]);
                    bean->caText = Node2QString(proxy["ca-str"]);
                    if (!alpn.isEmpty()) bean->alpn = alpn[0];
                    bean->sni = Node2QString(proxy["sni"]);

                    auto auth_str = FIRST_OR_SECOND(Node2QString(proxy["auth_str"]), Node2QString(proxy["auth-str"]));
                    auto auth = Node2QString(proxy["auth"]);
                    if (!auth_str.isEmpty()) {
                        bean->authPayloadType = NekoGui_fmt::QUICBean::hysteria_auth_string;
                        bean->authPayload = auth_str;
                    }
                    if (!auth.isEmpty()) {
                        bean->authPayloadType = NekoGui_fmt::QUICBean::hysteria_auth_base64;
                        bean->authPayload = auth;
                    }
                    bean->obfsPassword = Node2QString(proxy["obfs"]);

                    if (Node2Bool(proxy["disable_mtu_discovery"]) || Node2Bool(proxy["disable-mtu-discovery"])) bean->disableMtuDiscovery = true;
                    bean->streamReceiveWindow = Node2Int(proxy["recv-window"]);
                    bean->connectionReceiveWindow = Node2Int(proxy["recv-window-conn"]);

                    auto upMbps = Node2QString(proxy["up"]).split(" ")[0].toInt();
                    auto downMbps = Node2QString(proxy["down"]).split(" ")[0].toInt();
                    if (upMbps > 0) bean->uploadMbps = upMbps;
                    if (downMbps > 0) bean->downloadMbps = downMbps;
                } else if (type == "hysteria2") {
                    auto bean = ent->QUICBean();

                    QStringList canonicalPorts;
                    bool havePorts = false, invalidPorts = false;
                    for (const auto &key : {"ports", "mport", "server-ports"}) {
                        const auto value = proxy[key];
                        if (!value.IsDefined()) continue;
                        QString text;
                        if (QString(key) == "server-ports") {
                            bool valid = value.IsSequence();
                            if (valid) for (const auto &port : value) valid &= port.IsScalar();
                            if (!valid) { Reject(QObject::tr("Hysteria2 server-ports 需为端口列表")); invalidPorts = true; break; }
                            text = Node2QStringList(value).join(",");
                        } else text = Node2QString(value);
                        NekoGui_fmt::QUICBean candidate(NekoGui_fmt::QUICBean::proxy_Hysteria2);
                        candidate.hopPort = text;
                        QString error;
                        auto normalized = candidate.Hysteria2ServerPorts(&error);
                        normalized.removeDuplicates();
                        normalized.sort();
                        if (!error.isEmpty() || (havePorts && normalized != canonicalPorts)) {
                            Reject(error.isEmpty() ? QObject::tr("Hysteria2 ports、mport 与 server-ports 的范围冲突") : error);
                            invalidPorts = true;
                            break;
                        }
                        if (!havePorts) bean->hopPort = text;
                        canonicalPorts = normalized;
                        havePorts = true;
                    }
                    if (invalidPorts) continue;

                    bean->allowInsecure = proxy["skip-cert-verify"].IsDefined() && proxy["skip-cert-verify"].as<bool>();
                    bean->pinSHA256 = Node2QString(proxy["fingerprint"]);
                    bean->caText = Node2QString(proxy["ca-str"]);
                    bean->sni = Node2QString(proxy["sni"]);
                    bean->alpn = Node2QStringList(proxy["alpn"]).join(",");

                    bean->obfsType = Node2QString(proxy["obfs"]);
                    bean->obfsPassword = Node2QString(proxy["obfs-password"]);
                    bean->password = Node2QString(proxy["password"]);

                    auto bandwidth = [&](const char *key, int *result) {
                        if (!proxy[key].IsDefined()) return true;
                        static const QRegularExpression pattern("^([0-9]+)(?:\\s*Mbps)?$");
                        const auto match = pattern.match(Node2QString(proxy[key]).trimmed());
                        bool valid = false;
                        const auto value = match.captured(1).toInt(&valid);
                        if (!match.hasMatch() || !valid) return false;
                        *result = value;
                        return true;
                    };
                    if (!bandwidth("up", &bean->uploadMbps) || !bandwidth("down", &bean->downloadMbps)) {
                        Reject(QObject::tr("Hysteria2 带宽必须是非负整数 Mbps，例如 100 或 100 Mbps；0 表示使用 BBR"));
                        continue;
                    }
                    bean->bbrProfile = Node2QString(proxy["bbr-profile"], bean->bbrProfile);
                    if (proxy["gecko-min-packet-size"].IsDefined()) bean->geckoMinPacketSize = proxy["gecko-min-packet-size"].as<int>();
                    if (proxy["gecko-max-packet-size"].IsDefined()) bean->geckoMaxPacketSize = proxy["gecko-max-packet-size"].as<int>();
                    bean->disableChromeParrot = proxy["disable-chrome-parrot"].IsDefined() && proxy["disable-chrome-parrot"].as<bool>();
                    if (proxy["hop-interval"].IsDefined() && proxy["hop-interval-min"].IsDefined()) {
                        Reject(QObject::tr("Hysteria2 跳端口最小间隔不能重复指定"));
                        continue;
                    }
                    auto duration = [&](const YAML::Node &value) {
                        auto text = Node2QString(value);
                        bool numeric = false;
                        text.toDouble(&numeric);
                        return numeric ? text + "s" : text;
                    };
                    bean->hopIntervalMin = duration(NodeChild(proxy, {"hop-interval-min", "hop-interval"}));
                    bean->hopIntervalMax = duration(proxy["hop-interval-max"]);
                    if (proxy["udp"].IsDefined() && !proxy["udp"].as<bool>()) {
                        Reject(QObject::tr("Hysteria2 暂不支持订阅中的禁用 UDP 设置"));
                        continue;
                    }
                    const auto invalid = bean->Validate();
                    if (!invalid.isEmpty()) { Reject(invalid); continue; }
                } else if (type == "tuic") {
                    auto bean = ent->QUICBean();

                    bean->uuid = Node2QString(proxy["uuid"]);
                    bean->password = Node2QString(proxy["password"]);

                    if (Node2Int(proxy["heartbeat-interval"]) != 0) {
                        bean->heartbeat = Int2String(Node2Int(proxy["heartbeat-interval"])) + "ms";
                    }

                    bean->udpRelayMode = Node2QString(proxy["udp-relay-mode"], bean->udpRelayMode);
                    bean->congestionControl = Node2QString(proxy["congestion-controller"], bean->congestionControl);

                    bean->disableSni = Node2Bool(proxy["disable-sni"]);
                    bean->zeroRttHandshake = Node2Bool(proxy["reduce-rtt"]);
                    bean->allowInsecure = Node2Bool(proxy["skip-cert-verify"]);
                    bean->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    bean->caText = Node2QString(proxy["ca-str"]);
                    bean->sni = Node2QString(proxy["sni"]);

                    if (Node2Bool(proxy["udp-over-stream"])) bean->uos = true;

                    if (!Node2QString(proxy["ip"]).isEmpty()) {
                        if (bean->sni.isEmpty()) bean->sni = bean->serverAddress;
                        bean->serverAddress = Node2QString(proxy["ip"]);
                    }
                } else {
                    Reject(QObject::tr("Unsupported Clash protocol"));
                    continue;
                }

                if (needFix) RawUpdater_FixEnt(ent);
                if (!AddParsedProfile(ent)) Reject(QObject::tr("Invalid Clash node address or port"));
            }
        } catch (const YAML::Exception &ex) {
            parse_error = QObject::tr("Invalid Clash/YAML subscription at line %1, column %2")
                              .arg(ex.mark.line + 1).arg(ex.mark.column + 1);
            Reject(parse_error);
        }
#else
        parse_error = QObject::tr("This build cannot import Clash/YAML subscriptions");
        Reject(parse_error);
#endif
    }

    SubscriptionUpdateResult CommitSubscriptionUpdate(const std::shared_ptr<NekoGui::Group> &group,
                                                       const RawUpdater &parsed, const QString &subUserInfo) {
        SubscriptionUpdateResult result;
        if (!group || group->archive || NekoGui::profileManager->GetGroup(group->id) != group) {
            result.error = QObject::tr("The target group is no longer available");
            return result;
        }
        if (!parsed.parse_error.isEmpty()) {
            result.error = parsed.parse_error;
            return result;
        }
        if (parsed.rejected_count > 0) {
            result.error = QObject::tr("订阅中有 %1 项无法完整导入，已保留原分组：%2")
                               .arg(parsed.rejected_count).arg(parsed.rejection_reasons.join("；"));
            return result;
        }
        if (parsed.updated_order.isEmpty()) {
            result.error = QObject::tr("No valid nodes were found; existing nodes were kept");
            return result;
        }
        QSet<const NekoGui::ProxyEntity *> detached;
        for (const auto &profile: parsed.updated_order) {
            if (!parsed.stage_only || !profile || !profile->bean || profile->id >= 0 ||
                detached.contains(profile.get())) {
                result.error = QObject::tr("Subscription nodes were not staged safely");
                return result;
            }
            detached.insert(profile.get());
        }

        // Both update modes use the same staged replacement. "Clear before
        // update" can no longer erase identity/preferences before parsing.
        const auto before = group->Profiles();
        const auto matches = NekoGui::ProfileFilter::MatchSubscription(before, parsed.updated_order, group->id);
        result.ambiguous = matches.ambiguous_count;
        QList<std::shared_ptr<NekoGui::ProxyEntity>> added;
        // Allocate while old IDs still exist, so callbacks and chains cannot
        // mistake a newly imported node for a deleted node with a reused ID.
        for (int index = 0; index < parsed.updated_order.size(); ++index) {
            if (matches.existing_for_incoming[index]) continue;
            auto profile = parsed.updated_order[index];
            profile->core_preference = 0;
            if (!NekoGui::profileManager->AddProfile(profile, group->id)) {
                for (const auto &created: added) NekoGui::profileManager->DeleteProfile(created->id);
                result.error = QObject::tr("Could not commit subscription nodes; existing nodes were kept");
                return result;
            }
            added.append(profile);
        }

        QList<int> order;
        for (int index = 0; index < parsed.updated_order.size(); ++index) {
            const auto incoming = parsed.updated_order[index];
            const auto existing = matches.existing_for_incoming[index];
            if (existing) {
                // The complete bean is equal except for its display name.
                // Retain the pointer, ID, local preference, traffic and history.
                existing->bean->name = incoming->bean->name;
                existing->Save();
                order.append(existing->id);
                ++result.preserved;
            } else {
                order.append(incoming->id);
                ++result.added;
            }
        }
        for (const auto &profile: before) {
            if (order.contains(profile->id)) continue;
            NekoGui::profileManager->DeleteProfile(profile->id);
            if (NekoGui::profileManager->GetProfile(profile->id) == profile) {
                // Existing policy protects the active connection. Keep it
                // visible rather than hiding an undeletable live node.
                order.append(profile->id);
                ++result.retained_running;
            } else {
                ++result.removed;
            }
        }
        group->order = order;
        group->sub_last_update = QDateTime::currentSecsSinceEpoch();
        group->info = subUserInfo;
        group->Save();
        result.applied = true;
        return result;
    }

    void GroupUpdater::AsyncUpdate(const QString &str, int _sub_gid, const std::function<void()> &finish) {
        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, [=] { AsyncUpdate(str, _sub_gid, finish); }, Qt::QueuedConnection);
            return;
        }
        auto content = str.trimmed();
        auto subGid = _sub_gid;
        const int importGid = NekoGui::dataStore->current_group;
        bool asURL = subGid >= 0;

        if (subGid < 0 && (content.startsWith("http://") || content.startsWith("https://"))) {
            UrlImportDialog choices(content, GetMainWindow());
            const auto choice = choices.exec();
            if (choice == UrlImportDialog::Cancel) return;
            asURL = choice != UrlImportDialog::Link;
            if (choice == UrlImportDialog::NewGroup) {
                auto draft = NekoGui::ProfileManager::NewGroup();
                draft->name = QUrl(content).host();
                draft->url = content;
                DialogEditGroup editor(draft, GetMainWindow());
                if (editor.exec() != QDialog::Accepted) return;
                if (!NekoGui::profileManager->AddGroup(draft)) return;
                NekoGui::profileManager->SaveManager();
                GetMainWindow()->refresh_groups();
                GetMainWindow()->show_group(draft->id);
                if (draft->url.isEmpty()) return;
                subGid = draft->id;
                content = draft->url;
            }
        }

        const int targetGid = subGid >= 0 ? subGid : importGid;
        const auto target = NekoGui::profileManager->GetGroup(targetGid);
        if (target == nullptr || target->archive) {
            if (finish != nullptr) finish();
            return;
        }
        const auto originalURL = target->url;
        const auto groupName = target->name;

        runOnNewThread([=] {
            auto downloaded = content;
            QString subUserInfo;
            bool succeeded = true;
            if (asURL) {
                MW_show_log(">>>>>>>> " + QObject::tr("Requesting subscription: %1").arg(groupName));
                auto resp = NetworkRequestHelper::HttpGet(content);
                succeeded = resp.error.isEmpty();
                if (succeeded) {
                    downloaded = resp.data;
                    subUserInfo = NetworkRequestHelper::GetHeader(resp.header, "Subscription-UserInfo");
                    MW_show_log("<<<<<<<< " + QObject::tr("Subscription request fininshed: %1").arg(groupName));
                } else {
                    MW_show_log("<<<<<<<< " + QObject::tr("Requesting subscription %1 error: %2").arg(groupName, resp.error + "\n" + resp.data));
                }
            }
            QMetaObject::invokeMethod(this, [=] {
                // The group may have been deleted or edited while downloading.
                // Never redirect an in-flight import to the newly selected tab.
                if (succeeded && NekoGui::profileManager->GetGroup(targetGid) == target && !target->archive &&
                    (subGid < 0 || target->url == originalURL)) {
                    UpdateContent(downloaded, subGid, targetGid, subUserInfo);
                }
                emit asyncUpdateCallback(targetGid);
                if (finish != nullptr) finish();
            }, Qt::QueuedConnection);
        });
    }

    void GroupUpdater::UpdateContent(const QString &_str, int subGid, int targetGid, const QString &subUserInfo) {
        NekoGui::dataStore->imported_count = 0;
        auto content = _str.trimmed();
        auto group = NekoGui::profileManager->GetGroup(subGid);

        // Keep the existing import hook, but run it before any model mutation.
        auto source = qjs::ReadHookJS();
        if (!source.isEmpty()) {
            qjs::QJS js(source);
            auto js_result = js.EvalFunction("hook.hook_import", content);
            if (content != js_result) {
                MW_show_log("hook.js modified your import content.");
                content = js_result;
            }
        }

        RawUpdater parsed;
        parsed.gid_add_to = targetGid;
        parsed.stage_only = true;
        parsed.update(content);
        if (parsed.rejected_count > 0) {
            MW_show_log(QObject::tr("Skipped %1 invalid or unsupported node(s): %2")
                            .arg(parsed.rejected_count).arg(parsed.rejection_reasons.join("; ")));
        }

        if (group != nullptr) {
            const auto result = CommitSubscriptionUpdate(group, parsed, subUserInfo);
            if (!result.applied) {
                MW_show_log(QObject::tr("Subscription %1 was not changed: %2").arg(group->name, result.error));
                return;
            }
            MW_show_log("<<<<<<<< " + QObject::tr("Change of %1: added %2, removed %3, preserved %4")
                                          .arg(group->name).arg(result.added).arg(result.removed).arg(result.preserved));
            if (result.ambiguous > 0) {
                MW_show_log(QObject::tr("%1 node(s) could not be matched uniquely; their core preference is Auto")
                                .arg(result.ambiguous));
            }
            if (result.retained_running > 0) {
                MW_show_log(QObject::tr("The active node was kept because it is still connected"));
            }
            MW_dialog_message("SubUpdater", "finish-dingyue");
        } else {
            if (!parsed.parse_error.isEmpty()) {
                MW_show_log(QObject::tr("Import was not applied: %1").arg(parsed.parse_error));
                return;
            }
            // A plain import still appends to its captured group, even when a
            // different tab was selected while its URL was downloading.
            for (const auto &profile: parsed.updated_order) {
                if (NekoGui::profileManager->AddProfile(profile, targetGid)) ++NekoGui::dataStore->imported_count;
            }
            MW_dialog_message("SubUpdater", "finish");
        }
    }
} // namespace NekoGui_sub

bool UI_update_all_groups_Updating = false;

#define should_skip_group(g) (g == nullptr || g->url.isEmpty() || g->archive || (onlyAllowed && g->skip_auto_update))

void serialUpdateSubscription(const QList<int> &groupsTabOrder, int _order, bool onlyAllowed) {
    if (_order >= groupsTabOrder.size()) {
        UI_update_all_groups_Updating = false;
        return;
    }

    // calculate this group
    auto group = NekoGui::profileManager->GetGroup(groupsTabOrder[_order]);
    if (group == nullptr || should_skip_group(group)) {
        serialUpdateSubscription(groupsTabOrder, _order + 1, onlyAllowed);
        return;
    }

    int nextOrder = _order + 1;
    while (nextOrder < groupsTabOrder.size()) {
        auto nextGid = groupsTabOrder[nextOrder];
        auto nextGroup = NekoGui::profileManager->GetGroup(nextGid);
        if (!should_skip_group(nextGroup)) {
            break;
        }
        nextOrder += 1;
    }

    // Async update current group
    UI_update_all_groups_Updating = true;
    NekoGui_sub::groupUpdater->AsyncUpdate(group->url, group->id, [=] {
        serialUpdateSubscription(groupsTabOrder, nextOrder, onlyAllowed);
    });
}

void UI_update_all_groups(bool onlyAllowed) {
    if (UI_update_all_groups_Updating) {
        MW_show_log("The last subscription update has not exited.");
        return;
    }

    auto groupsTabOrder = NekoGui::profileManager->groupsTabOrder;
    serialUpdateSubscription(groupsTabOrder, 0, onlyAllowed);
}
