#pragma once

#include "ProxyEntity.hpp"
#include "sys/ExternalProcess.hpp"

namespace NekoGui {
    // Capture on the GUI thread, then pass the same immutable context through
    // preflight, start, rollback and background probes. Never change coreType
    // temporarily to build a configuration for a different core.
    struct BuildConfigContext {
        int targetCore = CoreType::V2RAY;
        int sourceCore = CoreType::V2RAY;
        std::shared_ptr<DataStore> settings;
        QByteArray hookSource;
        QString geoipPath;
        QString geositePath;
        bool externalTun = false;
        int tunProxyPort = 0;
        QStringList tunBypassProcessPaths;

        [[nodiscard]] QByteArray Fingerprint(bool forTest = false) const;
    };

    BuildConfigContext CaptureBuildConfigContext(int targetCore, bool externalTun = false);

    class BuildConfigResult {
    public:
        QString error;
        QJsonObject coreConfig;
        int targetCore = -1;
        bool needKeepVpnOff = false;
        QMap<int, QString> generatedLocalPorts;

        [[nodiscard]] QByteArray Fingerprint() const;

        QList<std::shared_ptr<NekoGui_traffic::TrafficData>> outboundStats; // all, but not including "bypass" "block"
        std::shared_ptr<NekoGui_traffic::TrafficData> outboundStat;         // main
        QStringList ignoreConnTag;

        std::list<std::shared_ptr<NekoGui_fmt::ExternalBuildResult>> extRs;
    };

    class BuildConfigStatus {
    public:
        std::shared_ptr<BuildConfigResult> result;
        std::shared_ptr<ProxyEntity> ent;
        bool forTest;
        bool forExport;
        BuildConfigContext context;

        [[nodiscard]] bool IsSingBox() const { return context.targetCore == CoreType::SING_BOX; }
        [[nodiscard]] bool InternalTun() const {
            return IsSingBox() && context.settings->vpn_internal_tun && !context.externalTun;
        }

        // priv
        QList<int> globalProfiles;

        // xxList is V2Ray format string list

        QStringList domainListDNSRemote;
        QStringList domainListDNSDirect;
        QStringList domainListRemote;
        QStringList domainListDirect;
        QStringList ipListRemote;
        QStringList ipListDirect;
        QStringList domainListBlock;
        QStringList ipListBlock;

        // config format

        QJsonArray routingRules;
        QJsonArray inbounds;
        QJsonArray outbounds;
    };

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport);
    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport,
                                                 int targetCore);
    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport,
                                                 const BuildConfigContext &context);

    void BuildConfigV2Ray(const std::shared_ptr<BuildConfigStatus> &status);

    void BuildConfigSingBox(const std::shared_ptr<BuildConfigStatus> &status);

    QString BuildChain(int chainId, const std::shared_ptr<BuildConfigStatus> &status);

    QString BuildChainInternal(int chainId, const QList<std::shared_ptr<ProxyEntity>> &ents,
                               const std::shared_ptr<BuildConfigStatus> &status);

    QString WriteVPNSingBoxConfig();
    QString WriteVPNSingBoxConfig(const BuildConfigContext &context);
    std::shared_ptr<BuildConfigResult> BuildVPNSingBoxConfig(const BuildConfigContext &context);

    QString WriteVPNLinuxScript(const QString &protectPath, const QString &configPath);
} // namespace NekoGui
