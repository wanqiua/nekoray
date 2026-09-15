#pragma once

#include "main/NekoGui.hpp"
#include "rpc/gRPC.h"

#include <QObject>
#include <QProcess>
#include <QMutex>
#include <array>
#include <atomic>
#include <memory>

namespace NekoGui_sys {

// Each daemon owns a control endpoint and can run tests independently of its
// currently loaded proxy. A proxy switch never exits either control daemon.
class CoreManager : public QObject {
    Q_OBJECT
public:
    struct Instance {
        int core = -1;
        int port = 0;
        QString token;
        QString program;
        std::atomic<quint64> generation{0};
        std::atomic_bool alive{false};
        std::atomic_bool ready{false};
        QMutex prepareMutex;
        QMutex metadataMutex;
        QString identity;
        QString error;
        QProcess *process = nullptr; // Access only on DS_cores.
        std::atomic_bool expectedExit{false};
#ifndef NKR_NO_GRPC
        std::shared_ptr<NekoGui_rpc::Client> client;
        libcore::CapabilitiesResp capabilities;
#endif
    };

    explicit CoreManager(QObject *parent = nullptr);
    std::shared_ptr<Instance> Get(int core) const;
    // Blocking operations belong on a worker, never the GUI thread.
    bool EnsureReady(int core, QString &error);
    // Reconcile a failed or unacknowledged Stop. Never used for normal switches.
    bool StopFaulted(int core, QString &error);
    void Shutdown();
    QString Identity(int core) const;
    QStringList Features(int core) const;
    quint64 Generation(int core) const;
    bool Ready(int core) const;
    static QString Name(int core);
    std::function<void(int, quint64)> onExited;

private:
    std::array<std::shared_ptr<Instance>, 2> instances;
    std::atomic_bool shuttingDown{false};
    void StartProcess(const std::shared_ptr<Instance> &instance);
};

} // namespace NekoGui_sys
