#pragma once

#include "fmt/AbstractBean.hpp"

namespace NekoGui_fmt {
    class AnyTLSBean : public AbstractBean {
    public:
        QString password;
        bool allowInsecure = false;
        QString sni;
        QString caText;
        QString alpn;
        QString utlsFingerprint;
        // Go duration strings. Empty leaves the sing-box 30s defaults.
        QString idleSessionCheckInterval;
        QString idleSessionTimeout;
        int minIdleSession = 0;
        QString clientMetadata;

        AnyTLSBean();

        QString DisplayType() override { return "AnyTLS"; }
        QString DisplayCoreType() override { return "sing-box"; }
        [[nodiscard]] QString Validate() const;
        CoreObjOutboundBuildResult BuildCoreObjV2Ray(const NekoGui::DataStore *runtimeSettings = nullptr) override;
        CoreObjOutboundBuildResult BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings = nullptr) override;

        // anytls://<percent-encoded-password>@host:port?sni=...&insecure=0#name
        // The entire user-info is the password, including literal colons.
        // Extended query keys use the member names or sing-box snake_case.
        // Non-basic fields are exported using the complete Nekoray format.
        bool TryParseLink(const QString &link, QString *error = nullptr);
        QString ToShareLink() override;
    };
}
