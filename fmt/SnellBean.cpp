#include "fmt/SnellBean.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <limits>

namespace {
    bool snellError(QString *error, const QString &message) {
        if (error != nullptr) *error = message;
        return false;
    }

    bool snellBoolean(const QString &text, bool *value) {
        if (text == "1" || text == "true") { *value = true; return true; }
        if (text == "0" || text == "false") { *value = false; return true; }
        return false;
    }

    bool snellDecodeComponent(const QString &encoded, QString *decoded) {
        const auto bytes = QByteArray::fromPercentEncoding(encoded.toLatin1());
        *decoded = QString::fromUtf8(bytes);
        return decoded->toUtf8() == bytes;
    }

    bool snellHostValid(const QString &host) {
        if (host.isEmpty() || host.contains(QRegularExpression("[\\s/\\\\@?#\\[\\]]")) || host.contains(QChar(0))) return false;
        QHostAddress address;
        if (address.setAddress(host)) return true;
        if (host.contains(':')) return false;
        QUrl url;
        url.setScheme("https");
        url.setHost(host);
        return url.isValid() && !url.host().isEmpty();
    }

    bool snellShareObject(const QUrl &url, NekoGui_fmt::SnellBean *bean, QString *error) {
        if (url.host() != "snell" || !url.userInfo().isEmpty() || url.port() != -1 ||
            !url.path().isEmpty() || url.hasQuery()) {
            return snellError(error, QObject::tr("Snell 完整分享链接格式无效"));
        }
        const auto encoded = url.fragment(QUrl::FullyDecoded).toLatin1();
        if (!QRegularExpression("^[A-Za-z0-9_-]+={0,2}$").match(QString::fromLatin1(encoded)).hasMatch()) {
            return snellError(error, QObject::tr("Snell 分享内容不是有效的 Base64URL"));
        }
        const auto bytes = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding);
        auto canonical = bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        auto unpadded = encoded;
        while (unpadded.endsWith('=')) unpadded.chop(1);
        if (canonical != unpadded) return snellError(error, QObject::tr("Snell 分享内容的 Base64URL 编码无效"));
        QJsonParseError jsonError;
        const auto document = QJsonDocument::fromJson(bytes, &jsonError);
        if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
            return snellError(error, QObject::tr("Snell 分享内容不是有效的 JSON 对象"));
        }
        const auto object = document.object();
        for (const auto &required : {"addr", "port", "psk", "version"}) {
            if (!object.contains(required)) return snellError(error, QObject::tr("Snell 分享缺少字段：%1").arg(required));
        }
        for (auto item = object.begin(); item != object.end(); ++item) {
            const auto field = bean->_map.value(item.key());
            if (!field) return snellError(error, QObject::tr("Snell 分享包含未支持的字段：%1").arg(item.key()));
            const auto value = item.value();
            const bool valid = (field->type == itemType::string && value.isString()) ||
                               (field->type == itemType::boolean && value.isBool()) ||
                               (field->type == itemType::integer && value.isDouble() &&
                                std::floor(value.toDouble()) == value.toDouble() &&
                                value.toDouble() >= std::numeric_limits<int>::min() &&
                                value.toDouble() <= std::numeric_limits<int>::max());
            if (!valid) return snellError(error, QObject::tr("Snell 分享字段类型无效：%1").arg(item.key()));
        }
        if (object.contains("_v") && object.value("_v").toInt() != 0) {
            return snellError(error, QObject::tr("不支持此 Snell 节点数据版本"));
        }
        bean->FromJson(object);
        return true;
    }
}

namespace NekoGui_fmt {
    SnellBean::SnellBean() : AbstractBean(0) {
        _add(new configItem("version", &snellVersion, itemType::integer));
        _add(new configItem("psk", &psk, itemType::string));
        _add(new configItem("userkey", &userkey, itemType::string));
        _add(new configItem("reuse", &reuse, itemType::boolean));
        _add(new configItem("network", &network, itemType::string));
        _add(new configItem("obfsMode", &obfsMode, itemType::string));
        _add(new configItem("obfsHost", &obfsHost, itemType::string));
        _add(new configItem("mode", &mode, itemType::string));
    }

    QString SnellBean::Validate() const {
        if (!snellHostValid(serverAddress)) return QObject::tr("Snell 服务器地址必须是有效的域名或 IP，不含端口、路径或空白");
        if (serverPort < 1 || serverPort > 65535) return QObject::tr("Snell 端口必须在 1 到 65535 之间");
        if (snellVersion != 4 && snellVersion != 6) return QObject::tr("Snell 客户端只支持版本 4 或 6");
        if (psk.isEmpty()) return QObject::tr("Snell 预共享密钥不能为空");
        if (snellVersion == 6 && (psk.toUtf8().size() < 12 || psk.toUtf8().size() > 255)) {
            return QObject::tr("Snell 6 预共享密钥必须为 12 到 255 个 UTF-8 字节");
        }
        if (userkey.toUtf8().size() > 255) return QObject::tr("Snell 用户密钥不能超过 255 个 UTF-8 字节");
        if (!network.isEmpty() && network != "tcp" && network != "udp") {
            return QObject::tr("Snell 网络类型只支持 TCP、UDP 或留空启用两者");
        }
        if (snellVersion == 4) {
            if (!obfsMode.isEmpty() && obfsMode != "none" && obfsMode != "http" && obfsMode != "tls") {
                return QObject::tr("Snell 4 混淆模式只支持 none、http 或 tls");
            }
            if (!mode.isEmpty()) return QObject::tr("Snell 4 不支持版本 6 的流量整形模式");
            if (!obfsHost.isEmpty()) {
                if (obfsMode != "http" && obfsMode != "tls") return QObject::tr("Snell 混淆域名仅适用于 HTTP 或 TLS 混淆");
                if (!snellHostValid(obfsHost)) return QObject::tr("Snell 混淆域名格式无效");
            }
        } else {
            if (!obfsMode.isEmpty() || !obfsHost.isEmpty()) return QObject::tr("Snell 6 不支持版本 4 的混淆参数，请清空后使用流量整形模式");
            if (!mode.isEmpty() && mode != "default" && mode != "unshaped" && mode != "unsafe-raw") {
                return QObject::tr("Snell 6 流量整形模式只支持 default、unshaped 或 unsafe-raw");
            }
        }
        return {};
    }

    CoreObjOutboundBuildResult SnellBean::BuildCoreObjV2Ray(const NekoGui::DataStore *) {
        return {{}, QObject::tr("Xray 不支持 Snell，请使用 sing-box")};
    }

    CoreObjOutboundBuildResult SnellBean::BuildCoreObjSingBox(const NekoGui::DataStore *) {
        CoreObjOutboundBuildResult result;
        result.error = Validate();
        if (!result.error.isEmpty()) return result;
        result.outbound = QJsonObject{
            {"type", "snell"}, {"server", serverAddress}, {"server_port", serverPort},
            {"version", snellVersion}, {"psk", psk}, {"reuse", reuse},
        };
        if (!userkey.isEmpty()) result.outbound["userkey"] = userkey;
        if (!network.isEmpty()) result.outbound["network"] = network;
        if (snellVersion == 4) {
            if (!obfsMode.isEmpty()) result.outbound["obfs_mode"] = obfsMode;
            if (!obfsHost.isEmpty()) result.outbound["obfs_host"] = obfsHost;
        } else if (!mode.isEmpty()) {
            result.outbound["mode"] = mode;
        }
        return result;
    }

    bool SnellBean::TryParseLink(const QString &link, QString *error) {
        if (error != nullptr) error->clear();
        const QUrl url(link.trimmed(), QUrl::StrictMode);
        if (!url.isValid()) return snellError(error, QObject::tr("Snell 链接格式无效"));
        SnellBean candidate;
        if (url.scheme() == "nekoray") {
            if (!snellShareObject(url, &candidate, error)) return false;
        } else {
            if (url.scheme() != "snell" || (!url.path().isEmpty() && url.path() != "/")) {
                return snellError(error, QObject::tr("Snell 链接必须使用 snell://，且不能包含路径"));
            }
            candidate.serverAddress = url.host(QUrl::FullyDecoded);
            candidate.serverPort = url.port(-1);
            // Decode the entire user-info once so literal and escaped colons
            // remain part of the PSK. Qt rejects FullyDecoded for userInfo().
            if (!snellDecodeComponent(url.userInfo(QUrl::FullyEncoded), &candidate.psk)) return snellError(error, QObject::tr("Snell 预共享密钥包含无效的 UTF-8 编码"));
            if (!snellDecodeComponent(url.fragment(QUrl::FullyEncoded), &candidate.name)) return snellError(error, QObject::tr("Snell 节点名称包含无效的 UTF-8 编码"));
            QSet<QString> seen;
            for (const auto &item : QUrlQuery(url).queryItems(QUrl::FullyEncoded)) {
                QString queryName, queryValue;
                if (!snellDecodeComponent(item.first, &queryName) || !snellDecodeComponent(item.second, &queryValue)) {
                    return snellError(error, QObject::tr("Snell 链接参数包含无效的 UTF-8 编码"));
                }
                auto key = queryName;
                if (key == "obfs-mode" || key == "obfsMode") key = "obfs_mode";
                if (key == "obfs-host" || key == "obfsHost") key = "obfs_host";
                if (seen.contains(key)) return snellError(error, QObject::tr("Snell 链接包含重复参数：%1").arg(key));
                seen.insert(key);
                if (key == "version") {
                    if (queryValue != "4" && queryValue != "6") return snellError(error, QObject::tr("Snell 链接版本必须是 4 或 6"));
                    candidate.snellVersion = queryValue.toInt();
                } else if (key == "userkey") {
                    candidate.userkey = queryValue;
                } else if (key == "reuse") {
                    if (!snellBoolean(queryValue, &candidate.reuse)) return snellError(error, QObject::tr("Snell reuse 必须是 0、1、false 或 true"));
                } else if (key == "network") {
                    candidate.network = queryValue;
                } else if (key == "obfs_mode") {
                    candidate.obfsMode = queryValue;
                } else if (key == "obfs_host") {
                    candidate.obfsHost = queryValue;
                } else if (key == "mode") {
                    candidate.mode = queryValue;
                } else {
                    return snellError(error, QObject::tr("Snell 链接包含未支持的参数：%1").arg(queryName));
                }
            }
            if (!seen.contains("version")) return snellError(error, QObject::tr("Snell 链接必须显式指定 version=4 或 version=6"));
        }
        const auto invalid = candidate.Validate();
        if (!invalid.isEmpty()) return snellError(error, invalid);
        auto state = candidate.ToJson();
        // JsonStore omits empty strings. Include them when replacing an
        // existing bean so an old SNI/obfs/custom field cannot survive import.
        for (const auto &item : candidate._map) {
            if (item->type == itemType::string && !state.contains(item->name)) state[item->name] = "";
        }
        FromJson(state);
        return true;
    }

    QString SnellBean::ToShareLink() {
        // No uniform Snell sharing standard is assumed. Keep all protocol
        // settings and custom JSON; core preferences live outside the bean.
        return ToNekorayShareLink("snell");
    }
}
