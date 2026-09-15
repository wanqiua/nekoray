#include "db/ProxyEntity.hpp"
#include "fmt/includes.h"

#include <QHostAddress>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QUrl>

#include <limits>

namespace {
    bool hysteria2HostValid(const QString &host) {
        if (host.isEmpty() || host.contains(QRegularExpression("[\\s/\\\\@?#\\[\\]]")) || host.contains(QChar(0))) return false;
        QHostAddress address;
        if (address.setAddress(host)) return true;
        if (host.contains(':')) return false;
        QUrl url;
        url.setScheme("https");
        url.setHost(host);
        return url.isValid() && !url.host().isEmpty();
    }

    // Match non-negative Go durations without floating-point rounding at
    // the 5s boundary or the signed int64 nanosecond limit.
    bool hysteria2Duration(const QString &duration, quint64 *nanoseconds) {
        *nanoseconds = 0;
        if (duration.isEmpty()) return true;
        auto text = duration;
        const bool negative = text.startsWith('-');
        if (negative || text.startsWith('+')) text.remove(0, 1);
        if (text == "0") return true;
        static const QRegularExpression component("([0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(ns|us|µs|μs|ms|s|m|h)");
        const QMap<QString, quint64> units{
            {"ns", 1}, {"us", 1000}, {"µs", 1000}, {"μs", 1000}, {"ms", 1000000},
            {"s", 1000000000}, {"m", 60000000000ULL}, {"h", 3600000000000ULL},
        };
        const auto maximum = static_cast<quint64>(std::numeric_limits<qint64>::max());
        quint64 total = 0;
        qsizetype consumed = 0;
        auto matches = component.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (match.capturedStart() != consumed) return false;
            consumed = match.capturedEnd();
            const auto number = match.captured(1);
            const auto scale = units.value(match.captured(2));
            const auto dot = number.indexOf('.');
            const auto whole = dot == -1 ? number : number.left(dot);
            bool valid = true;
            const quint64 integral = whole.isEmpty() ? 0 : whole.toULongLong(&valid);
            if (!valid || integral > maximum / scale) return false;
            quint64 fraction = 0;
            if (dot != -1) {
                for (auto index = number.size() - 1; index > dot; --index) {
                    fraction = (fraction + static_cast<quint64>(number[index].digitValue()) * scale) / 10;
                }
            }
            const auto integralNs = integral * scale;
            if (fraction > maximum - integralNs) return false;
            const auto part = integralNs + fraction;
            if (part > maximum - total) return false;
            total += part;
        }
        if (consumed != text.size() || text.isEmpty() || (negative && total != 0)) return false;
        *nanoseconds = total;
        return true;
    }
}

namespace NekoGui_fmt {
    void V2rayStreamSettings::BuildStreamSettingsSingBox(QJsonObject *outbound, const NekoGui::DataStore *runtimeSettings) {
        // https://sing-box.sagernet.org/configuration/shared/v2ray-transport

        if (network != "tcp") {
            QJsonObject transport{{"type", network}};
            if (network == "ws") {
                if (!host.isEmpty()) transport["headers"] = QJsonObject{{"Host", host}};
                // ws path & ed
                auto pathWithoutEd = SubStrBefore(path, "?ed=");
                if (!pathWithoutEd.isEmpty()) transport["path"] = pathWithoutEd;
                if (pathWithoutEd != path) {
                    auto ed = SubStrAfter(path, "?ed=").toInt();
                    if (ed > 0) {
                        transport["max_early_data"] = ed;
                        transport["early_data_header_name"] = "Sec-WebSocket-Protocol";
                    }
                }
                if (ws_early_data_length > 0) {
                    transport["max_early_data"] = ws_early_data_length;
                    transport["early_data_header_name"] = ws_early_data_name;
                }
            } else if (network == "http") {
                if (!path.isEmpty()) transport["path"] = path;
                if (!host.isEmpty()) transport["host"] = QList2QJsonArray(host.split(","));
            } else if (network == "grpc") {
                if (!path.isEmpty()) transport["service_name"] = path;
            } else if (network == "httpupgrade") {
                if (!path.isEmpty()) transport["path"] = path;
                if (!host.isEmpty()) transport["host"] = host;
            }
            outbound->insert("transport", transport);
        }

        // 对应字段 tls
        if (security == "tls") {
            QJsonObject tls{{"enabled", true}};
            if (allow_insecure || (runtimeSettings != nullptr ? runtimeSettings : NekoGui::dataStore)->skip_cert) tls["insecure"] = true;
            if (!sni.trimmed().isEmpty()) tls["server_name"] = sni;
            if (!certificate.trimmed().isEmpty()) {
                tls["certificate"] = certificate.trimmed();
            }
            if (!alpn.trimmed().isEmpty()) {
                tls["alpn"] = QList2QJsonArray(alpn.split(","));
            }
            QString fp = utlsFingerprint;
            if (!reality_pbk.trimmed().isEmpty()) {
                tls["reality"] = QJsonObject{
                    {"enabled", true},
                    {"public_key", reality_pbk},
                    {"short_id", reality_sid.split(",")[0]},
                };
                if (fp.isEmpty()) fp = "chrome";
            }
            if (!fp.isEmpty()) {
                tls["utls"] = QJsonObject{
                    {"enabled", true},
                    {"fingerprint", fp},
                };
            }
            outbound->insert("tls", tls);
        }

        if (outbound->value("type").toString() == "vmess" || outbound->value("type").toString() == "vless") {
            outbound->insert("packet_encoding", packet_encoding);
        }
    }

    CoreObjOutboundBuildResult SocksHttpBean::BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;
        if (stream->network != "tcp" || (!stream->header_type.isEmpty() && stream->header_type != "none") ||
            (socks_http_type != type_HTTP && stream->security == "tls")) {
            result.error = QObject::tr("sing-box SOCKS/HTTP 无法保留此 TLS 或传输组合");
            return result;
        }

        QJsonObject outbound;
        outbound["type"] = socks_http_type == type_HTTP ? "http" : "socks";
        if (socks_http_type == type_Socks4) outbound["version"] = "4";
        outbound["server"] = serverAddress;
        outbound["server_port"] = serverPort;

        if (!username.isEmpty() || !password.isEmpty()) {
            outbound["username"] = username;
            outbound["password"] = password;
        }

        stream->BuildStreamSettingsSingBox(&outbound, runtimeSettings);
        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult ShadowSocksBean::BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;
        if (stream->network != "tcp" || (!stream->header_type.isEmpty() && stream->header_type != "none") || stream->security == "tls") {
            result.error = QObject::tr("sing-box Shadowsocks 无法直接应用 TLS 或 V2Ray transport");
            return result;
        }

        QJsonObject outbound{{"type", "shadowsocks"}};
        outbound["server"] = serverAddress;
        outbound["server_port"] = serverPort;
        outbound["method"] = method;
        outbound["password"] = password;

        if (uot != 0) {
            QJsonObject udp_over_tcp{
                {"enabled", true},
                {"version", uot},
            };
            outbound["udp_over_tcp"] = udp_over_tcp;
        } else {
            outbound["udp_over_tcp"] = false;
        }

        if (!plugin.trimmed().isEmpty()) {
            outbound["plugin"] = SubStrBefore(plugin, ";");
            outbound["plugin_opts"] = SubStrAfter(plugin, ";");
        }

        stream->BuildStreamSettingsSingBox(&outbound, runtimeSettings);
        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult VMessBean::BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;

        QJsonObject outbound{
            {"type", "vmess"},
            {"server", serverAddress},
            {"server_port", serverPort},
            {"uuid", uuid.trimmed()},
            {"alter_id", aid},
            {"security", security},
        };

        stream->BuildStreamSettingsSingBox(&outbound, runtimeSettings);
        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult TrojanVLESSBean::BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;

        QJsonObject outbound{
            {"type", proxy_type == proxy_VLESS ? "vless" : "trojan"},
            {"server", serverAddress},
            {"server_port", serverPort},
        };

        QJsonObject settings;
        if (proxy_type == proxy_VLESS) {
            const auto normalizedFlow = flow == "none" ? QString() : flow;
            outbound["uuid"] = password.trimmed();
            outbound["flow"] = normalizedFlow;
        } else {
            outbound["password"] = password;
        }

        stream->BuildStreamSettingsSingBox(&outbound, runtimeSettings);
        result.outbound = outbound;
        return result;
    }

    QStringList QUICBean::Hysteria2ServerPorts(QString *error) const {
        if (error != nullptr) error->clear();
        QStringList ports;
        if (hopPort.trimmed().isEmpty()) return ports;
        static const QRegularExpression range("^([0-9]+)(?:\\s*[-:]\\s*([0-9]+))?$");
        for (const auto &part : hopPort.split(',')) {
            const auto match = range.match(part.trimmed());
            bool startValid = false, endValid = false;
            const auto start = match.captured(1).toUInt(&startValid);
            const auto end = match.captured(2).isEmpty() ? start : match.captured(2).toUInt(&endValid);
            if (match.captured(2).isEmpty()) endValid = startValid;
            if (!match.hasMatch() || !startValid || !endValid || start < 1 || start > end || end > 65535) {
                if (error != nullptr) *error = QObject::tr("Hysteria2 跳端口必须是 1 到 65535 的端口或递增范围，例如 443,2000-3000；不能包含空项");
                return {};
            }
            // sing-quic requires a colon even for a singleton port.
            ports.append(QString::number(start) + ":" + QString::number(end));
        }
        return ports;
    }

    QString QUICBean::Hysteria2ObfsType() const {
        if (obfsType == "none") return {};
        if (!obfsType.isEmpty()) return obfsType;
        return obfsPassword.isEmpty() ? QString() : QStringLiteral("salamander");
    }

    QString QUICBean::Hysteria2HopInterval() const {
        if (!hopIntervalMin.isEmpty()) return hopIntervalMin;
        return hopInterval == 0 ? QString() : QString::number(hopInterval) + "s";
    }

    QString QUICBean::Validate() const {
        if (proxy_type != proxy_Hysteria2) return {};
        if (!hysteria2HostValid(serverAddress)) return QObject::tr("Hysteria2 服务器地址必须是有效的域名或 IP，不含端口、路径或空白");
        QString portError;
        const auto ports = Hysteria2ServerPorts(&portError);
        if (!portError.isEmpty()) return portError;
        if (serverPort < (ports.isEmpty() ? 1 : 0) || serverPort > 65535) return QObject::tr("Hysteria2 端口必须在 1 到 65535 之间；使用跳端口时可为 0");
        if (uploadMbps < 0 || downloadMbps < 0) return QObject::tr("Hysteria2 带宽不能为负数；0 表示使用 BBR");
        if (streamReceiveWindow < 0 || connectionReceiveWindow < 0) return QObject::tr("Hysteria2 接收窗口不能为负数");
        if (hopInterval < 0) return QObject::tr("Hysteria2 跳端口间隔必须是非负整数秒；0 表示使用默认值");
        if (!bbrProfile.isEmpty() && bbrProfile != "standard" && bbrProfile != "conservative" && bbrProfile != "aggressive") {
            return QObject::tr("Hysteria2 BBR 配置只支持 standard、conservative 或 aggressive");
        }
        if (!obfsType.isEmpty() && obfsType != "none" && obfsType != "salamander" && obfsType != "gecko") {
            return QObject::tr("Hysteria2 混淆只支持 salamander、gecko 或关闭");
        }
        const auto obfs = Hysteria2ObfsType();
        if (!obfs.isEmpty() && obfsPassword.isEmpty()) return QObject::tr("Hysteria2 启用混淆时必须填写混淆密码");
        if (obfsType == "none" && !obfsPassword.isEmpty()) return QObject::tr("Hysteria2 已关闭混淆，请清空混淆密码或选择混淆类型");
        if (obfs == "gecko") {
            const auto minimum = geckoMinPacketSize == 0 ? 512 : geckoMinPacketSize;
            const auto maximum = geckoMaxPacketSize == 0 ? 1200 : geckoMaxPacketSize;
            if (minimum < 1 || minimum > maximum || maximum > 2048) return QObject::tr("Hysteria2 Gecko 包长必须满足 1 ≤ 最小值 ≤ 最大值 ≤ 2048；0 使用默认值 512/1200");
        } else if ((geckoMinPacketSize != 0 && geckoMinPacketSize != 512) ||
                   (geckoMaxPacketSize != 0 && geckoMaxPacketSize != 1200)) {
            return QObject::tr("Hysteria2 自定义包长仅适用于 Gecko 混淆");
        }
        if (forceExternal) {
            const auto externalError = Hysteria2ExternalCompatibilityError();
            if (!externalError.isEmpty()) return externalError;
        }
        if (!sni.isEmpty() && !hysteria2HostValid(sni)) return QObject::tr("Hysteria2 TLS 服务器名称必须是有效的域名或 IP");
        if (!caText.trimmed().isEmpty() && QSslCertificate::fromData(caText.toUtf8(), QSsl::Pem).isEmpty()) {
            return QObject::tr("Hysteria2 自定义 CA 必须包含有效的 PEM 证书");
        }
        if (!alpn.isEmpty()) {
            for (const auto &entry : alpn.split(',')) {
                const auto token = entry.trimmed();
                if (token.isEmpty() || token.toUtf8().size() > 255 || token.contains(QChar(0)) || token.contains('\r') || token.contains('\n')) {
                    return QObject::tr("Hysteria2 ALPN 必须是逗号分隔的非空协议名，每项不超过 255 字节");
                }
            }
        }
        if (ports.isEmpty()) {
            if (!hopIntervalMin.isEmpty() || !hopIntervalMax.isEmpty()) return QObject::tr("Hysteria2 设置跳端口间隔前必须填写跳端口范围");
            return {};
        }
        quint64 minimum, maximum;
        if (!hysteria2Duration(Hysteria2HopInterval(), &minimum) || !hysteria2Duration(hopIntervalMax, &maximum)) {
            return QObject::tr("Hysteria2 跳端口间隔必须是有效时长，例如 10s 或 1m30s，且不能为负数或溢出");
        }
        if (minimum == 0 && maximum > 0) return QObject::tr("Hysteria2 使用随机跳端口间隔时必须同时设置最小间隔");
        if (minimum != 0 && minimum < 5000000000ULL) return QObject::tr("Hysteria2 跳端口最小间隔不能小于 5 秒");
        if (maximum != 0 && maximum < minimum) return QObject::tr("Hysteria2 跳端口最大间隔不能小于最小间隔");
        return {};
    }

    QString QUICBean::Hysteria2ExternalCompatibilityError() const {
        if (proxy_type != proxy_Hysteria2) return {};
        if ((!bbrProfile.isEmpty() && bbrProfile != "standard") || Hysteria2ObfsType() == "gecko" ||
            disableChromeParrot || !hopIntervalMax.isEmpty()) {
            return QObject::tr("Hysteria2 的 BBR 配置、Gecko、Chrome Parrot 或随机跳端口选项需要 sing-box 原生支持，不能交给外置核心");
        }
        return {};
    }

    CoreObjOutboundBuildResult QUICBean::BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;
        if (proxy_type == proxy_Hysteria2) {
            result.error = Validate();
            if (!result.error.isEmpty()) return result;
        }

        const auto *settings = runtimeSettings != nullptr ? runtimeSettings : NekoGui::dataStore;

        QJsonObject coreTlsObj{
            {"enabled", true},
            {"disable_sni", disableSni},
            {"insecure", allowInsecure || (settings != nullptr && settings->skip_cert)},
            {"certificate", caText.trimmed()},
            {"server_name", sni},
        };
        if (!alpn.trimmed().isEmpty()) coreTlsObj["alpn"] = QList2QJsonArray(alpn.split(","));
        if (proxy_type == proxy_Hysteria2 && alpn.trimmed().isEmpty()) coreTlsObj["alpn"] = QJsonArray{"h3"};

        QJsonObject outbound{
            {"server", serverAddress},
            {"server_port", serverPort},
            {"tls", coreTlsObj},
        };

        if (proxy_type == proxy_Hysteria) {
            outbound["type"] = "hysteria";
            outbound["obfs"] = obfsPassword;
            outbound["disable_path_mtu_discovery"] = disableMtuDiscovery;
            if (streamReceiveWindow > 0) outbound["stream_receive_window"] = streamReceiveWindow;
            if (connectionReceiveWindow > 0) outbound["connection_receive_window"] = connectionReceiveWindow;
            outbound["up_mbps"] = uploadMbps;
            outbound["down_mbps"] = downloadMbps;

            if (!hopPort.trimmed().isEmpty()) {
                outbound["hop_ports"] = hopPort;
                outbound["hop_interval"] = hopInterval;
            }
            if (authPayloadType == hysteria_auth_base64) outbound["auth"] = authPayload;
            if (authPayloadType == hysteria_auth_string) outbound["auth_str"] = authPayload;
        } else if (proxy_type == proxy_Hysteria2) {
            outbound["type"] = "hysteria2";
            outbound["password"] = password;
            outbound["up_mbps"] = uploadMbps;
            outbound["down_mbps"] = downloadMbps;
            outbound["disable_path_mtu_discovery"] = disableMtuDiscovery;
            if (streamReceiveWindow > 0) outbound["stream_receive_window"] = streamReceiveWindow;
            if (connectionReceiveWindow > 0) outbound["connection_receive_window"] = connectionReceiveWindow;
            outbound["bbr_profile"] = bbrProfile.isEmpty() ? "standard" : bbrProfile;
            outbound["disable_chrome_parrot"] = disableChromeParrot;
            const auto ports = Hysteria2ServerPorts();
            if (!ports.isEmpty()) {
                outbound.remove("server_port");
                outbound["server_ports"] = QJsonArray::fromStringList(ports);
                if (!Hysteria2HopInterval().isEmpty()) outbound["hop_interval"] = Hysteria2HopInterval();
                if (!hopIntervalMax.isEmpty()) outbound["hop_interval_max"] = hopIntervalMax;
            }
            const auto obfsType = Hysteria2ObfsType();
            if (!obfsType.isEmpty()) {
                QJsonObject obfs{{"type", obfsType}, {"password", obfsPassword}};
                if (obfsType == "gecko") {
                    obfs["min_packet_size"] = geckoMinPacketSize == 0 ? 512 : geckoMinPacketSize;
                    obfs["max_packet_size"] = geckoMaxPacketSize == 0 ? 1200 : geckoMaxPacketSize;
                }
                outbound["obfs"] = obfs;
            }
            if (!alpn.trimmed().isEmpty()) {
                QJsonArray protocols;
                for (const auto &entry : alpn.split(',')) protocols.append(entry.trimmed());
                coreTlsObj["alpn"] = protocols;
                outbound["tls"] = coreTlsObj;
            }
        } else if (proxy_type == proxy_TUIC) {
            outbound["type"] = "tuic";
            outbound["uuid"] = uuid;
            outbound["password"] = password;
            outbound["congestion_control"] = congestionControl;
            if (uos) {
                outbound["udp_over_stream"] = true;
            } else {
                outbound["udp_relay_mode"] = udpRelayMode;
            }
            outbound["zero_rtt_handshake"] = zeroRttHandshake;
            if (!heartbeat.trimmed().isEmpty()) outbound["heartbeat"] = heartbeat;
        }

        result.outbound = outbound;
        return result;
    }

    CoreObjOutboundBuildResult CustomBean::BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;

        if (core == "internal") {
            result.outbound = QString2QJsonObject(config_simple);
        }

        return result;
    }
} // namespace NekoGui_fmt
