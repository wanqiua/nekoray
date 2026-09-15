#pragma once

#include <QObject>
#include <QString>
#include <memory>

namespace NekoGui::TunLifecycle {

// The daemon/request types keep the worker independent of MainWindow. The
// production daemon owns an exact process handle and RPC client; tests use the
// same transitions with controlled process exits and RPC responses.
template<typename Daemon, typename Request>
struct State {
    std::shared_ptr<Daemon> daemon;
    Request request;
    QString fingerprint;
    bool enabled = false;
    // A timed-out RPC must be reconciled with Stop before another Start or
    // process exit. This is distinct from a confirmed enabled checkbox.
    bool uncertain = false;
};

template<typename Session>
struct Outcome {
    Session state;
    QString notice;
    bool succeeded = false;
};

inline QString StartError(const QString &error) {
    // Core diagnostics may contain user-configured DNS URLs and credentials.
    // Only map known causes; never put raw configuration diagnostics in logs.
    if (error.contains("permission", Qt::CaseInsensitive) || error.contains("access denied", Qt::CaseInsensitive))
        return QObject::tr("TUN 启动失败：没有所需的系统权限，请重新开启并完成系统授权。");
    if (error.contains("wintun", Qt::CaseInsensitive))
        return QObject::tr("TUN 启动失败：无法加载 Wintun，请检查本版本配套的驱动文件。");
    if (error.contains("address already in use", Qt::CaseInsensitive) || error.contains("already exists", Qt::CaseInsensitive))
        return QObject::tr("TUN 启动失败：端口或虚拟网卡已被占用，请关闭冲突的程序后重试。");
    return QObject::tr("TUN 启动失败，已尝试清理本次启动；请检查 TUN 设置和系统授权。");
}

template<typename Session>
bool Stop(Session &state, bool exitDaemon, QString &error) {
    if (!state.daemon) {
        state = {};
        return true;
    }
    const auto daemon = state.daemon;
    if (!daemon->process->Running()) {
        state = {};
        return true;
    }
    bool acknowledged = false;
    QString stopError;
    for (int attempt = 0; attempt < 2; ++attempt) {
        stopError = daemon->client->Stop(&acknowledged);
        if (acknowledged || !daemon->process->Running()) break;
    }
    if (!daemon->process->Running()) {
        state = {};
        return true;
    }
    if (!acknowledged || !stopError.isEmpty()) {
        state.uncertain = true;
        error = QObject::tr("尚未确认 TUN 已停止，已保留该进程的控制权；请重试关闭 TUN。");
        return false;
    }
    state.enabled = false;
    state.uncertain = false;
    if (!exitDaemon) return true;
    // Exit is sent only after Stop confirms that privileged resources closed.
    daemon->client->Exit();
    if (!daemon->process->WaitForExit(2000) && !daemon->process->TerminateOwned(2000, error)) return false;
    state = {};
    return true;
}

// This path is only used for a newly launched daemon before any Start request
// has been sent. There can be no TUN routes to clean up in that process.
template<typename Session>
bool DiscardUnstarted(Session &state, QString &error) {
    if (!state.daemon) return true;
    const auto daemon = state.daemon;
    if (daemon->process->Running()) {
        bool stopped = false;
        const auto stopError = daemon->client->Stop(&stopped);
        if (stopped && stopError.isEmpty()) daemon->client->Exit();
        if (!daemon->process->WaitForExit(1000) && !daemon->process->TerminateOwned(2000, error)) return false;
    }
    state = {};
    return true;
}

template<typename Session>
bool RequireRunning(Session &state, QString &notice) {
    if (state.daemon && state.daemon->process->Running()) return true;
    state = {};
    notice = QObject::tr("TUN 辅助进程在应用设置期间退出，TUN 已关闭；请重新开启。");
    return false;
}

// Apply only after both preflights have passed. A process can still exit at
// any point, including during Stop; never start or restore a cleared session.
// Validation may set out.notice with the reason the GUI snapshot is obsolete.
template<typename Session, typename Request, typename Cancelled, typename Validate>
void Apply(Outcome<Session> &out, const Request &request, const QString &fingerprint,
           const Cancelled &cancelledNow, const Validate &stillValid) {
    const auto previous = out.state;
    if ((out.state.enabled || out.state.uncertain) && !Stop(out.state, false, out.notice)) return;
    if (cancelledNow()) {
        Stop(out.state, true, out.notice);
        return;
    }
    if (!RequireRunning(out.state, out.notice)) return;

    const auto restorePrevious = [&](const QString &successNotice) {
        if (!previous.enabled || cancelledNow()) return false;
        if (!RequireRunning(out.state, out.notice)) return false;
        bool restored = false;
        const auto restoreError = out.state.daemon->client->Start(&restored, previous.request);
        if (!RequireRunning(out.state, out.notice)) return false;
        if (restored && restoreError.isEmpty()) {
            out.state = previous;
            out.state.uncertain = false;
            out.notice = successNotice;
            return true;
        }
        out.state.uncertain = !restored;
        out.notice = QObject::tr("新的 TUN 设置启动失败，原连接也未能恢复；请检查设置后重试。");
        return false;
    };

    bool ok = false;
    const auto startError = out.state.daemon->client->Start(&ok, request);
    if (ok && startError.isEmpty()) {
        if (!RequireRunning(out.state, out.notice)) return;
        out.state.enabled = true;
        out.state.uncertain = false;
        out.state.request = request;
        out.state.fingerprint = fingerprint;
        if (cancelledNow()) {
            Stop(out.state, true, out.notice);
        } else if (!stillValid()) {
            QString cleanupError;
            if (!Stop(out.state, false, cleanupError)) {
                out.notice = cleanupError;
                return;
            }
            if (restorePrevious(QObject::tr("TUN 设置在启动期间发生变化，已恢复原来的 TUN 连接，请重新应用设置。"))) return;
            if (!Stop(out.state, true, cleanupError)) out.notice = cleanupError;
        } else {
            out.succeeded = true;
        }
        return;
    }

    out.state.uncertain = !ok;
    out.notice = ok ? StartError(startError) : QObject::tr("未收到 TUN 启动确认，正在清理本次启动。");
    QString cleanupError;
    if (!Stop(out.state, false, cleanupError)) {
        out.notice = cleanupError;
        return;
    }
    if (restorePrevious(QObject::tr("新的 TUN 设置启动失败，已恢复原来的 TUN 连接。"))) return;
    if (!Stop(out.state, true, cleanupError)) out.notice = cleanupError;
}

} // namespace NekoGui::TunLifecycle
