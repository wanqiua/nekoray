#include "sys/OwnedElevatedProcess.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--owned-test-child"))) {
        QTimer::singleShot(20000, &application, &QCoreApplication::quit);
        return application.exec();
    }
#ifdef Q_OS_WIN
    using NekoGui_sys::OwnedElevatedProcess;
    const QStringList arguments{QStringLiteral("test-program"), QString{}, QStringLiteral("plain"),
                                QStringLiteral("space inside"), QStringLiteral("C:\\path with space\\"),
                                QStringLiteral("embedded\"quote"), QStringLiteral("slash\\\"quote"),
                                QStringLiteral("中文路径\\末尾\\"), QStringLiteral("& $() ` ;")};
    const auto encoded = OwnedElevatedProcess::WindowsCommandLine(arguments).toStdWString();
    int count = 0;
    auto decoded = CommandLineToArgvW(encoded.c_str(), &count);
    bool correct = decoded && count == arguments.size();
    if (correct) {
        for (int i = 0; i < count; ++i) correct = correct && QString::fromWCharArray(decoded[i]) == arguments[i];
    }
    if (decoded) LocalFree(decoded);
    if (!correct) {
        qCritical() << "Windows argument quoting did not round-trip";
        return 1;
    }

    // Two independent, hidden copies of this harmless test executable share
    // the same image name. Ending one must leave the other running.
    OwnedElevatedProcess first;
    OwnedElevatedProcess second;
    QString error;
    const auto program = QCoreApplication::applicationFilePath();
    const auto directory = QCoreApplication::applicationDirPath();
    const QStringList child{QStringLiteral("--owned-test-child")};
    if (!first.Start(program, child, directory, false, true, error)) {
        qCritical() << "First unelevated test launch failed" << error;
        return 1;
    }
    if (!second.Start(program, child, directory, false, true, error)) {
        first.TerminateOwned(3000, error);
        qCritical() << "Second unelevated test launch failed";
        return 1;
    }
    const bool ownership = first.Pid() > 0 && second.Pid() > 0 && first.Pid() != second.Pid() &&
                           first.NativeHandle() && second.NativeHandle() && first.Running() && second.Running();
    const bool firstStopped = first.TerminateOwned(3000, error) && !first.Running();
    const bool secondSurvived = second.Running();
    const bool secondStopped = second.TerminateOwned(3000, error) && !second.Running();
    if (!ownership || !firstStopped || !secondSurvived || !secondStopped) {
        qCritical() << "Owned process isolation failed";
        return 1;
    }
    qInfo() << "PASS: argument quoting and independent retained process handles";
#endif
    return 0;
}
