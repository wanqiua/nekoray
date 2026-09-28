#include "db/ProxyEntity.hpp"
#include "fmt/includes.h"

#include <QUrlQuery>

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <limits>

namespace {
    bool hysteria2LinkError(QString *error, const QString &message) {
        if (error != nullptr) *error = message;
        return false;
    }

    bool hysteria2DecodeComponent(const QString &encoded, QString *decoded) {
        const auto bytes = QByteArray::fromPercentEncoding(encoded.toLatin1());
        *decoded = QString::fromUtf8(bytes);
        return decoded->toUtf8() == bytes;
    }

    bool hysteria2LinkBoolean(const QString &text, bool *value) {
        if (text == "1" || text == "true") { *value = true; return true; }
        if (text == "0" || text == "false") { *value = false; return true; }
        return false;
    }

    bool hysteria2LinkInteger(const QString &text, qint64 *value) {
        if (!QRegularExpression("^[0-9]+$").match(text).hasMatch()) return false;
        bool valid = false;
        *value = text.toLongLong(&valid);
        return valid;
    }

    QString hysteria2QueryKey(const QString &key) {
        static const QMap<QString, QString> aliases{
            {"peer", "sni"}, {"server_name", "sni"},
            {"insecure", "allowInsecure"}, {"allow_insecure", "allowInsecure"}, {"skip-cert-verify", "allowInsecure"},
            {"obfs", "obfsType"}, {"obfs_type", "obfsType"},
            {"obfs-password", "obfsPassword"}, {"obfs_password", "obfsPassword"},
            {"mport", "hopPort"}, {"server_ports", "hopPort"}, {"serverPorts", "hopPort"},
            {"hop_interval", "hopIntervalMin"}, {"hop_interval_min", "hopIntervalMin"}, {"hop-interval", "hopIntervalMin"}, {"hopInterval", "hopIntervalMin"},
            {"hop_interval_max", "hopIntervalMax"}, {"hop-interval-max", "hopIntervalMax"},
            {"bbr_profile", "bbrProfile"}, {"bbr-profile", "bbrProfile"},
            {"gecko_min_packet_size", "geckoMinPacketSize"}, {"gecko-min-packet-size", "geckoMinPacketSize"}, {"min_packet_size", "geckoMinPacketSize"},
            {"gecko_max_packet_size", "geckoMaxPacketSize"}, {"gecko-max-packet-size", "geckoMaxPacketSize"}, {"max_packet_size", "geckoMaxPacketSize"},
            {"disable_chrome_parrot", "disableChromeParrot"}, {"disable-chrome-parrot", "disableChromeParrot"},
            {"up_mbps", "uploadMbps"}, {"upmbps", "uploadMbps"},
            {"down_mbps", "downloadMbps"}, {"downmbps", "downloadMbps"},
            {"ca", "caText"}, {"certificate", "caText"},
            {"fingerprint", "pinSHA256"}, {"pin-sha256", "pinSHA256"}, {"pin_sha256", "pinSHA256"},
            {"disable_sni", "disableSni"},
            {"stream_receive_window", "streamReceiveWindow"}, {"connection_receive_window", "connectionReceiveWindow"},
            {"disable_path_mtu_discovery", "disableMtuDiscovery"},
        };
        return aliases.value(key, key);
    }

    bool hysteria2ShareObject(const QUrl &url, NekoGui_fmt::QUICBean *bean, QString *error) {
        if (url.host() != "hysteria2" || !url.userInfo().isEmpty() || url.port() != -1 || !url.path().isEmpty() || url.hasQuery()) {
            return hysteria2LinkError(error, QObject::tr("Hysteria2 完整分享链接格式无效"));
        }
        const auto encoded = url.fragment(QUrl::FullyDecoded).toLatin1();
        if (!QRegularExpression("^[A-Za-z0-9_-]+={0,2}$").match(QString::fromLatin1(encoded)).hasMatch()) {
            return hysteria2LinkError(error, QObject::tr("Hysteria2 分享内容不是有效的 Base64URL"));
        }
        const auto bytes = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding);
        const auto canonical = bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        auto unpadded = encoded;
        while (unpadded.endsWith('=')) unpadded.chop(1);
        if (canonical != unpadded) return hysteria2LinkError(error, QObject::tr("Hysteria2 分享内容的 Base64URL 编码无效"));
        QJsonParseError jsonError;
        const auto document = QJsonDocument::fromJson(bytes, &jsonError);
        if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
            return hysteria2LinkError(error, QObject::tr("Hysteria2 分享内容不是有效的 JSON 对象"));
        }
        const auto object = document.object();
        for (const auto &required : {"addr", "port"}) {
            if (!object.contains(required)) return hysteria2LinkError(error, QObject::tr("Hysteria2 分享缺少字段：%1").arg(required));
        }
        for (auto item = object.begin(); item != object.end(); ++item) {
            const auto field = bean->_map.value(item.key());
            if (!field) return hysteria2LinkError(error, QObject::tr("Hysteria2 分享包含未支持的字段：%1").arg(item.key()));
            const auto value = item.value();
            const auto number = value.toDouble();
            const bool integral = value.isDouble() && std::floor(number) == number;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const bool validInteger64 = value.isDouble() && value.toInteger(-1) >= 0;
#else
            const bool validInteger64 = integral && number >= 0 && number < 9223372036854775808.0;
#endif
            const bool valid = (field->type == itemType::string && value.isString()) ||
                               (field->type == itemType::boolean && value.isBool()) ||
                               (field->type == itemType::integer && integral &&
                                number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max()) ||
                               (field->type == itemType::integer64 && validInteger64);
            if (!valid) return hysteria2LinkError(error, QObject::tr("Hysteria2 分享字段类型无效：%1").arg(item.key()));
        }
        if (object.contains("_v") && object.value("_v").toInt() != 0) return hysteria2LinkError(error, QObject::tr("不支持此 Hysteria2 节点数据版本"));
        bean->FromJson(object);
        return true;
    }

    bool parseHysteria2Link(const QString &link, NekoGui_fmt::QUICBean *bean, QString *error) {
        const QUrl url(link.trimmed(), QUrl::StrictMode);
        if (!url.isValid()) return hysteria2LinkError(error, QObject::tr("Hysteria2 链接格式无效"));
        NekoGui_fmt::QUICBean candidate(NekoGui_fmt::QUICBean::proxy_Hysteria2);
        if (url.scheme() == "nekoray") {
            if (!hysteria2ShareObject(url, &candidate, error)) return false;
        } else {
            if ((url.scheme() != "hysteria2" && url.scheme() != "hy2") || (!url.path().isEmpty() && url.path() != "/")) {
                return hysteria2LinkError(error, QObject::tr("Hysteria2 链接必须使用 hy2:// 或 hysteria2://，且不能包含路径"));
            }
            candidate.serverAddress = url.host(QUrl::FullyDecoded);
            candidate.serverPort = url.port(443);
            // The whole user-info is the password, including the colon in
            // userpass authentication. Qt forbids FullyDecoded on userInfo.
            if (!hysteria2DecodeComponent(url.userInfo(QUrl::FullyEncoded), &candidate.password) ||
                !hysteria2DecodeComponent(url.fragment(QUrl::FullyEncoded), &candidate.name)) {
                return hysteria2LinkError(error, QObject::tr("Hysteria2 密码或节点名称包含无效的 UTF-8 编码"));
            }
            QSet<QString> seen;
            for (const auto &item : QUrlQuery(url).queryItems(QUrl::FullyEncoded)) {
                QString queryName, queryValue;
                if (!hysteria2DecodeComponent(item.first, &queryName) || !hysteria2DecodeComponent(item.second, &queryValue)) {
                    return hysteria2LinkError(error, QObject::tr("Hysteria2 链接参数包含无效的 UTF-8 编码"));
                }
                const auto key = hysteria2QueryKey(queryName);
                if (seen.contains(key)) return hysteria2LinkError(error, QObject::tr("Hysteria2 链接包含重复或冲突参数：%1").arg(queryName));
                seen.insert(key);
                if (key == "sni") candidate.sni = queryValue;
                else if (key == "alpn") candidate.alpn = queryValue;
                else if (key == "caText") candidate.caText = queryValue;
                else if (key == "pinSHA256") candidate.pinSHA256 = queryValue;
                else if (key == "obfsType") candidate.obfsType = queryValue;
                else if (key == "obfsPassword") candidate.obfsPassword = queryValue;
                else if (key == "hopPort") candidate.hopPort = queryValue;
                else if (key == "bbrProfile") candidate.bbrProfile = queryValue;
                else if (key == "hopIntervalMax") candidate.hopIntervalMax = queryValue;
                else if (key == "hopIntervalMin") {
                    if (queryName == "hopInterval") {
                        qint64 seconds;
                        if (!hysteria2LinkInteger(queryValue, &seconds) || seconds > std::numeric_limits<int>::max()) {
                            return hysteria2LinkError(error, QObject::tr("Hysteria2 旧版 hopInterval 必须是有效的非负整数秒"));
                        }
                        candidate.hopInterval = static_cast<int>(seconds);
                    } else {
                        candidate.hopIntervalMin = queryValue;
                    }
                } else if (key == "allowInsecure" || key == "disableChromeParrot" || key == "disableSni" || key == "disableMtuDiscovery") {
                    bool value;
                    if (!hysteria2LinkBoolean(queryValue, &value)) return hysteria2LinkError(error, QObject::tr("Hysteria2 布尔参数 %1 必须是 0、1、false 或 true").arg(queryName));
                    if (key == "allowInsecure") candidate.allowInsecure = value;
                    else if (key == "disableChromeParrot") candidate.disableChromeParrot = value;
                    else if (key == "disableSni") candidate.disableSni = value;
                    else candidate.disableMtuDiscovery = value;
                } else if (key == "geckoMinPacketSize" || key == "geckoMaxPacketSize" || key == "uploadMbps" || key == "downloadMbps" ||
                           key == "streamReceiveWindow" || key == "connectionReceiveWindow") {
                    qint64 value;
                    const bool window = key == "streamReceiveWindow" || key == "connectionReceiveWindow";
                    if (!hysteria2LinkInteger(queryValue, &value) || (!window && value > std::numeric_limits<int>::max())) {
                        return hysteria2LinkError(error, QObject::tr("Hysteria2 参数 %1 必须是范围内的非负整数").arg(queryName));
                    }
                    if (key == "geckoMinPacketSize") candidate.geckoMinPacketSize = static_cast<int>(value);
                    else if (key == "geckoMaxPacketSize") candidate.geckoMaxPacketSize = static_cast<int>(value);
                    else if (key == "uploadMbps") candidate.uploadMbps = static_cast<int>(value);
                    else if (key == "downloadMbps") candidate.downloadMbps = static_cast<int>(value);
                    else if (key == "streamReceiveWindow") candidate.streamReceiveWindow = value;
                    else candidate.connectionReceiveWindow = value;
                } else {
                    return hysteria2LinkError(error, QObject::tr("Hysteria2 链接包含未支持的参数：%1").arg(queryName));
                }
            }
        }
        const auto invalid = candidate.Validate();
        if (!invalid.isEmpty()) return hysteria2LinkError(error, invalid);
        auto state = candidate.ToJson();
        // JsonStore omits empty strings and stores pointers to members. Do
        // not copy the temporary bean; commit a complete replacement state.
        for (const auto &item : candidate._map) {
            if (item->type == itemType::string && !state.contains(item->name)) state[item->name] = "";
        }
        bean->FromJson(state);
        return true;
    }

    // allowInsecure (v2rayN), insecure and allow_insecure are aliases and the
    // first present wins. Only 1/true enable it, so allowInsecure=0 stays off.
    bool streamLinkInsecure(const QUrlQuery &query) {
        for (const auto &key: {"allowInsecure", "insecure", "allow_insecure"}) {
            if (!query.hasQueryItem(key)) continue;
            const auto value = query.queryItemValue(key).trimmed().toLower();
            return value == "1" || value == "true";
        }
        return false;
    }

    // "pcs" pins the server certificate. REALITY authenticates with its own
    // public key, so a pin there is ignored.
    bool streamLinkCertificatePin(const QUrlQuery &query, NekoGui_fmt::V2rayStreamSettings *stream, QString *error) {
        stream->certificate_pin.clear();
        const auto text = query.queryItemValue("pcs");
        if (text.isEmpty() || stream->security != "tls" || !stream->reality_pbk.trimmed().isEmpty()) return true;
        QStringList pins;
        if (!NekoGui_fmt::V2rayStreamSettings::ParseCertificatePins(text, &pins)) {
            if (error != nullptr) *error = QObject::tr("pcs 必须是 64 位十六进制的证书 SHA-256，多个值用逗号分隔");
            return false;
        }
        stream->certificate_pin = pins.join(",");
        return true;
    }
}

namespace NekoGui_fmt {

#define DECODE_V2RAY_N_1                                                                                                        \
    QString linkN = DecodeB64IfValid(SubStrBefore(SubStrAfter(link, "://"), "#"), QByteArray::Base64Option::Base64UrlEncoding); \
    if (linkN.isEmpty()) return false;                                                                                          \
    auto hasRemarks = link.contains("#");                                                                                       \
    if (hasRemarks) linkN += "#" + SubStrAfter(link, "#");                                                                      \
    auto url = QUrl("https://" + linkN);

    bool SocksHttpBean::TryParseLink(const QString &link) {
        auto url = QUrl(link);
        if (!url.isValid()) return false;
        auto query = GetQuery(url);

        if (link.startsWith("socks4")) socks_http_type = type_Socks4;
        if (link.startsWith("http")) socks_http_type = type_HTTP;
        name = url.fragment(QUrl::FullyDecoded);
        serverAddress = url.host();
        serverPort = url.port();
        username = url.userName();
        password = url.password();
        if (serverPort == -1) serverPort = socks_http_type == type_HTTP ? 443 : 1080;

        // v2rayN fmt
        if (password.isEmpty() && !username.isEmpty()) {
            QString n = DecodeB64IfValid(username);
            if (!n.isEmpty()) {
                username = SubStrBefore(n, ":");
                password = SubStrAfter(n, ":");
            }
        }

        stream->security = GetQueryValue(query, "security", "");
        stream->sni = GetQueryValue(query, "sni");
        if (link.startsWith("https")) stream->security = "tls";

        return !serverAddress.isEmpty();
    }

    bool TrojanVLESSBean::TryParseLink(const QString &link, QString *error) {
        auto url = QUrl(link);
        if (!url.isValid()) return false;
        auto query = GetQuery(url);

        name = url.fragment(QUrl::FullyDecoded);
        serverAddress = url.host();
        serverPort = url.port();
        password = url.userName();
        if (serverPort == -1) serverPort = 443;

        // security

        auto type =  GetQueryValue(query, "type", "tcp");
        if (type == "h2") {
            type = "http";
        }
        stream->network = type;

        if (proxy_type == proxy_Trojan) {
            stream->security = GetQueryValue(query, "security", "tls").replace("reality", "tls").replace("none", "");
        } else {
            stream->security = GetQueryValue(query, "security", "").replace("reality", "tls").replace("none", "");
        }
        auto sni1 = GetQueryValue(query, "sni");
        auto sni2 = GetQueryValue(query, "peer");
        if (!sni1.isEmpty()) stream->sni = sni1;
        if (!sni2.isEmpty()) stream->sni = sni2;
        stream->alpn = GetQueryValue(query, "alpn");
        stream->allow_insecure = streamLinkInsecure(query);
        stream->reality_pbk = GetQueryValue(query, "pbk", "");
        stream->reality_sid = GetQueryValue(query, "sid", "");
        stream->reality_spx = GetQueryValue(query, "spx", "");
        if (!streamLinkCertificatePin(query, stream.get(), error)) return false;
        stream->utlsFingerprint = GetQueryValue(query, "fp", "");
        if (stream->utlsFingerprint.isEmpty()) {
            stream->utlsFingerprint = NekoGui::dataStore->utlsFingerprint;
        }

        // type
        if (stream->network == "ws") {
            stream->path = GetQueryValue(query, "path", "");
            stream->host = GetQueryValue(query, "host", "");
        } else if (stream->network == "http") {
            stream->path = GetQueryValue(query, "path", "");
            stream->host = GetQueryValue(query, "host", "").replace("|", ",");
        } else if (stream->network == "httpupgrade") {
            stream->path = GetQueryValue(query, "path", "");
            stream->host = GetQueryValue(query, "host", "");
        } else if (stream->network == "grpc") {
            stream->path = GetQueryValue(query, "serviceName", "");
        } else if (stream->network == "tcp") {
            if (GetQueryValue(query, "headerType") == "http") {
                stream->header_type = "http";
                stream->host = GetQueryValue(query, "host", "");
                stream->path = GetQueryValue(query, "path", "");
            }
        }

        // protocol
        if (proxy_type == proxy_VLESS) {
            flow = GetQueryValue(query, "flow", "");
        }

        return !(password.isEmpty() || serverAddress.isEmpty());
    }

    bool ShadowSocksBean::TryParseLink(const QString &link) {
        if (SubStrBefore(link, "#").contains("@")) {
            // SS
            auto url = QUrl(link);
            if (!url.isValid()) return false;

            name = url.fragment(QUrl::FullyDecoded);
            serverAddress = url.host();
            serverPort = url.port();

            if (url.password().isEmpty()) {
                // traditional format
                auto method_password = DecodeB64IfValid(url.userName(), QByteArray::Base64Option::Base64UrlEncoding);
                if (method_password.isEmpty()) return false;
                method = SubStrBefore(method_password, ":");
                password = SubStrAfter(method_password, ":");
            } else {
                // 2022 format
                method = url.userName();
                password = url.password();
            }

            auto query = GetQuery(url);
            plugin = query.queryItemValue("plugin").replace("simple-obfs;", "obfs-local;");
        } else {
            // v2rayN
            DECODE_V2RAY_N_1

            if (hasRemarks) name = url.fragment(QUrl::FullyDecoded);
            serverAddress = url.host();
            serverPort = url.port();
            method = url.userName();
            password = url.password();
        }
        return !(serverAddress.isEmpty() || method.isEmpty() || password.isEmpty());
    }

    bool VMessBean::TryParseLink(const QString &link, QString *error) {
        // V2RayN Format
        auto linkN = DecodeB64IfValid(SubStrAfter(link, "vmess://"));
        if (!linkN.isEmpty()) {
            auto objN = QString2QJsonObject(linkN);
            if (objN.isEmpty()) return false;
            // REQUIRED
            uuid = objN["id"].toString();
            serverAddress = objN["add"].toString();
            serverPort = objN["port"].toVariant().toInt();
            // OPTIONAL
            name = objN["ps"].toString();
            aid = objN["aid"].toVariant().toInt();
            stream->host = objN["host"].toString();
            stream->path = objN["path"].toString();
            stream->sni = objN["sni"].toString();
            stream->header_type = objN["type"].toString();
            auto net = objN["net"].toString();
            if (!net.isEmpty()) {
                if (net == "h2") {
                    net = "http";
                }
                stream->network = net;
            }
            auto scy = objN["scy"].toString();
            if (!scy.isEmpty()) security = scy;
            // TLS (XTLS?)
            stream->security = objN["tls"].toString();
            QUrlQuery tlsQuery;
            for (const auto &key: {"allowInsecure", "insecure", "allow_insecure", "pcs"}) {
                if (objN.contains(key)) tlsQuery.addQueryItem(key, objN[key].toVariant().toString());
            }
            stream->allow_insecure = streamLinkInsecure(tlsQuery);
            if (!streamLinkCertificatePin(tlsQuery, stream.get(), error)) return false;
            // TODO quic & kcp
            return true;
        } else {
            // https://github.com/XTLS/Xray-core/discussions/716
            auto url = QUrl(link);
            if (!url.isValid()) return false;
            auto query = GetQuery(url);

            name = url.fragment(QUrl::FullyDecoded);
            serverAddress = url.host();
            serverPort = url.port();
            uuid = url.userName();
            if (serverPort == -1) serverPort = 443;

            aid = 0; // “此分享标准仅针对 VMess AEAD 和 VLESS。”
            security = GetQueryValue(query, "encryption", "auto");

            // security
            auto type = GetQueryValue(query, "type", "tcp");
            if (type == "h2") {
                type = "http";
            }
            stream->network = type;
            stream->security = GetQueryValue(query, "security", "tls").replace("reality", "tls");
            auto sni1 = GetQueryValue(query, "sni");
            auto sni2 = GetQueryValue(query, "peer");
            if (!sni1.isEmpty()) stream->sni = sni1;
            if (!sni2.isEmpty()) stream->sni = sni2;
            stream->allow_insecure = streamLinkInsecure(query);
            stream->reality_pbk = GetQueryValue(query, "pbk", "");
            stream->reality_sid = GetQueryValue(query, "sid", "");
            stream->reality_spx = GetQueryValue(query, "spx", "");
            if (!streamLinkCertificatePin(query, stream.get(), error)) return false;
            stream->utlsFingerprint = GetQueryValue(query, "fp", "");
            if (stream->utlsFingerprint.isEmpty()) {
                stream->utlsFingerprint = NekoGui::dataStore->utlsFingerprint;
            }

            // type
            if (stream->network == "ws") {
                stream->path = GetQueryValue(query, "path", "");
                stream->host = GetQueryValue(query, "host", "");
            } else if (stream->network == "http") {
                stream->path = GetQueryValue(query, "path", "");
                stream->host = GetQueryValue(query, "host", "").replace("|", ",");
            } else if (stream->network == "httpupgrade") {
                stream->path = GetQueryValue(query, "path", "");
                stream->host = GetQueryValue(query, "host", "");
            } else if (stream->network == "grpc") {
                stream->path = GetQueryValue(query, "serviceName", "");
            } else if (stream->network == "tcp") {
                if (GetQueryValue(query, "headerType") == "http") {
                    stream->header_type = "http";
                    stream->path = GetQueryValue(query, "path", "");
                    stream->host = GetQueryValue(query, "host", "");
                }
            }
            return !(uuid.isEmpty() || serverAddress.isEmpty());
        }

        return false;
    }

    bool NaiveBean::TryParseLink(const QString &link) {
        auto url = QUrl(link);
        if (!url.isValid()) return false;

        protocol = url.scheme().replace("naive+", "");
        if (protocol != "https" && protocol != "quic") return false;

        name = url.fragment(QUrl::FullyDecoded);
        serverAddress = url.host();
        serverPort = url.port();
        username = url.userName();
        password = url.password();

        return !(username.isEmpty() || password.isEmpty() || serverAddress.isEmpty());
    }

    bool QUICBean::TryParseLink(const QString &link, QString *error) {
        if (error != nullptr) error->clear();
        if (proxy_type == proxy_Hysteria2) return parseHysteria2Link(link, this, error);
        auto url = QUrl(link);
        auto query = QUrlQuery(url.query());
        if (url.host().isEmpty() || url.port() == -1) return false;

        if (url.scheme() == "hysteria") {
            // https://hysteria.network/docs/uri-scheme/
            if (!query.hasQueryItem("upmbps") || !query.hasQueryItem("downmbps")) return false;

            name = url.fragment(QUrl::FullyDecoded);
            serverAddress = url.host();
            serverPort = url.port();
            hopPort = query.queryItemValue("mport");
            obfsPassword = query.queryItemValue("obfsParam");
            allowInsecure = QStringList{"1", "true"}.contains(query.queryItemValue("insecure"));
            uploadMbps = query.queryItemValue("upmbps").toInt();
            downloadMbps = query.queryItemValue("downmbps").toInt();

            auto protocolStr = (query.hasQueryItem("protocol") ? query.queryItemValue("protocol") : "udp").toLower();
            if (protocolStr == "faketcp") {
                hyProtocol = NekoGui_fmt::QUICBean::hysteria_protocol_facktcp;
            } else if (protocolStr.startsWith("wechat")) {
                hyProtocol = NekoGui_fmt::QUICBean::hysteria_protocol_wechat_video;
            }

            if (query.hasQueryItem("auth")) {
                authPayload = query.queryItemValue("auth");
                authPayloadType = NekoGui_fmt::QUICBean::hysteria_auth_string;
            }

            alpn = query.queryItemValue("alpn");
            sni = FIRST_OR_SECOND(query.queryItemValue("peer"), query.queryItemValue("sni"));

            connectionReceiveWindow = query.queryItemValue("recv_window").toInt();
            streamReceiveWindow = query.queryItemValue("recv_window_conn").toInt();
        } else if (url.scheme() == "tuic") {
            // by daeuniverse
            // https://github.com/daeuniverse/dae/discussions/182

            name = url.fragment(QUrl::FullyDecoded);
            serverAddress = url.host();
            if (serverPort == -1) serverPort = 443;
            serverPort = url.port();

            uuid = url.userName();
            password = url.password();

            congestionControl = query.queryItemValue("congestion_control");
            alpn = query.queryItemValue("alpn");
            sni = query.queryItemValue("sni");
            udpRelayMode = query.queryItemValue("udp_relay_mode");
            allowInsecure = query.queryItemValue("allow_insecure") == "1";
            disableSni = query.queryItemValue("disable_sni") == "1";
        }

        return true;
    }

} // namespace NekoGui_fmt
