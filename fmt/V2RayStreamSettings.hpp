#pragma once

#include "AbstractBean.hpp"

#include <QRegularExpression>

namespace NekoGui_fmt {
    class V2rayStreamSettings : public JsonStore {
    public:
        QString network = "tcp";
        QString security = "";
        QString packet_encoding = "";
        // ws/http/grpc/tcp-http/httpupgrade
        QString path = "";
        QString host = "";
        // kcp/quic/tcp-http
        QString header_type = "";
        // tls
        QString sni = "";
        QString alpn = "";
        QString certificate = "";
        QString utlsFingerprint = "";
        bool allow_insecure = false;
        // SHA-256 of the server certificate DER (share link "pcs"). A pin
        // replaces CA validation; not used with REALITY.
        QString certificate_pin = "";
        // ws early data
        QString ws_early_data_name = "";
        int ws_early_data_length = 0;
        // reality
        QString reality_pbk = "";
        QString reality_sid = "";
        QString reality_spx = "";
        // multiplex
        int multiplex_status = 0;

        V2rayStreamSettings() : JsonStore() {
            _add(new configItem("net", &network, itemType::string));
            _add(new configItem("sec", &security, itemType::string));
            _add(new configItem("pac_enc", &packet_encoding, itemType::string));
            _add(new configItem("path", &path, itemType::string));
            _add(new configItem("host", &host, itemType::string));
            _add(new configItem("sni", &sni, itemType::string));
            _add(new configItem("alpn", &alpn, itemType::string));
            _add(new configItem("cert", &certificate, itemType::string));
            _add(new configItem("insecure", &allow_insecure, itemType::boolean));
            _add(new configItem("h_type", &header_type, itemType::string));
            _add(new configItem("ed_name", &ws_early_data_name, itemType::string));
            _add(new configItem("ed_len", &ws_early_data_length, itemType::integer));
            _add(new configItem("utls", &utlsFingerprint, itemType::string));
            _add(new configItem("pbk", &reality_pbk, itemType::string));
            _add(new configItem("sid", &reality_sid, itemType::string));
            _add(new configItem("spx", &reality_spx, itemType::string));
            _add(new configItem("mux_s", &multiplex_status, itemType::integer));
            _add(new configItem("pcs", &certificate_pin, itemType::string));
        }

        // Accepts 64 hex digits or 32 colon-separated bytes; "," separates
        // several pins. Outputs lowercase hex without colons.
        static bool ParseCertificatePins(const QString &text, QStringList *pins = nullptr) {
            static const QRegularExpression pattern("^(?:[0-9a-fA-F]{64}|(?:[0-9a-fA-F]{2}:){31}[0-9a-fA-F]{2})$");
            QStringList result;
            if (!text.trimmed().isEmpty()) {
                for (const auto &entry: text.split(',')) {
                    const auto pin = entry.trimmed();
                    if (!pattern.match(pin).hasMatch()) return false;
                    result << QString(pin).remove(':').toLower();
                }
            }
            if (pins != nullptr) *pins = result;
            return true;
        }

        [[nodiscard]] QStringList CertificatePins() const {
            QStringList pins;
            return ParseCertificatePins(certificate_pin, &pins) ? pins : QStringList{};
        }

        [[nodiscard]] QString CertificatePinError(bool singBox) const {
            if (certificate_pin.trimmed().isEmpty()) return {};
            if (security != "tls") return QObject::tr("证书 SHA-256 需要启用 TLS");
            if (!reality_pbk.trimmed().isEmpty()) return QObject::tr("REALITY 不使用证书 SHA-256，请清空该字段");
            if (!ParseCertificatePins(certificate_pin))
                return QObject::tr("证书 SHA-256 必须是 64 位十六进制，多个值用逗号分隔");
            if (singBox) return QObject::tr("sing-box 暂不支持此协议的证书 SHA-256 校验，请使用 Xray");
            return {};
        }

        QJsonObject BuildStreamSettingsV2Ray(const NekoGui::DataStore *runtimeSettings = nullptr);

        void BuildStreamSettingsSingBox(QJsonObject *outbound, const NekoGui::DataStore *runtimeSettings = nullptr);
    };

    inline V2rayStreamSettings *GetStreamSettings(AbstractBean *bean) {
        if (bean == nullptr) return nullptr;
        auto stream_item = bean->_get("stream");
        if (stream_item != nullptr) {
            auto stream_store = (JsonStore *) stream_item->ptr;
            auto stream = (NekoGui_fmt::V2rayStreamSettings *) stream_store;
            return stream;
        }
        return nullptr;
    }
} // namespace NekoGui_fmt
