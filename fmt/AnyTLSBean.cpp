#include "fmt/AnyTLSBean.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSslCertificate>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <limits>

namespace {
    bool anytlsError(QString *error, const QString &message) {
        if (error != nullptr) *error = message;
        return false;
    }

    bool anytlsBoolean(const QString &text, bool *value) {
        if (text == "1" || text == "true") { *value = true; return true; }
        if (text == "0" || text == "false") { *value = false; return true; }
        return false;
    }

    bool anytlsDecodeComponent(const QString &encoded, QString *decoded) {
        const auto bytes = QByteArray::fromPercentEncoding(encoded.toLatin1());
        *decoded = QString::fromUtf8(bytes);
        return decoded->toUtf8() == bytes;
    }

    bool anytlsHostValid(const QString &host) {
        if (host.isEmpty() || host.contains(QRegularExpression("[\\s/\\\\@?#\\[\\]]")) || host.contains(QChar(0))) return false;
        QHostAddress address;
        if (address.setAddress(host)) return true;
        if (host.contains(':')) return false;
        QUrl url;
        url.setScheme("https");
        url.setHost(host);
        return url.isValid() && !url.host().isEmpty();
    }

    // Parse positive Go durations in integer nanoseconds. Keep the same
    // int64 limit, truncate fractional nanoseconds, and reject <=5s: the
    // AnyTLS library silently replaces those values with its 30s defaults.
    bool anytlsDurationValid(const QString &duration) {
        if (duration.isEmpty()) return true;
        auto text = duration;
        if (text.startsWith('+')) text.remove(0, 1);
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
                // Reverse decimal evaluation avoids floating-point rounding
                // across the 5s boundary and cannot overflow a unit scale.
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
        return consumed == text.size() && total > 5000000000ULL;
    }

    bool anytlsShareObject(const QUrl &url, NekoGui_fmt::AnyTLSBean *bean, QString *error) {
        if (url.host() != "anytls" || !url.userInfo().isEmpty() || url.port() != -1 ||
            !url.path().isEmpty() || url.hasQuery()) {
            return anytlsError(error, QObject::tr("AnyTLS 完整分享链接格式无效"));
        }
        const auto encoded = url.fragment(QUrl::FullyDecoded).toLatin1();
        if (!QRegularExpression("^[A-Za-z0-9_-]+={0,2}$").match(QString::fromLatin1(encoded)).hasMatch()) {
            return anytlsError(error, QObject::tr("AnyTLS 分享内容不是有效的 Base64URL"));
        }
        const auto bytes = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding);
        const auto canonical = bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        auto unpadded = encoded;
        while (unpadded.endsWith('=')) unpadded.chop(1);
        if (canonical != unpadded) return anytlsError(error, QObject::tr("AnyTLS 分享内容的 Base64URL 编码无效"));
        QJsonParseError jsonError;
        const auto document = QJsonDocument::fromJson(bytes, &jsonError);
        if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
            return anytlsError(error, QObject::tr("AnyTLS 分享内容不是有效的 JSON 对象"));
        }
        const auto object = document.object();
        for (const auto &required : {"addr", "port", "password"}) {
            if (!object.contains(required)) return anytlsError(error, QObject::tr("AnyTLS 分享缺少字段：%1").arg(required));
        }
        for (auto item = object.begin(); item != object.end(); ++item) {
            const auto field = bean->_map.value(item.key());
            if (!field) return anytlsError(error, QObject::tr("AnyTLS 分享包含未支持的字段：%1").arg(item.key()));
            const auto value = item.value();
            const bool valid = (field->type == itemType::string && value.isString()) ||
                               (field->type == itemType::boolean && value.isBool()) ||
                               (field->type == itemType::integer && value.isDouble() &&
                                std::floor(value.toDouble()) == value.toDouble() &&
                                value.toDouble() >= std::numeric_limits<int>::min() &&
                                value.toDouble() <= std::numeric_limits<int>::max());
            if (!valid) return anytlsError(error, QObject::tr("AnyTLS 分享字段类型无效：%1").arg(item.key()));
        }
        if (object.contains("_v") && object.value("_v").toInt() != 0) {
            return anytlsError(error, QObject::tr("不支持此 AnyTLS 节点数据版本"));
        }
        bean->FromJson(object);
        return true;
    }

    QString anytlsQueryKey(const QString &key) {
        static const QMap<QString, QString> aliases{
            {"peer", "sni"}, {"server_name", "sni"},
            {"insecure", "allowInsecure"}, {"allow_insecure", "allowInsecure"}, {"skip-cert-verify", "allowInsecure"},
            {"fp", "utlsFingerprint"}, {"fingerprint", "utlsFingerprint"}, {"utls_fingerprint", "utlsFingerprint"},
            {"ca", "caText"}, {"certificate", "caText"},
            {"idle_session_check_interval", "idleSessionCheckInterval"}, {"idle-session-check-interval", "idleSessionCheckInterval"},
            {"idle_session_timeout", "idleSessionTimeout"}, {"idle-session-timeout", "idleSessionTimeout"},
            {"min_idle_session", "minIdleSession"}, {"min-idle-session", "minIdleSession"},
            {"client_metadata", "clientMetadata"}, {"client-metadata", "clientMetadata"},
        };
        return aliases.value(key, key);
    }
}

namespace NekoGui_fmt {
    AnyTLSBean::AnyTLSBean() : AbstractBean(0) {
        serverPort = 443;
        _add(new configItem("password", &password, itemType::string));
        _add(new configItem("allowInsecure", &allowInsecure, itemType::boolean));
        _add(new configItem("sni", &sni, itemType::string));
        _add(new configItem("caText", &caText, itemType::string));
        _add(new configItem("alpn", &alpn, itemType::string));
        _add(new configItem("utlsFingerprint", &utlsFingerprint, itemType::string));
        _add(new configItem("idleSessionCheckInterval", &idleSessionCheckInterval, itemType::string));
        _add(new configItem("idleSessionTimeout", &idleSessionTimeout, itemType::string));
        _add(new configItem("minIdleSession", &minIdleSession, itemType::integer));
        _add(new configItem("clientMetadata", &clientMetadata, itemType::string));
    }

    QString AnyTLSBean::Validate() const {
        if (!anytlsHostValid(serverAddress)) return QObject::tr("AnyTLS 服务器地址必须是有效的域名或 IP，不含端口、路径或空白");
        if (serverPort < 1 || serverPort > 65535) return QObject::tr("AnyTLS 端口必须在 1 到 65535 之间");
        if (password.isEmpty()) return QObject::tr("AnyTLS 密码不能为空");
        if (!sni.isEmpty() && !anytlsHostValid(sni)) return QObject::tr("AnyTLS TLS 服务器名称必须是有效的域名或 IP");
        if (!caText.trimmed().isEmpty() && QSslCertificate::fromData(caText.toUtf8(), QSsl::Pem).isEmpty()) {
            return QObject::tr("AnyTLS 自定义 CA 必须包含有效的 PEM 证书");
        }
        if (!alpn.isEmpty()) {
            for (const auto &entry : alpn.split(',')) {
                const auto token = entry.trimmed();
                if (token.isEmpty() || token.toUtf8().size() > 255 || token.contains(QChar(0)) || token.contains('\r') || token.contains('\n')) {
                    return QObject::tr("AnyTLS ALPN 必须是逗号分隔的非空协议名，每项不超过 255 字节");
                }
            }
        }
        static const QSet<QString> fingerprints{
            "chrome", "chrome_psk", "chrome_psk_shuffle", "chrome_padding_psk_shuffle", "chrome_pq", "chrome_pq_psk",
            "firefox", "edge", "safari", "360", "qq", "ios", "android", "random", "randomized",
        };
        if (!utlsFingerprint.isEmpty() && !fingerprints.contains(utlsFingerprint)) return QObject::tr("AnyTLS uTLS 指纹未被当前 sing-box 支持：%1").arg(utlsFingerprint);
        if (!anytlsDurationValid(idleSessionCheckInterval)) return QObject::tr("AnyTLS 空闲检查间隔必须留空或为大于 5 秒的有效时长，例如 10s 或 1m30s");
        if (!anytlsDurationValid(idleSessionTimeout)) return QObject::tr("AnyTLS 空闲会话超时必须留空或为大于 5 秒的有效时长，例如 30s");
        if (minIdleSession < 0) return QObject::tr("AnyTLS 最少保留的空闲会话数不能为负数");
        if (clientMetadata.contains(QChar(0)) || clientMetadata.contains('\r') || clientMetadata.contains('\n')) {
            return QObject::tr("AnyTLS 客户端标识不能包含换行或空字符");
        }
        // sing-anytls v0.0.11 adds 56 bytes for v, padding-md5 and delimiters
        // to the settings frame, whose payload length is an unsigned uint16.
        if (clientMetadata.toUtf8().size() > 65535 - 56) return QObject::tr("AnyTLS 客户端标识过长，不能超过 65479 个 UTF-8 字节");
        return {};
    }

    CoreObjOutboundBuildResult AnyTLSBean::BuildCoreObjV2Ray(const NekoGui::DataStore *) {
        return {{}, QObject::tr("Xray 不支持 AnyTLS，请使用 sing-box")};
    }

    CoreObjOutboundBuildResult AnyTLSBean::BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings) {
        CoreObjOutboundBuildResult result;
        result.error = Validate();
        if (!result.error.isEmpty()) return result;
        const auto *settings = runtimeSettings != nullptr ? runtimeSettings : NekoGui::dataStore;
        QJsonObject tls{{"enabled", true}, {"insecure", allowInsecure || (settings != nullptr && settings->skip_cert)}};
        if (!sni.isEmpty()) tls["server_name"] = sni;
        if (!caText.trimmed().isEmpty()) tls["certificate"] = QJsonArray{caText};
        if (!alpn.isEmpty()) {
            QJsonArray protocols;
            for (const auto &entry : alpn.split(',')) protocols.append(entry.trimmed());
            tls["alpn"] = protocols;
        }
        if (!utlsFingerprint.isEmpty()) tls["utls"] = QJsonObject{{"enabled", true}, {"fingerprint", utlsFingerprint}};
        result.outbound = QJsonObject{
            {"type", "anytls"}, {"server", serverAddress}, {"server_port", serverPort},
            {"password", password}, {"tls", tls}, {"min_idle_session", minIdleSession},
        };
        if (!idleSessionCheckInterval.isEmpty()) result.outbound["idle_session_check_interval"] = idleSessionCheckInterval;
        if (!idleSessionTimeout.isEmpty()) result.outbound["idle_session_timeout"] = idleSessionTimeout;
        if (!clientMetadata.isEmpty()) result.outbound["client_metadata"] = clientMetadata;
        return result;
    }

    bool AnyTLSBean::TryParseLink(const QString &link, QString *error) {
        if (error != nullptr) error->clear();
        const QUrl url(link.trimmed(), QUrl::StrictMode);
        if (!url.isValid()) return anytlsError(error, QObject::tr("AnyTLS 链接格式无效"));
        AnyTLSBean candidate;
        if (url.scheme() == "nekoray") {
            if (!anytlsShareObject(url, &candidate, error)) return false;
        } else {
            if (url.scheme() != "anytls" || (!url.path().isEmpty() && url.path() != "/")) {
                return anytlsError(error, QObject::tr("AnyTLS 链接必须使用 anytls://，且不能包含路径"));
            }
            candidate.serverAddress = url.host(QUrl::FullyDecoded);
            candidate.serverPort = url.port(443);
            // Qt does not support FullyDecoded for userInfo(), because it
            // cannot distinguish an encoded colon from its user/password
            // separator. This URI treats the entire component as a password.
            if (!anytlsDecodeComponent(url.userInfo(QUrl::FullyEncoded), &candidate.password)) return anytlsError(error, QObject::tr("AnyTLS 密码包含无效的 UTF-8 编码"));
            if (!anytlsDecodeComponent(url.fragment(QUrl::FullyEncoded), &candidate.name)) return anytlsError(error, QObject::tr("AnyTLS 节点名称包含无效的 UTF-8 编码"));
            QSet<QString> seen;
            // Decode each component once. Decoding the whole query first
            // would turn escaped '&'/'=' in metadata into new parameters.
            for (const auto &item : QUrlQuery(url).queryItems(QUrl::FullyEncoded)) {
                QString queryName, queryValue;
                if (!anytlsDecodeComponent(item.first, &queryName) || !anytlsDecodeComponent(item.second, &queryValue)) {
                    return anytlsError(error, QObject::tr("AnyTLS 链接参数包含无效的 UTF-8 编码"));
                }
                const auto key = anytlsQueryKey(queryName);
                if (seen.contains(key)) return anytlsError(error, QObject::tr("AnyTLS 链接包含重复或冲突参数：%1").arg(queryName));
                seen.insert(key);
                if (key == "sni") candidate.sni = queryValue;
                else if (key == "alpn") candidate.alpn = queryValue;
                else if (key == "caText") candidate.caText = queryValue;
                else if (key == "utlsFingerprint") candidate.utlsFingerprint = queryValue;
                else if (key == "idleSessionCheckInterval") candidate.idleSessionCheckInterval = queryValue;
                else if (key == "idleSessionTimeout") candidate.idleSessionTimeout = queryValue;
                else if (key == "clientMetadata") candidate.clientMetadata = queryValue;
                else if (key == "allowInsecure") {
                    if (!anytlsBoolean(queryValue, &candidate.allowInsecure)) return anytlsError(error, QObject::tr("AnyTLS insecure 必须是 0、1、false 或 true"));
                } else if (key == "minIdleSession") {
                    if (!QRegularExpression("^[0-9]+$").match(queryValue).hasMatch()) return anytlsError(error, QObject::tr("AnyTLS min_idle_session 必须是非负整数"));
                    bool valid = false;
                    candidate.minIdleSession = queryValue.toInt(&valid);
                    if (!valid) return anytlsError(error, QObject::tr("AnyTLS min_idle_session 超出整数范围"));
                } else {
                    return anytlsError(error, QObject::tr("AnyTLS 链接包含未支持的参数：%1").arg(queryName));
                }
            }
        }
        const auto invalid = candidate.Validate();
        if (!invalid.isEmpty()) return anytlsError(error, invalid);
        auto state = candidate.ToJson();
        for (const auto &item : candidate._map) {
            if (item->type == itemType::string && !state.contains(item->name)) state[item->name] = "";
        }
        FromJson(state);
        return true;
    }

    QString AnyTLSBean::ToShareLink() {
        if (!caText.isEmpty() || !alpn.isEmpty() || !utlsFingerprint.isEmpty() ||
            !idleSessionCheckInterval.isEmpty() || !idleSessionTimeout.isEmpty() || minIdleSession != 0 ||
            !clientMetadata.isEmpty() || !custom_config.isEmpty() || !custom_outbound.isEmpty()) {
            return ToNekorayShareLink("anytls");
        }
        QUrl url;
        url.setScheme("anytls");
        url.setUserName(password, QUrl::DecodedMode);
        url.setHost(serverAddress);
        url.setPort(serverPort);
        if (!name.isEmpty()) url.setFragment(name, QUrl::DecodedMode);
        QUrlQuery query;
        if (!sni.isEmpty()) query.addQueryItem("sni", sni);
        if (allowInsecure) query.addQueryItem("insecure", "1");
        if (!query.isEmpty()) url.setQuery(query);
        return url.toString(QUrl::FullyEncoded);
    }
}
