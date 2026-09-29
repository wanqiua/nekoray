#include "db/ProxyEntity.hpp"
#include "fmt/includes.h"

#include <QUrl>
#include <QUrlQuery>

#define MAKE_SETTINGS_STREAM_SETTINGS                         \
    result.error = stream->CertificatePinError(false);        \
    if (!result.error.isEmpty()) return result;               \
    outbound["settings"] = settings;                          \
    auto streamSettings = stream->BuildStreamSettingsV2Ray(runtimeSettings); \
    outbound["streamSettings"] = streamSettings;

namespace NekoGui_fmt {
    QJsonObject V2rayStreamSettings::BuildStreamSettingsV2Ray(const NekoGui::DataStore *runtimeSettings) {
        QJsonObject streamSettings{{"network", network}};

        if (network == "ws") {
            QJsonObject ws;
            if (!host.isEmpty()) ws["headers"] = QJsonObject{{"Host", host}};
            // ws path & ed
            auto wsPath = path;
            if (ws_early_data_length > 0) {
                QUrl url(wsPath);
                QUrlQuery query(url);
                query.removeAllQueryItems("ed");
                query.addQueryItem("ed", QString::number(ws_early_data_length));
                url.setQuery(query);
                wsPath = url.toString(QUrl::FullyEncoded);
            }
            if (!wsPath.isEmpty()) ws["path"] = wsPath;
            streamSettings["wsSettings"] = ws;
        } else if (network == "httpupgrade") {
            QJsonObject httpupgrade;
            if (!path.isEmpty()) httpupgrade["path"] = path;
            if (!host.isEmpty()) httpupgrade["host"] = host;
            streamSettings["httpupgradeSettings"] = httpupgrade;
        } else if (network == "http") {
            QJsonObject http;
            if (!path.isEmpty()) http["path"] = path;
            if (!host.isEmpty()) http["host"] = QList2QJsonArray(host.split(","));
            streamSettings["httpSettings"] = http;
        } else if (network == "grpc") {
            QJsonObject grpc;
            if (!path.isEmpty()) grpc["serviceName"] = path;
            streamSettings["grpcSettings"] = grpc;
        } else if (network == "kcp") {
            QJsonObject kcp;
            if (!path.isEmpty()) kcp["seed"] = path;
            if (!header_type.isEmpty()) kcp["header"] = QJsonObject{{"type", header_type}};
            streamSettings["kcpSettings"] = kcp;
        } else if (network == "quic") {
            QJsonObject quic;
            if (!header_type.isEmpty()) quic["header"] = QJsonObject{{"type", header_type}};
            if (!path.isEmpty()) quic["key"] = path;
            if (!host.isEmpty()) quic["security"] = host;
            streamSettings["quicSettings"] = quic;
        } else if (network == "tcp" && !header_type.isEmpty()) {
            QJsonObject header{{"type", header_type}};
            if (header_type == "http") {
                header["request"] = QJsonObject{
                    {"path", QList2QJsonArray(path.split(","))},
                    {"headers", QJsonObject{{"Host", QList2QJsonArray(host.split(","))}}},
                };
            }
            streamSettings["tcpSettings"] = QJsonObject{{"header", header}};
        }

        if (security == "tls") {
            QJsonObject tls;
            if (!utlsFingerprint.isEmpty()) tls["fingerprint"] = utlsFingerprint;
            if (!sni.trimmed().isEmpty()) tls["serverName"] = sni;
            if (reality_pbk.trimmed().isEmpty()) {
                if (allow_insecure || (runtimeSettings != nullptr ? runtimeSettings : NekoGui::dataStore)->skip_cert) tls["allowInsecure"] = true;
                // The native certificate pin replaces CA validation and works
                // when the server also sends intermediate certificates.
                if (const auto pins = CertificatePins(); !pins.isEmpty()) {
                    tls["pinnedPeerCertSha256"] = pins.join(",");
                }
                if (!alpn.trimmed().isEmpty()) tls["alpn"] = QList2QJsonArray(alpn.split(","));
                if (!certificate.trimmed().isEmpty()) {
                    tls["disableSystemRoot"] = true;
                    tls["certificates"] = QJsonArray{
                        QJsonObject{
                            {"usage", "verify"},
                            {"certificate", QList2QJsonArray(SplitLines(certificate.trimmed()))},
                        },
                    };
                }
                streamSettings["tlsSettings"] = tls;
                streamSettings["security"] = "tls";
            } else {
                tls["publicKey"] = reality_pbk;
                tls["shortId"] = reality_sid;
                tls["spiderX"] = reality_spx;
                if (utlsFingerprint.isEmpty()) tls["fingerprint"] = "chrome";
                streamSettings["realitySettings"] = tls;
                streamSettings["security"] = "reality";
            }
        }

        return streamSettings;
    }

    CoreObjOutboundBuildResult SocksHttpBean::BuildCoreObjV2Ray(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;

        QJsonObject outbound;
        outbound["protocol"] = socks_http_type == type_HTTP ? "http" : "socks";

        QJsonObject settings;
        if (socks_http_type == type_Socks4) settings["version"] = "4";
        else if (socks_http_type == type_Socks5) settings["version"] = "5";
        QJsonArray servers;
        QJsonObject server;

        server["address"] = serverAddress;
        server["port"] = serverPort;

        QJsonArray users;
        QJsonObject user;
        user["user"] = username;
        user["pass"] = password;
        users.push_back(user);
        if (!username.isEmpty() || !password.isEmpty()) server["users"] = users;

        servers.push_back(server);
        settings["servers"] = servers;

        MAKE_SETTINGS_STREAM_SETTINGS

        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult ShadowSocksBean::BuildCoreObjV2Ray(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;

        QJsonObject outbound{{"protocol", "shadowsocks"}};

        QJsonObject settings;
        QJsonArray servers;
        QJsonObject server;

        server["address"] = serverAddress;
        server["port"] = serverPort;
        server["method"] = method;
        server["password"] = password;

        if (uot != 0) {
            server["uot"] = true;
            server["UoTVersion"] = uot;
        } else {
            server["uot"] = false;
        }

        servers.push_back(server);
        settings["servers"] = servers;

        if (!plugin.trimmed().isEmpty()) {
            settings["plugin"] = SubStrBefore(plugin, ";");
            settings["pluginOpts"] = SubStrAfter(plugin, ";");
        }

        MAKE_SETTINGS_STREAM_SETTINGS

        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult VMessBean::BuildCoreObjV2Ray(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;
        QJsonObject outbound{{"protocol", "vmess"}};

        QJsonObject settings{
            {"vnext", QJsonArray{
                          QJsonObject{
                              {"address", serverAddress},
                              {"port", serverPort},
                              {"users", QJsonArray{
                                            QJsonObject{
                                                {"id", uuid.trimmed()},
                                                {"alterId", aid},
                                                {"security", security},
                                            }}},
                          }}}};

        MAKE_SETTINGS_STREAM_SETTINGS

        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult TrojanVLESSBean::BuildCoreObjV2Ray(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;
        QJsonObject outbound{
            {"protocol", proxy_type == proxy_VLESS ? "vless" : "trojan"},
        };

        QJsonObject settings;
        if (proxy_type == proxy_VLESS) {
            if (stream->packet_encoding == "xudp") {
                // Xray configures XUDP through mux, independently of TCP mux.
                // Preserve the explicit UDP encoding even when TCP mux is off.
                outbound["mux"] = QJsonObject{{"enabled", false}, {"concurrency", -1},
                                               {"xudpConcurrency", 8}, {"xudpProxyUDP443", "allow"}};
            }
            const auto normalizedFlow = flow == "none" ? QString() : flow;
            settings = QJsonObject{
                {"vnext", QJsonArray{
                              QJsonObject{
                                  {"address", serverAddress},
                                  {"port", serverPort},
                                  {"users", QJsonArray{
                                                QJsonObject{
                                                    {"id", password.trimmed()},
                                                    {"encryption", "none"},
                                                    {"flow", normalizedFlow},
                                                }}},
                              }}}};
        } else {
            settings = QJsonObject{
                {"servers", QJsonArray{
                                QJsonObject{
                                    {"address", serverAddress},
                                    {"port", serverPort},
                                    {"password", password},
                                }}}};
        }

        MAKE_SETTINGS_STREAM_SETTINGS

        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult CustomBean::BuildCoreObjV2Ray(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;

        if (core == "internal") {
            result.outbound = QString2QJsonObject(config_simple);
        }

        return result;
    }
} // namespace NekoGui_fmt
