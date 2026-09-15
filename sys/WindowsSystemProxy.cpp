#include "WindowsSystemProxy.hpp"
#include "SystemProxyLease.hpp"

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#include <ras.h>
#include <raserror.h>
#include <vector>

namespace NekoGui_sys {
namespace {
QString WindowsError(const QString &operation, DWORD code) {
    return operation + QStringLiteral(" (Windows error %1)").arg(code);
}

class WindowsProxyBackend final : public SystemProxyBackend {
public:
    bool Connections(QStringList &connections, QString &error) override {
        connections = QStringList{QString{}}; // LAN connection
        RASENTRYNAMEW first{};
        first.dwSize = sizeof(first);
        DWORD bytes = sizeof(first), count = 0;
        auto code = RasEnumEntriesW(nullptr, nullptr, &first, &bytes, &count);
        std::vector<RASENTRYNAMEW> entries;
        if (code == ERROR_BUFFER_TOO_SMALL) {
            entries.resize((bytes + sizeof(RASENTRYNAMEW) - 1) / sizeof(RASENTRYNAMEW));
            entries.front().dwSize = sizeof(RASENTRYNAMEW);
            code = RasEnumEntriesW(nullptr, nullptr, entries.data(), &bytes, &count);
        } else if (code == ERROR_SUCCESS && count > 0) {
            entries.push_back(first);
        }
        if (code != ERROR_SUCCESS) {
            error = WindowsError(QStringLiteral("Cannot enumerate system proxy connections"), code);
            return false;
        }
        for (DWORD i = 0; i < count; ++i) connections.append(QString::fromWCharArray(entries[i].szEntryName));
        return true;
    }

    bool Read(const QString &connection, SystemProxyState &state, QString &error) override {
        INTERNET_PER_CONN_OPTIONW options[4]{};
        options[0].dwOption = INTERNET_PER_CONN_FLAGS;
        options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
        options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
        options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
        INTERNET_PER_CONN_OPTION_LISTW list{};
        auto name = connection.toStdWString();
        list.dwSize = sizeof(list);
        list.pszConnection = name.empty() ? nullptr : name.data();
        list.dwOptionCount = 4;
        list.pOptions = options;
        DWORD bytes = sizeof(list);
        const bool ok = InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &bytes);
        const auto code = GetLastError();
        if (ok) {
            state.flags = options[0].Value.dwValue;
            state.server = QString::fromWCharArray(options[1].Value.pszValue);
            state.bypass = QString::fromWCharArray(options[2].Value.pszValue);
            state.autoConfigUrl = QString::fromWCharArray(options[3].Value.pszValue);
        } else {
            error = WindowsError(QStringLiteral("Cannot read system proxy settings"), code);
        }
        for (int i = 1; i < 4; ++i) {
            if (options[i].Value.pszValue) GlobalFree(options[i].Value.pszValue);
        }
        return ok;
    }

    bool Write(const QString &connection, const SystemProxyState &state, QString &error) override {
        auto name = connection.toStdWString();
        auto server = state.server.toStdWString();
        auto bypass = state.bypass.toStdWString();
        auto autoConfig = state.autoConfigUrl.toStdWString();
        INTERNET_PER_CONN_OPTIONW options[4]{};
        options[0].dwOption = INTERNET_PER_CONN_FLAGS;
        options[0].Value.dwValue = state.flags;
        options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
        options[1].Value.pszValue = server.data();
        options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
        options[2].Value.pszValue = bypass.data();
        options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
        options[3].Value.pszValue = autoConfig.data();
        INTERNET_PER_CONN_OPTION_LISTW list{};
        list.dwSize = sizeof(list);
        list.pszConnection = name.empty() ? nullptr : name.data();
        list.dwOptionCount = 4;
        list.pOptions = options;
        if (!InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof(list))) {
            error = WindowsError(QStringLiteral("Cannot update system proxy settings"), GetLastError());
            return false;
        }
        return true;
    }
};

WindowsProxyBackend backend;
SystemProxyLease lease(backend);

void NotifyChanged() {
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
}
} // namespace

bool ApplyWindowsSystemProxy(const QString &server, QString &error) {
    const bool ok = lease.Apply(server, error);
    NotifyChanged();
    return ok;
}

bool RestoreWindowsSystemProxy(QString &error) {
    const bool ok = lease.Restore(error);
    NotifyChanged();
    return ok;
}

bool WindowsSystemProxyRestorePending() { return lease.HasPendingRestore(); }
} // namespace NekoGui_sys
#endif
