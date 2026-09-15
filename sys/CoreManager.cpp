#include "CoreManager.hpp"
#include "db/CoreCapabilities.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSemaphore>
#include <QThread>

namespace NekoGui_sys {

QString CoreManager::Name(int core) {
    return core == NekoGui::CoreType::SING_BOX ? QStringLiteral("sing-box") : QStringLiteral("Xray");
}

CoreManager::CoreManager(QObject *parent) : QObject(parent) {
    for (int core = 0; core < 2; ++core) {
        auto instance = std::make_shared<Instance>();
        instance->core = core;
        instance->port = MkPort();
        if (instance->port <= 0 || (core == 1 && instance->port == instances[0]->port)) instance->port = MkPort();
        instance->token = GetRandomString(48);
        instance->program = QApplication::applicationDirPath() +
                            (core == NekoGui::CoreType::SING_BOX ? "/nekobox_core" : "/nekoray_core");
#ifdef Q_OS_WIN
        instance->program += ".exe";
#endif
#ifndef NKR_NO_GRPC
        instance->client = std::make_shared<NekoGui_rpc::Client>(
            [core](const QString &error) { MW_show_log("[" + Name(core) + "] " + error); },
            "127.0.0.1:" + QString::number(instance->port), instance->token);
#endif
        instances[core] = instance;
    }
}

std::shared_ptr<CoreManager::Instance> CoreManager::Get(int core) const {
    return core >= 0 && core < int(instances.size()) ? instances[core] : nullptr;
}

QString CoreManager::Identity(int core) const {
    auto instance = Get(core);
    if (!instance) return {};
    QMutexLocker lock(&instance->metadataMutex);
    return instance->identity;
}

quint64 CoreManager::Generation(int core) const {
    auto instance = Get(core);
    return instance ? instance->generation.load() : 0;
}

QStringList CoreManager::Features(int core) const {
    QStringList features;
#ifndef NKR_NO_GRPC
    auto instance = Get(core);
    if (!instance) return features;
    QMutexLocker lock(&instance->metadataMutex);
    for (const auto &feature : instance->capabilities.features()) features.append(QString::fromStdString(feature));
#endif
    return features;
}

bool CoreManager::Ready(int core) const {
    auto instance = Get(core);
    return instance && instance->alive.load() && instance->ready.load();
}

void CoreManager::StartProcess(const std::shared_ptr<Instance> &instance) {
    if (shuttingDown || instance->alive) return;
    if (instance->process) {
        instance->process->deleteLater();
        instance->process = nullptr;
    }
    instance->expectedExit = false;
    instance->ready = false;
    NekoGui::ClearCoreRuntimeFeatures(instance->core);
    ++instance->generation;
    {
        QMutexLocker lock(&instance->metadataMutex);
        instance->error.clear();
    }
    auto process = new QProcess;
    instance->process = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto assets = NekoGui::FindCoreAsset("geoip.dat");
    if (!assets.isEmpty()) environment.insert("XRAY_LOCATION_ASSET", QFileInfo(assets).absolutePath());
    if (NekoGui::dataStore->core_ray_direct_dns) environment.insert("NKR_CORE_RAY_DIRECT_DNS", "1");
    if (NekoGui::dataStore->core_ray_windows_disable_auto_interface)
        environment.insert("NKR_CORE_RAY_WINDOWS_DISABLE_AUTO_INTERFACE", "1");
    process->setProcessEnvironment(environment);
    const auto guard = QPointer<CoreManager>(this);
    const auto log = [instance, process](bool stderrChannel) {
        auto bytes = stderrChannel ? process->readAllStandardError() : process->readAllStandardOutput();
        // Authentication is supplied over stdin, never included in a command
        // line or diagnostic environment dump.
        bytes.replace("Please set a token: ", "");
        bytes.replace("token is set", "");
        if (!bytes.trimmed().isEmpty()) MW_show_log("[" + Name(instance->core) + "] " + QString::fromUtf8(bytes).trimmed());
    };
    connect(process, &QProcess::readyReadStandardOutput, process, [log] { log(false); });
    connect(process, &QProcess::readyReadStandardError, process, [log] { log(true); });
    connect(process, &QProcess::errorOccurred, process, [instance, process](QProcess::ProcessError) {
        QMutexLocker lock(&instance->metadataMutex);
        instance->error = process->errorString();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
            [guard, instance](int, QProcess::ExitStatus) {
        const bool expected = instance->expectedExit.load();
        instance->alive = false;
        instance->ready = false;
        const auto exitedGeneration = instance->generation.fetch_add(1);
        if (!guard) return;
        runOnUiThread([guard, instance, expected, exitedGeneration] {
            if (!guard) return;
            NekoGui::dataStore->core_running = guard->Ready(0) || guard->Ready(1);
            if (!guard->shuttingDown && !expected && guard->onExited) guard->onExited(instance->core, exitedGeneration);
        }, guard);
    });
    QStringList arguments{instance->core == NekoGui::CoreType::SING_BOX ? "nekobox" : "nekoray",
                          "-port", QString::number(instance->port),
                          "-parent-pid", QString::number(QApplication::applicationPid())};
    if (NekoGui::dataStore->flag_debug) arguments << "-debug";
    process->start(instance->program, arguments);
    if (!process->waitForStarted(3000)) return;
    instance->alive = true;
    process->write((instance->token + "\n").toUtf8());
}

bool CoreManager::EnsureReady(int core, QString &error) {
#ifndef NKR_NO_GRPC
    auto instance = Get(core);
    if (!instance || shuttingDown) {
        error = tr("Core is not available.");
        return false;
    }
    QMutexLocker preparing(&instance->prepareMutex);
    if (instance->ready && instance->alive) return true;
    if (!QFileInfo::exists(instance->program) || instance->port <= 0) {
        error = tr("%1 core is missing or no control port is available.").arg(Name(core));
        return false;
    }
    QSemaphore started;
    runOnUiThread([this, instance, &started] { StartProcess(instance); started.release(); }, DS_cores);
    started.acquire();
    if (!instance->alive) {
        QMutexLocker lock(&instance->metadataMutex);
        error = tr("Cannot start %1: %2").arg(Name(core), instance->error);
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    const auto preparingGeneration = instance->generation.load();
    while (timer.elapsed() < 8000 && instance->alive && !shuttingDown) {
        bool ok = false;
        auto capabilities = instance->client->Capabilities(&ok);
        if (ok) {
            const std::string expected = core == NekoGui::CoreType::SING_BOX ? "sing-box" : "xray";
            if (capabilities.core_family() != expected || capabilities.build_id().empty()) {
                error = tr("%1 did not report a compatible build identity.").arg(Name(core));
                return false;
            }
            QFile binary(instance->program);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!binary.open(QIODevice::ReadOnly) || !hash.addData(&binary)) {
                error = tr("Cannot verify the %1 binary.").arg(Name(core));
                return false;
            }
            QFile guiBinary(QApplication::applicationFilePath());
            QCryptographicHash guiHash(QCryptographicHash::Sha256);
            if (!guiBinary.open(QIODevice::ReadOnly) || !guiHash.addData(&guiBinary)) {
                error = tr("Cannot verify this client build.");
                return false;
            }
            if (!instance->alive || instance->generation.load() != preparingGeneration || shuttingDown) {
                error = tr("%1 exited during initialization.").arg(Name(core));
                return false;
            }
            {
                QMutexLocker lock(&instance->metadataMutex);
                instance->capabilities = capabilities;
                instance->identity = QString::fromStdString(capabilities.build_id()) + ":" + QString::fromLatin1(hash.result().toHex()) +
                                     ":gui:" + QString::fromLatin1(guiHash.result().toHex());
            }
            NekoGui::SetCoreRuntimeFeatures(core, Features(core));
            instance->ready = true;
            const auto guard = QPointer<CoreManager>(this);
            runOnUiThread([guard] { if (guard) NekoGui::dataStore->core_running = guard->Ready(0) || guard->Ready(1); }, this);
            return true;
        }
        QThread::msleep(100);
    }
    error = tr("%1 control interface is unavailable. Use the matching core supplied with this build.").arg(Name(core));
    return false;
#else
    Q_UNUSED(core);
    error = tr("This build has no core RPC support.");
    return false;
#endif
}

bool CoreManager::StopFaulted(int core, QString &error) {
    auto instance = Get(core);
    if (!instance) return true;
    QMutexLocker preparing(&instance->prepareMutex);
    QSemaphore stopped;
    bool closed = false;
    runOnUiThread([instance, &stopped, &closed] {
        instance->expectedExit = true;
        auto process = instance->process;
        if (process && process->state() != QProcess::NotRunning) {
            process->kill(); // Retained QProcess ownership, never a process-name lookup.
            process->waitForFinished(3000);
        }
        closed = !process || process->state() == QProcess::NotRunning;
        if (closed) {
            instance->alive = false;
            instance->ready = false;
            NekoGui::ClearCoreRuntimeFeatures(instance->core);
        }
        stopped.release();
    }, DS_cores);
    stopped.acquire();
    if (!closed) error = tr("Cannot confirm that the owned %1 process stopped. Retry disconnecting.").arg(Name(core));
    return closed;
}

void CoreManager::Shutdown() {
    shuttingDown = true;
    for (const auto &instance : instances) {
        instance->expectedExit = true;
#ifndef NKR_NO_GRPC
        if (instance->alive) instance->client->Exit();
#endif
    }
    QSemaphore stopped;
    runOnUiThread([this, &stopped] {
        for (const auto &instance : instances) {
            if (!instance->process) continue;
            if (instance->process->state() != QProcess::NotRunning && !instance->process->waitForFinished(1000)) {
                instance->process->kill(); // Only the QProcess owned by this app.
                instance->process->waitForFinished(1000);
            }
            instance->process->deleteLater();
            instance->process = nullptr;
            instance->alive = false;
            instance->ready = false;
        }
        stopped.release();
    }, DS_cores);
    stopped.acquire();
}

} // namespace NekoGui_sys
