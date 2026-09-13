#include "OwnedElevatedProcess.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <atomic>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include "main/NekoGui.hpp"
#include <QSemaphore>
#include <QStandardPaths>
#include <QThread>
#endif

namespace NekoGui_sys {

struct OwnedElevatedProcess::State {
    QMutex mutex;
    bool started = false;
    qint64 pid = 0;
    void *handle = nullptr;
    QProcess *process = nullptr;
    std::atomic_bool running{false};

    ~State() {
#ifdef Q_OS_WIN
        if (handle) CloseHandle(static_cast<HANDLE>(handle));
#else
        // Successful shutdown already stopped the core through RPC. If the
        // GUI itself is destroyed unexpectedly, the core's explicit owner
        // watchdog is responsible for releasing its privileged resources.
        if (process) {
            auto owned = process;
            runOnUiThread([owned] { owned->deleteLater(); }, DS_cores);
        }
#endif
    }
};

OwnedElevatedProcess::OwnedElevatedProcess() : state(std::make_shared<State>()) {}
OwnedElevatedProcess::~OwnedElevatedProcess() = default;

QString OwnedElevatedProcess::WindowsCommandLine(const QStringList &arguments) {
    QStringList quoted;
    for (const auto &argument : arguments) {
        QString value = QStringLiteral("\"");
        int slashes = 0;
        for (const auto character : argument) {
            if (character == QLatin1Char('\\')) {
                ++slashes;
                continue;
            }
            if (character == QLatin1Char('"')) {
                value += QString(slashes * 2 + 1, QLatin1Char('\\'));
            } else {
                value += QString(slashes, QLatin1Char('\\'));
            }
            slashes = 0;
            value += character;
        }
        value += QString(slashes * 2, QLatin1Char('\\'));
        value += QLatin1Char('"');
        quoted.append(value);
    }
    return quoted.join(QLatin1Char(' '));
}

bool OwnedElevatedProcess::Start(const QString &program, const QStringList &arguments,
                                  const QString &workingDirectory, bool elevated, bool hideConsole, QString &error) {
    {
        QMutexLocker lock(&state->mutex);
        if (state->started) {
            error = QObject::tr("TUN 辅助进程已经启动过。");
            return false;
        }
        state->started = true;
    }
    const auto executable = QFileInfo(program).absoluteFilePath();
    if (!QFileInfo(executable).isFile()) {
        error = QObject::tr("缺少本版本配套的 sing-box 内核。");
        return false;
    }
#ifdef Q_OS_WIN
    const auto nativeProgram = QDir::toNativeSeparators(executable).toStdWString();
    const auto commandLine = WindowsCommandLine(arguments).toStdWString();
    const auto directory = QDir::toNativeSeparators(workingDirectory).toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = elevated ? L"runas" : L"open";
    info.lpFile = nativeProgram.c_str();
    info.lpParameters = commandLine.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = hideConsole ? SW_HIDE : SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const auto code = GetLastError();
        error = code == ERROR_CANCELLED ? QObject::tr("已取消系统授权，TUN 未开启。")
                                       : QObject::tr("无法启动 TUN 辅助进程（系统错误 %1）。").arg(code);
        if (info.hProcess) CloseHandle(info.hProcess);
        return false;
    }
    {
        QMutexLocker lock(&state->mutex);
        state->handle = info.hProcess;
        state->pid = GetProcessId(info.hProcess);
    }
    return true;
#else
    Q_UNUSED(hideConsole);
    if (!DS_cores) {
        error = QObject::tr("TUN 进程控制器尚未准备好。");
        return false;
    }
    QString launcher = executable;
    QStringList launchArguments = arguments;
    if (elevated) {
#ifdef Q_OS_MACOS
        auto shellQuote = [](QString text) {
            text.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            return QLatin1Char('\'') + text + QLatin1Char('\'');
        };
        QStringList command{shellQuote(executable)};
        for (const auto &argument : arguments) command += shellQuote(argument);
        QString script = QStringLiteral("cd ") + shellQuote(workingDirectory) + QStringLiteral(" && exec ") + command.join(QLatin1Char(' '));
        script.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        script.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        launcher = QStringLiteral("/usr/bin/osascript");
        launchArguments = {QStringLiteral("-e"), QStringLiteral("do shell script \"") + script + QStringLiteral("\" with administrator privileges")};
#else
        launcher = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (launcher.isEmpty()) {
            error = QObject::tr("未找到 pkexec，无法获得 TUN 所需的系统授权。");
            return false;
        }
        launchArguments.prepend(executable);
#endif
    }
    bool started = false;
    QSemaphore ready;
    const auto owned = state;
    runOnUiThread([owned, launcher, launchArguments, workingDirectory, &started, &ready] {
        auto process = new QProcess;
        {
            QMutexLocker lock(&owned->mutex);
            owned->process = process;
        }
        const auto weak = std::weak_ptr<State>(owned);
        QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
                         [weak](int, QProcess::ExitStatus) {
            if (auto state = weak.lock()) state->running = false;
        });
        // Never write the privileged command line, token, or core configuration
        // to the GUI log. User-visible failures are reported by the controller.
        QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process] { process->readAllStandardOutput(); });
        QObject::connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
        process->setWorkingDirectory(workingDirectory);
        process->start(launcher, launchArguments);
        started = process->waitForStarted(3000);
        if (started) {
            QMutexLocker lock(&owned->mutex);
            owned->pid = process->processId();
            owned->running = true;
        }
        ready.release();
    }, DS_cores);
    ready.acquire();
    if (!started) error = QObject::tr("无法启动 TUN 辅助进程，请检查系统授权。");
    return started;
#endif
}

bool OwnedElevatedProcess::Running() const {
#ifdef Q_OS_WIN
    QMutexLocker lock(&state->mutex);
    return state->handle && WaitForSingleObject(static_cast<HANDLE>(state->handle), 0) == WAIT_TIMEOUT;
#else
    return state->running.load();
#endif
}

qint64 OwnedElevatedProcess::Pid() const {
    QMutexLocker lock(&state->mutex);
    return state->pid;
}

void *OwnedElevatedProcess::NativeHandle() const {
    QMutexLocker lock(&state->mutex);
    return state->handle;
}

QProcess *OwnedElevatedProcess::WrapperProcess() const {
    QMutexLocker lock(&state->mutex);
    return state->process;
}

bool OwnedElevatedProcess::WaitForExit(int timeoutMs) const {
#ifdef Q_OS_WIN
    HANDLE handle;
    {
        QMutexLocker lock(&state->mutex);
        handle = static_cast<HANDLE>(state->handle);
    }
    return !handle || WaitForSingleObject(handle, DWORD(qMax(0, timeoutMs))) == WAIT_OBJECT_0;
#else
    bool stopped = !Running();
    if (stopped) return true;
    QSemaphore finished;
    const auto owned = state;
    runOnUiThread([owned, timeoutMs, &stopped, &finished] {
        stopped = !owned->process || owned->process->state() == QProcess::NotRunning || owned->process->waitForFinished(timeoutMs);
        if (stopped) owned->running = false;
        finished.release();
    }, DS_cores);
    finished.acquire();
    return stopped;
#endif
}

bool OwnedElevatedProcess::TerminateOwned(int timeoutMs, QString &error) {
    if (!Running()) return true;
#ifdef Q_OS_WIN
    HANDLE handle;
    {
        QMutexLocker lock(&state->mutex);
        handle = static_cast<HANDLE>(state->handle);
    }
    if (!TerminateProcess(handle, 0) && Running()) {
        error = QObject::tr("无法结束本次启动的 TUN 辅助进程（系统错误 %1）。").arg(GetLastError());
        return false;
    }
#else
    QSemaphore requested;
    const auto owned = state;
    runOnUiThread([owned, &requested] {
        if (owned->process && owned->process->state() != QProcess::NotRunning) owned->process->kill();
        requested.release();
    }, DS_cores);
    requested.acquire();
#endif
    if (!WaitForExit(timeoutMs)) {
        error = QObject::tr("TUN 已停止，但其辅助进程尚未退出；请重试关闭。");
        return false;
    }
    return true;
}

} // namespace NekoGui_sys
