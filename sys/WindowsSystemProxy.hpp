#pragma once

#include <QString>

namespace NekoGui_sys {
bool ApplyWindowsSystemProxy(const QString &server, QString &error);
bool RestoreWindowsSystemProxy(QString &error);
bool WindowsSystemProxyRestorePending();
}
