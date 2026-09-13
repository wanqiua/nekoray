#pragma once

#include "db/ConfigBuilder.hpp"
#include "db/Database.hpp"
#include "db/CoreCapabilities.hpp"
#include "db/ProfileTestResult.hpp"
#include "fmt/ChainBean.hpp"
#include "sys/CoreManager.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>
#include <QSaveFile>
#include <QDir>

inline bool MaterializeCoreHelperFiles(const std::list<std::shared_ptr<NekoGui_fmt::ExternalBuildResult>> &results, QString &error) {
    for (const auto &result : results) {
        for (auto file = result->generatedFiles.cbegin(); file != result->generatedFiles.cend(); ++file) {
            QSaveFile output(file.key());
            if (!QDir().mkpath(QFileInfo(file.key()).absolutePath()) || !output.open(QIODevice::WriteOnly) ||
                output.write(file.value()) != file.value().size() || !output.commit()) {
                error = QObject::tr("Cannot prepare an external protocol configuration.");
                return false;
            }
            QFile::setPermissions(file.key(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }
    return true;
}

inline void RemoveCoreHelperFiles(const std::list<std::shared_ptr<NekoGui_fmt::ExternalBuildResult>> &results) {
    for (const auto &result : results)
        for (auto file = result->generatedFiles.cbegin(); file != result->generatedFiles.cend(); ++file) QFile::remove(file.key());
}

namespace NekoGui {

inline QString CoreConfigDigest(const QJsonObject &config) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(config).toJson(QJsonDocument::Compact),
                                                       QCryptographicHash::Sha256).toHex());
}

inline QJsonObject CoreProfileInputs(const std::shared_ptr<ProxyEntity> &profile, QSet<int> &visited) {
    if (!profile || visited.contains(profile->id)) return {};
    visited.insert(profile->id);
    QJsonObject inputs{{"type", profile->type}, {"bean", profile->bean->ToJson({"name"})}};
    if (profile->type == "chain") {
        QJsonArray chain;
        for (int id : profile->ChainBean()->list) chain.append(CoreProfileInputs(profileManager->GetProfile(id), visited));
        inputs.insert("chain", chain);
    }
    auto group = profileManager->GetGroup(profile->gid);
    if (group && group->front_proxy_id >= 0)
        inputs.insert("front", CoreProfileInputs(profileManager->GetProfile(group->front_proxy_id), visited));
    return inputs;
}

inline QString CoreTestInputDigest(const std::shared_ptr<ProxyEntity> &profile, const BuildConfigContext &context,
                                   int mode, const QString &url, int timeout, const QString &downloadURL, int downloadTimeout,
                                   const QJsonObject &parameters = {}) {
    QSet<int> visited;
    return CoreConfigDigest({{"profile", CoreProfileInputs(profile, visited)},
                             {"context", QString::fromLatin1(context.Fingerprint(true))},
                             {"mode", mode}, {"url", url}, {"timeout", timeout},
                             {"download_url", downloadURL}, {"download_timeout", downloadTimeout}, {"parameters", parameters}});
}

struct RunningCoreSession {
    std::shared_ptr<ProxyEntity> profile;
    BuildConfigContext context;
    std::shared_ptr<BuildConfigResult> built;
    std::shared_ptr<NekoGui_sys::CoreManager::Instance> instance;
    QString fingerprint;
    QString displayName;
    QString profileInputDigest;
    QString selectionNotice;
    quint64 generation = 0;
    std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> external;
    std::shared_ptr<std::atomic_bool> externalHealthy = std::make_shared<std::atomic_bool>(true);
    std::function<void()> externalExited;
#ifndef NKR_NO_GRPC
    libcore::LoadConfigReq request;
#endif
};

struct CoreSwitchIntent {
    quint64 sequence = 0;
    quint64 preferenceRevision = 0;
    std::shared_ptr<ProxyEntity> profile; // null means stop
    int rollbackPreference = -1;
    bool crash = false;
    bool signalStopped = false;
    bool prepareForTun = false;
    std::shared_ptr<RunningCoreSession> recovery;
};

} // namespace NekoGui
