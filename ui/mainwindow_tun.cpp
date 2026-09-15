#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "CoreRuntime.hpp"
#include "TunLifecycle.hpp"
#include "db/traffic/TrafficLooper.hpp"
#include "sys/OwnedElevatedProcess.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSemaphore>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <optional>

using namespace NekoGui;

#ifndef NKR_NO_GRPC
namespace {

struct TunDaemon {
    std::shared_ptr<NekoGui_sys::OwnedElevatedProcess> process;
    std::shared_ptr<NekoGui_rpc::Client> client;
    QString buildIdentity;
};

using TunState = TunLifecycle::State<TunDaemon, libcore::LoadConfigReq>;
using TunOutcome = TunLifecycle::Outcome<TunState>;
using TunLifecycle::Stop;

struct TunStartOptions {
    NekoGui_sys::CoreManager *manager = nullptr;
    QString program;
    QString workingDirectory;
    QString fingerprint;
    libcore::LoadConfigReq request;
    qint64 ownerPid = 0;
    bool elevated = true;
    bool hideConsole = true;
    std::function<QString()> validate;
};

struct TunRequest {
    bool enable = false;
    bool save = true;
    TunStartOptions options;
};

void IncludeExternalCorePaths(BuildConfigContext &context, const std::shared_ptr<RunningCoreSession> &session) {
    const auto add = [&](const QString &program) {
        if (program.isEmpty()) return;
        auto resolved = program;
        if (!QFileInfo(program).isAbsolute()) {
            resolved = QStandardPaths::findExecutable(program);
            if (resolved.isEmpty()) resolved = QFileInfo(program).absoluteFilePath();
        }
        const QFileInfo file(resolved);
        const auto canonical = file.canonicalFilePath();
        context.tunBypassProcessPaths += canonical.isEmpty() ? file.absoluteFilePath() : canonical;
    };
    // Register all configured helpers before TUN starts, so changing nodes
    // later cannot send a newly selected helper's own sockets back into TUN.
    // Match full paths, never every process with the same executable name.
    const auto configured = QString2QJsonObject(context.settings->extraCore->core_map);
    for (auto item = configured.begin(); item != configured.end(); ++item) add(item.value().toString());
    if (session) for (const auto &external : session->external) add(external->program);
    context.tunBypassProcessPaths.removeDuplicates();
    context.tunBypassProcessPaths.sort();
}

bool CheckTun(const std::shared_ptr<NekoGui_rpc::Client> &client, const libcore::LoadConfigReq &request,
              const QString &identity, QString &error) {
    bool ok = false;
    const auto response = client->Check(&ok, request);
    if (!ok) {
        error = QObject::tr("无法完成 TUN 配置检查，现有 TUN 状态已保留；请检查配套内核是否正常运行。");
        return false;
    }
    if (!response.error().empty() || response.failure_category() != libcore::FailureNone) {
        error = response.failure_category() == libcore::FailureUnsupported
                    ? QObject::tr("当前 sing-box 不支持这份 TUN 配置，请检查 TUN、DNS 和路由设置。")
                    : QObject::tr("TUN 配置未通过内核检查，请检查 TUN、DNS 和路由设置；原配置已保留。");
        return false;
    }
    const auto configHash = QStringLiteral("sha256:") + QString::fromLatin1(QCryptographicHash::hash(
        QByteArray::fromStdString(request.core_config()), QCryptographicHash::Sha256).toHex());
    if (response.core_family() != "sing-box" || QString::fromStdString(response.build_id()) != identity ||
        QString::fromStdString(response.config_hash()) != configHash) {
        error = QObject::tr("TUN 配置检查与实际内核不一致，请使用本版本配套的 sing-box 并重启客户端。");
        return false;
    }
    return true;
}

TunOutcome EnableTun(TunState current, const TunStartOptions &options, const std::shared_ptr<std::atomic_bool> &cancelled) {
    TunOutcome out;
    out.state = std::move(current);
    const auto cancelledNow = [&] { return cancelled->load(); };
    const auto stillValid = [&] {
        if (cancelledNow()) return false;
        QString invalid;
        QSemaphore checked;
        runOnUiThread([&] {
            if (options.validate) invalid = options.validate();
            checked.release();
        }, QCoreApplication::instance());
        checked.acquire();
        if (!invalid.isEmpty()) out.notice = invalid;
        return invalid.isEmpty() && !cancelledNow();
    };
    if (cancelledNow()) return out;
    QFile binary(options.program);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!binary.open(QIODevice::ReadOnly) || !hash.addData(&binary)) {
        out.notice = QObject::tr("无法校验本版本配套的 sing-box 内核，TUN 状态未更改。");
        return out;
    }
    const auto identity = QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex());
    binary.close();
    if (out.state.daemon && !out.state.daemon->process->Running()) out.state = {};
    if (out.state.enabled && !out.state.uncertain && out.state.fingerprint == options.fingerprint &&
        out.state.daemon && out.state.daemon->buildIdentity == identity) {
        out.succeeded = true;
        return out;
    }

    // Validate without elevation first. This daemon is independent of the
    // active node and Check cannot create TUN/listeners or change system routes.
    QString readyError;
    if (!options.manager || !options.manager->EnsureReady(CoreType::SING_BOX, readyError)) {
        out.notice = QObject::tr("sing-box 控制接口尚未准备好，TUN 状态未更改；请使用本版本配套的内核。");
        return out;
    }
    if (cancelledNow()) return out;
    auto preflight = options.manager->Get(CoreType::SING_BOX);
    if (!CheckTun(preflight->client, options.request, identity, out.notice)) return out;
    if (!stillValid()) return out;
    if (out.state.daemon && out.state.daemon->buildIdentity != identity) {
        out.notice = QObject::tr("运行中的 TUN 使用另一份内核；请先关闭 TUN，再重新开启以加载更新。");
        return out;
    }

    bool newlyLaunched = false;
    if (!out.state.daemon) {
        const int port = MkPort();
        if (!IsValidPort(port)) {
            out.notice = QObject::tr("无法为 TUN 分配独立控制端口，请稍后重试。");
            return out;
        }
        const auto token = GetRandomString(64);
        auto daemon = std::make_shared<TunDaemon>();
        daemon->process = std::make_shared<NekoGui_sys::OwnedElevatedProcess>();
        daemon->client = std::make_shared<NekoGui_rpc::Client>([](const QString &) {},
                         QStringLiteral("127.0.0.1:") + QString::number(port), token);
        daemon->buildIdentity = identity;
        const QStringList arguments{QStringLiteral("nekobox"), QStringLiteral("-port"), QString::number(port),
                                    QStringLiteral("-token"), token, QStringLiteral("-parent-pid"), QString::number(options.ownerPid)};
        if (!daemon->process->Start(options.program, arguments, options.workingDirectory,
                                   options.elevated, options.hideConsole, out.notice)) return out;
        out.state.daemon = daemon;
        newlyLaunched = true;
        QElapsedTimer ready;
        ready.start();
        bool verified = false;
#ifdef Q_OS_WIN
        const int readyTimeout = 8000; // ShellExecuteEx has already completed UAC.
#else
        const int readyTimeout = 60000; // pkexec/osascript may still await approval.
#endif
        while (!cancelledNow() && daemon->process->Running() && ready.elapsed() < readyTimeout) {
            bool ok = false;
            const auto capabilities = daemon->client->Capabilities(&ok);
            if (ok) {
                bool watchdog = false;
                bool tun = false;
                for (const auto &feature : capabilities.features()) {
                    watchdog = watchdog || feature == "owner-process-watchdog";
                    tun = tun || feature == "tun";
                }
                verified = capabilities.core_family() == "sing-box" && watchdog && tun &&
                           QString::fromStdString(capabilities.build_id()) == identity;
                break;
            }
            QThread::msleep(100);
        }
        if (!verified || cancelledNow()) {
            QString cleanupError;
            if (!TunLifecycle::DiscardUnstarted(out.state, cleanupError)) out.notice = cleanupError;
            else if (!cancelledNow()) out.notice = QObject::tr("TUN 辅助进程未完成授权或身份校验，TUN 未开启。");
            return out;
        }
    }
    if (!CheckTun(out.state.daemon->client, options.request, identity, out.notice) || !stillValid()) {
        if (newlyLaunched) {
            QString cleanupError;
            if (!TunLifecycle::DiscardUnstarted(out.state, cleanupError)) out.notice = cleanupError;
        }
        return out;
    }

    TunLifecycle::Apply(out, options.request, options.fingerprint, cancelledNow, stillValid);
    return out;
}

class OwnedTunRuntime : public QObject {
public:
    explicit OwnedTunRuntime(QObject *parent) : QObject(parent) {
        setObjectName(QStringLiteral("ownedTunRuntime"));
        auto monitor = new QTimer(this);
        monitor->setInterval(1000);
        connect(monitor, &QTimer::timeout, this, [this] {
            if (!busy && !bridgePreparing && deferredReady && deferredReady()) {
                auto resume = std::move(deferredResume);
                deferredReady = {};
                if (resume) resume();
            }
            if (busy || !state.daemon || state.daemon->process->Running()) return;
            const bool interrupted = state.enabled || state.uncertain;
            state = {};
            desiredEnabled = false;
            if (changed) changed();
            if (interrupted && notice) notice(QObject::tr("TUN 辅助进程已退出，TUN 已关闭；请重新开启。"));
            if (idle) idle();
        });
        monitor->start();
    }

    ~OwnedTunRuntime() override {
        if (cancelled) cancelled->store(true);
    }

    TunState state;
    bool busy = false;
    bool requestedSave = true;
    bool desiredEnabled = false;
    bool bridgePreparing = false;
    bool bridgeSave = true;
    std::function<void()> changed;
    std::function<void(const QString &)> notice;
    std::function<void(bool)> remember;
    std::function<void()> enabled;
    std::function<void()> idle;

    void Enable(const TunStartOptions &options, bool save) {
        desiredEnabled = true;
        deferredReady = {};
        deferredResume = {};
        Queue(TunRequest{true, save, options});
    }

    void Disable(bool save) {
        desiredEnabled = false;
        deferredReady = {};
        deferredResume = {};
        Queue(TunRequest{false, save, {}});
    }

    void AwaitCore(std::function<bool()> ready, std::function<void()> resume) {
        deferredReady = std::move(ready);
        deferredResume = std::move(resume);
    }

private:
    std::optional<TunRequest> pending;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::function<bool()> deferredReady;
    std::function<void()> deferredResume;

    void Queue(const TunRequest &request) {
        if (!request.enable && !busy && !state.daemon) {
            state = {};
            if (request.save && remember) remember(false);
            if (changed) changed();
            return;
        }
        pending = request;
        if (cancelled) cancelled->store(true);
        Pump();
    }

    void Pump() {
        if (busy || !pending) return;
        const auto request = *pending;
        pending.reset();
        busy = true;
        cancelled = std::make_shared<std::atomic_bool>(false);
        const auto cancellation = cancelled;
        const auto before = state;
        if (changed) changed();
        const auto guarded = QPointer<OwnedTunRuntime>(this);
        runOnNewThread([guarded, request, cancellation, before] {
            TunOutcome out;
            if (request.enable) out = EnableTun(before, request.options, cancellation);
            else {
                out.state = before;
                out.succeeded = Stop(out.state, true, out.notice);
            }
            if (!guarded) {
                QString ignored;
                Stop(out.state, true, ignored);
                return;
            }
            runOnUiThread([guarded, request, cancellation, out = std::move(out)]() mutable {
                if (!guarded) return;
                auto &runtime = *guarded;
                runtime.state = std::move(out.state);
                runtime.busy = false;
                const bool superseded = runtime.pending.has_value();
                if (!superseded && request.enable && !out.succeeded) runtime.desiredEnabled = runtime.state.enabled;
                if (!superseded && request.save && runtime.remember &&
                    (out.succeeded || (!request.enable && !runtime.state.enabled && !runtime.state.uncertain)))
                    runtime.remember(request.enable);
                if (!superseded && !out.notice.isEmpty() && runtime.notice) runtime.notice(out.notice);
                if (runtime.pending) runtime.Pump();
                else {
                    if (runtime.changed) runtime.changed();
                    if (out.succeeded && request.enable && runtime.enabled) runtime.enabled();
                    if (runtime.idle) runtime.idle();
                }
            }, QCoreApplication::instance());
        });
    }
};

OwnedTunRuntime *TunRuntimeFor(MainWindow *window) {
    for (auto *child : window->children()) {
        if (auto *runtime = dynamic_cast<OwnedTunRuntime *>(child)) return runtime;
    }
    return new OwnedTunRuntime(window);
}

} // namespace
#endif

void MainWindow::neko_set_spmode_vpn(bool enable, bool save) {
#ifndef NKR_NO_GRPC
    if (enable && dataStore->prepare_exit) return;
    auto runtime = TunRuntimeFor(this);
    const auto guarded = QPointer<MainWindow>(this);
    runtime->requestedSave = save;
    runtime->desiredEnabled = enable;
    runtime->changed = [guarded, runtime] {
        if (!guarded) return;
        auto window = guarded.data();
        window->vpn_changing = runtime->busy;
        dataStore->spmode_vpn = runtime->state.enabled;
        const auto daemon = runtime->state.daemon;
        window->vpn_pid = daemon ? daemon->process->Pid() : 0;
        window->vpn_handle = daemon ? daemon->process->NativeHandle() : nullptr;
        window->vpn_process = daemon ? daemon->process->WrapperProcess() : nullptr;
        window->vpn_client = daemon ? daemon->client : nullptr;
        window->vpn_config_fingerprint = runtime->state.enabled ? runtime->state.fingerprint : QString{};
        {
            QMutexLocker lock(&NekoGui_traffic::trafficLooper->loop_mutex);
            ++NekoGui_traffic::trafficLooper->generation;
            NekoGui_traffic::trafficLooper->direct_client = runtime->state.enabled && !runtime->busy ? window->vpn_client : nullptr;
        }
        window->refresh_status();
        window->ui->checkBox_VPN->setEnabled(!runtime->busy && !runtime->bridgePreparing && !dataStore->prepare_exit);
        window->ui->menu_spmode_vpn->setEnabled(!runtime->busy && !runtime->bridgePreparing && !dataStore->prepare_exit);
        if (!runtime->busy && !runtime->bridgePreparing && !dataStore->prepare_exit) window->process_core_intent();
    };
    runtime->notice = [guarded](const QString &message) {
        if (!guarded) return;
        guarded->core_notice_message(message);
    };
    runtime->enabled = [guarded] {
        if (!guarded) return;
        guarded->core_notice_message(guarded->core_session ? tr("TUN 已开启，切换节点或内核时将保持运行。")
                                                         : tr("TUN 已开启，选择并连接节点后即可代理。"));
    };
    runtime->remember = [](bool enabled) {
        dataStore->remember_spmode.removeAll("vpn");
        if (enabled) dataStore->remember_spmode.append("vpn");
    };
    runtime->idle = [guarded, runtime] {
        if (!guarded || !dataStore->prepare_exit || runtime->busy) return;
        if (runtime->state.daemon) {
            dataStore->prepare_exit = false;
            if (runtime->changed) runtime->changed();
            guarded->core_notice_message(tr("TUN 清理尚未完成，已暂停退出；请重试关闭 TUN 或再次退出。"));
            return;
        }
        QTimer::singleShot(0, guarded, [guarded] {
            if (guarded && dataStore->prepare_exit) guarded->on_menu_exit_triggered();
        });
    };
    if (enable) {
        if (dataStore->prepare_exit) return;
        if (!StartVPNProcess()) {
            runtime->desiredEnabled = runtime->state.enabled;
            runtime->changed();
        }
    } else {
        runtime->Disable(save);
    }
#else
    dataStore->spmode_vpn = false;
    if (enable) core_notice_message(tr("此版本未启用内核 RPC，无法启动独立 TUN。"));
    else if (save) dataStore->remember_spmode.removeAll("vpn");
    refresh_status();
#endif
}

bool MainWindow::StartVPNProcess() {
#ifndef NKR_NO_GRPC
    auto runtime = TunRuntimeFor(this);
    if (dataStore->prepare_exit) return false;
    if (runtime->bridgePreparing) return true;
    if (!core_manager || !IsValidPort(tun_proxy_port)) {
        core_notice_message(tr("TUN 控制器尚未准备好，请等待内核初始化后重试。"));
        return false;
    }
    if (core_switch_busy) {
        const auto guarded = QPointer<MainWindow>(this);
        runtime->AwaitCore([guarded] { return guarded && !guarded->core_switch_busy; }, [guarded, runtime] {
            if (guarded && runtime->desiredEnabled && !dataStore->prepare_exit)
                guarded->neko_set_spmode_vpn(true, runtime->requestedSave);
        });
        core_notice_message(tr("节点或内核切换完成后将自动应用 TUN 设置。"));
        return true;
    }
    auto context = CaptureBuildConfigContext(CoreType::SING_BOX, true);
    context.tunProxyPort = tun_proxy_port;
    context.settings->spmode_vpn = true;
    IncludeExternalCorePaths(context, core_session);
    const auto config = BuildVPNSingBoxConfig(context);
    if (!config->error.isEmpty()) {
        core_notice_message(config->error);
        return false;
    }
    if (core_session && core_session->built->needKeepVpnOff) {
        runtime->bridgePreparing = true;
        runtime->bridgeSave = runtime->requestedSave;
        if (runtime->changed) runtime->changed();
        core_notice_message(tr("正在为当前节点准备 TUN 代理入口，完成后将继续开启 TUN。"));
        queue_core_start(core_session->profile, -1, true);
        return true;
    }
    TunStartOptions options;
    options.manager = core_manager;
    options.program = core_manager->Get(CoreType::SING_BOX)->program;
    options.workingDirectory = QDir::currentPath();
    options.fingerprint = QString::fromLatin1(config->Fingerprint()) + QLatin1Char(':') + QString::fromLatin1(context.Fingerprint());
    options.request.set_core_config(QJsonObject2QString(config->coreConfig, true).toStdString());
    options.request.add_stats_outbounds("bypass");
    options.ownerPid = QApplication::applicationPid();
    options.elevated = !NekoGui::IsAdmin();
    options.hideConsole = context.settings->vpn_hide_console;
    const auto guarded = QPointer<MainWindow>(this);
    const auto expected = options.fingerprint;
    options.validate = [guarded, expected]() -> QString {
        if (!guarded || dataStore->prepare_exit) return tr("已取消 TUN 启动。");
        if (guarded->core_switch_busy || (guarded->core_session && guarded->core_session->built->needKeepVpnOff))
            return tr("当前连接已改变，请等待切换完成后重新开启 TUN。");
        auto latest = CaptureBuildConfigContext(CoreType::SING_BOX, true);
        latest.tunProxyPort = guarded->tun_proxy_port;
        latest.settings->spmode_vpn = true;
        IncludeExternalCorePaths(latest, guarded->core_session);
        const auto checked = BuildVPNSingBoxConfig(latest);
        if (!checked->error.isEmpty()) return checked->error;
        if (expected != QString::fromLatin1(checked->Fingerprint()) + QLatin1Char(':') + QString::fromLatin1(latest.Fingerprint()))
            return tr("TUN 设置在启动期间发生变化，请重新开启以应用当前设置。");
        return {};
    };
    core_notice_message(runtime->state.enabled ? tr("正在检查新的 TUN 设置…") : tr("正在检查 TUN 配置并等待系统授权…"));
    runtime->Enable(options, runtime->requestedSave);
    return true;
#else
    return false;
#endif
}

void MainWindow::tun_bridge_prepared(bool succeeded, bool cancelled) {
#ifndef NKR_NO_GRPC
    auto runtime = TunRuntimeFor(this);
    if (!runtime->bridgePreparing) return;
    runtime->bridgePreparing = false;
    if (!runtime->desiredEnabled || dataStore->prepare_exit) {
        if (runtime->changed) runtime->changed();
        return;
    }
    if (succeeded) {
        if (core_session && core_session->built->needKeepVpnOff) {
            runtime->desiredEnabled = false;
            core_notice_message(tr("当前外部协议无法建立兼容 TUN 的代理入口，TUN 未开启；可改用原生内核支持的节点。"));
            if (runtime->changed) runtime->changed();
        } else {
            neko_set_spmode_vpn(true, runtime->bridgeSave);
        }
    } else if (cancelled) {
        const auto guarded = QPointer<MainWindow>(this);
        runtime->AwaitCore([guarded] { return guarded && !guarded->core_switch_busy; }, [guarded, runtime] {
            if (guarded && runtime->desiredEnabled && !dataStore->prepare_exit)
                guarded->neko_set_spmode_vpn(true, runtime->bridgeSave);
        });
        if (runtime->changed) runtime->changed();
    } else {
        runtime->desiredEnabled = false;
        core_notice_message(tr("未能建立 TUN 所需的本地代理入口，TUN 未开启。"));
        if (runtime->changed) runtime->changed();
    }
#else
    Q_UNUSED(succeeded);
    Q_UNUSED(cancelled);
#endif
}

void MainWindow::reload_tun_settings() {
#ifndef NKR_NO_GRPC
    auto runtime = TunRuntimeFor(this);
    if (runtime->desiredEnabled || runtime->state.enabled)
        neko_set_spmode_vpn(true, runtime->requestedSave);
#endif
}

bool MainWindow::StopVPNProcess(bool unconditional) {
    Q_UNUSED(unconditional);
#ifndef NKR_NO_GRPC
    neko_set_spmode_vpn(false, !dataStore->prepare_exit);
    const auto runtime = QPointer<OwnedTunRuntime>(TunRuntimeFor(this));
    // The troubleshooting action needs a completion result. Keep the GUI event
    // loop responsive while the worker performs the owned RPC/process cleanup.
    QEventLoop wait;
    QTimer timer;
    timer.setInterval(20);
    connect(&timer, &QTimer::timeout, &wait, [&] {
        if (!runtime || !runtime->busy) wait.quit();
    });
    timer.start();
    if (runtime->busy) wait.exec();
    return !runtime || (!runtime->state.enabled && !runtime->state.uncertain && !runtime->state.daemon);
#else
    return true;
#endif
}
