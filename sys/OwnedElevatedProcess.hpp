#pragma once

#include <QString>
#include <QStringList>
#include <memory>

class QProcess;

namespace NekoGui_sys {

// A process started by this application, with retained ownership. No operation
// discovers or terminates processes by executable name. Blocking methods are
// called on workers; Running/Pid/NativeHandle are short, thread-safe snapshots.
class OwnedElevatedProcess {
public:
    OwnedElevatedProcess();
    ~OwnedElevatedProcess();
    OwnedElevatedProcess(const OwnedElevatedProcess &) = delete;
    OwnedElevatedProcess &operator=(const OwnedElevatedProcess &) = delete;

    bool Start(const QString &program, const QStringList &arguments, const QString &workingDirectory,
               bool elevated, bool hideConsole, QString &error);
    [[nodiscard]] bool Running() const;
    [[nodiscard]] qint64 Pid() const;
    [[nodiscard]] void *NativeHandle() const;
    [[nodiscard]] QProcess *WrapperProcess() const;
    bool WaitForExit(int timeoutMs) const;
    // Only use after RPC Stop has acknowledged that TUN is closed, or before
    // any Start RPC was sent. The handle/pointer identifies this launch only.
    bool TerminateOwned(int timeoutMs, QString &error);

    static QString WindowsCommandLine(const QStringList &arguments);

private:
    struct State;
    std::shared_ptr<State> state;
};

} // namespace NekoGui_sys
