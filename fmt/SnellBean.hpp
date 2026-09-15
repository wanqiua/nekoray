#pragma once

#include "fmt/AbstractBean.hpp"

namespace NekoGui_fmt {
    class SnellBean : public AbstractBean {
    public:
        // AbstractBean::version is the storage schema; this is the protocol.
        int snellVersion = 4;
        QString psk;
        QString userkey;
        bool reuse = false;
        QString network; // Empty means TCP and UDP.
        QString obfsMode;
        QString obfsHost;
        QString mode;

        SnellBean();

        QString DisplayType() override { return "Snell"; }
        QString DisplayCoreType() override { return "sing-box"; }
        [[nodiscard]] QString Validate() const;
        CoreObjOutboundBuildResult BuildCoreObjV2Ray(const NekoGui::DataStore *runtimeSettings = nullptr) override;
        CoreObjOutboundBuildResult BuildCoreObjSingBox(const NekoGui::DataStore *runtimeSettings = nullptr) override;

        // Local compatibility syntax, not an official Snell URI standard:
        // snell://<percent-encoded-PSK>@host:port?version=4|6&...#name
        // Query: userkey, reuse, network, obfs_mode/obfs_host (v4), mode (v6).
        // Hyphen/camel-case spellings of the obfs fields are accepted aliases.
        // Both this syntax and nekoray://snell#<base64url JSON> are accepted.
        bool TryParseLink(const QString &link, QString *error = nullptr);
        QString ToShareLink() override;
    };
}
