#pragma once

#include "db/ProxyEntity.hpp"

namespace NekoGui {
    // Preferences are 0 Auto / 1 Xray / 2 sing-box, while targetCore uses
    // CoreType::V2RAY / CoreType::SING_BOX. Do not compare them directly.
    struct CoreResolution {
        bool xraySupported = false;
        bool singBoxSupported = false;
        QString xrayReason;
        QString singBoxReason;
        QString reason;
        int targetCore = -1;
        bool preferenceFallback = false;

        [[nodiscard]] bool Supports(int core) const {
            return core == CoreType::V2RAY ? xraySupported : core == CoreType::SING_BOX && singBoxSupported;
        }
    };

    QString CoreDisplayName(int core);

    // These checks describe this GUI adapter and the pinned kernel families.
    // The selected running binary must still provide capabilities and Check.
    CoreResolution ResolveCore(const std::shared_ptr<ProxyEntity> &ent, int preferredDefault);
    CoreResolution ResolveCore(const std::shared_ptr<ProxyEntity> &ent, int preferredDefault,
                               int sourceCore, const DataStore *settings);
    QString CoreCompatibilityError(const std::shared_ptr<ProxyEntity> &ent, int targetCore,
                                   int sourceCore, const DataStore *settings);
    QString CoreSettingsCompatibilityError(int targetCore, int sourceCore, const DataStore *settings,
                                          bool forTest = false);

    // The RPC feature list comes from the executable actually being used.
    // This is a local build check; remote connection failures must not trigger
    // automatic fallback to a different core.
    QString CoreBuildFeaturesError(const QJsonObject &config, int targetCore,
                                  const QStringList &advertisedFeatures);
    void SetCoreRuntimeFeatures(int core, const QStringList &features);
    void ClearCoreRuntimeFeatures(int core);
} // namespace NekoGui
