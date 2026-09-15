#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace NekoGui_sys {

struct SystemProxyState {
    unsigned long flags = 1; // WinINet PROXY_TYPE_DIRECT
    QString server;
    QString bypass;
    QString autoConfigUrl;

    bool operator==(const SystemProxyState &other) const {
        return flags == other.flags && server == other.server && bypass == other.bypass &&
               autoConfigUrl == other.autoConfigUrl;
    }
};

class SystemProxyBackend {
public:
    virtual ~SystemProxyBackend() = default;
    virtual bool Connections(QStringList &connections, QString &error) = 0;
    virtual bool Read(const QString &connection, SystemProxyState &state, QString &error) = 0;
    virtual bool Write(const QString &connection, const SystemProxyState &state, QString &error) = 0;
};

// Used only on the GUI thread. A saved value is restored only while the OS still
// has the value this instance installed; a later change by another application
// or by the user is left intact. The backend is injectable for tests which must
// never modify the developer's active network settings.
class SystemProxyLease {
public:
    explicit SystemProxyLease(SystemProxyBackend &backend) : backend(backend) {}

    bool Apply(const QString &server, QString &error) {
        error.clear();
        if (server.isEmpty()) {
            error = QStringLiteral("The system proxy address is empty.");
            return false;
        }
        if (!entries.isEmpty() && !Restore(error)) return false;
        QStringList connections;
        if (!backend.Connections(connections, error)) return false;
        QMap<QString, SystemProxyState> before;
        for (const auto &connection : connections) {
            SystemProxyState state;
            if (!backend.Read(connection, state, error)) return false;
            before.insert(connection, state);
        }
        for (auto it = before.cbegin(); it != before.cend(); ++it) {
            auto applied = it.value();
            applied.flags = 3; // PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY
            applied.server = server;
            entries.insert(it.key(), Entry{it.value(), applied, true});
            QString writeError, readError;
            const bool written = backend.Write(it.key(), applied, writeError);
            SystemProxyState observed;
            const bool read = backend.Read(it.key(), observed, readError);
            if (written && read && observed == applied) {
                entries[it.key()].uncertain = false;
                continue;
            }
            if (!written) AppendError(error, writeError.isEmpty() ? QStringLiteral("Cannot update system proxy settings.") : writeError);
            if (!read) AppendError(error, readError.isEmpty() ? QStringLiteral("Cannot verify system proxy settings.") : readError);
            else if (written) AppendError(error, QStringLiteral("System proxy settings did not match the requested values."));
            QString rollbackError;
            Restore(rollbackError);
            if (!rollbackError.isEmpty()) AppendError(error, rollbackError);
            return false;
        }
        return true;
    }

    bool Restore(QString &error) {
        error.clear();
        const auto connections = entries.keys();
        for (const auto &connection : connections) {
            const auto saved = entries.value(connection);
            SystemProxyState current;
            QString operationError;
            if (!backend.Read(connection, current, operationError)) {
                AppendError(error, operationError);
                continue;
            }
            if (current == saved.before) {
                entries.remove(connection);
                continue;
            }
            if (!(current == saved.applied) && !(saved.uncertain && IsOwnedPartial(current, saved))) {
                if (saved.uncertain) {
                    // An unconfirmed write may have changed only some fields.
                    // Do not overwrite unrelated changes, or claim cleanup.
                    AppendError(error, QStringLiteral("System proxy recovery is pending because the settings changed during an unconfirmed update. Check the system settings and retry."));
                } else {
                    // A confirmed setting was subsequently changed elsewhere.
                    entries.remove(connection);
                }
                continue;
            }
            entries[connection].uncertain = true;
            const bool written = backend.Write(connection, saved.before, operationError);
            SystemProxyState restored;
            QString readError;
            if (backend.Read(connection, restored, readError) && restored == saved.before) {
                // Readback is authoritative even if the write reported failure.
                entries.remove(connection);
                continue;
            }
            if (!written && !operationError.isEmpty()) AppendError(error, operationError);
            AppendError(error, readError.isEmpty() ? QStringLiteral("System proxy restoration has not been confirmed; retry before exiting.") : readError);
        }
        return entries.isEmpty();
    }

    bool HasPendingRestore() const { return !entries.isEmpty(); }

private:
    struct Entry { SystemProxyState before; SystemProxyState applied; bool uncertain; };
    static bool IsOwnedPartial(const SystemProxyState &current, const Entry &saved) {
        // Apply only changes flags and server. A partial update is ours only
        // when every field is one of the saved/requested values and no other
        // setting (PAC or bypass) has changed.
        return (current.flags == saved.before.flags || current.flags == saved.applied.flags) &&
               (current.server == saved.before.server || current.server == saved.applied.server) &&
               current.bypass == saved.before.bypass && current.autoConfigUrl == saved.before.autoConfigUrl;
    }
    static void AppendError(QString &error, const QString &message) {
        if (!error.isEmpty()) error += QLatin1Char('\n');
        error += message;
    }
    SystemProxyBackend &backend;
    QMap<QString, Entry> entries;
};

} // namespace NekoGui_sys
