// Test-only entry point. The production startup code is included unchanged;
// only its final UI initializer is wrapped to schedule a bounded acceptance
// action followed by the normal asynchronous Exit command.
#include <windows.h>
#include <shellapi.h>

#include "ui/mainwindow.h"
#include "db/Database.hpp"

#include <QApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

namespace {
    QString fixtureRoot;
    QString reportPath;
    QString action;
    QJsonObject fixtureMarker;
    QJsonObject result;
    bool actionCompleted = false;
    bool exitedNormally = false;

    QJsonObject readObject(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        return error.error == QJsonParseError::NoError ? document.object() : QJsonObject{};
    }

    bool plainPath(const QString &path) {
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    bool verifyFixture(const QString &executable) {
        if (!plainPath(fixtureRoot) || !plainPath(fixtureRoot + "/package-fixture.json")) return false;
        fixtureMarker = readObject(fixtureRoot + "/package-fixture.json");
        if (fixtureMarker.value("kind") != "nekoray-package-synthetic-config" ||
            fixtureMarker.value("schema").toInt() != 1) return false;
        const auto core = fixtureMarker.value("source_core").toInt(-1);
        if (core != 0 && core != 1) return false;
        const auto payloadMarker = readObject(QFileInfo(executable).absolutePath() + "/package-acceptance-owned.json");
        if (payloadMarker.value("kind") != "nekoray-package-acceptance-payload" ||
            !payloadMarker.value("fixture_run_ids").toArray().contains(fixtureMarker.value("run_id"))) return false;
        QDirIterator entries(fixtureRoot, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                             QDirIterator::Subdirectories);
        while (entries.hasNext()) if (!plainPath(entries.next())) return false;
        QFile sourceCore(fixtureRoot + "/config/groups/coreType");
        if (!sourceCore.open(QIODevice::ReadOnly) || sourceCore.readAll().trimmed() != QByteArray::number(core)) return false;
        for (const auto &name: {"nekoray.json", "nekobox.json"}) {
            const auto settings = readObject(fixtureRoot + "/config/groups/" + name);
            if (settings.isEmpty() || settings.value("remember_enable").toBool(true) ||
                settings.value("remember_id").toInt() >= 0 ||
                !settings.value("spmode2").toArray().isEmpty() ||
                settings.value("sub_auto_update").toInt() > 0) return false;
            for (const auto &key: {"hk_mw", "hk_group", "hk_route", "hk_spmenu"}) {
                if (!settings.value(key).toString().isEmpty()) return false;
            }
        }
        for (const auto &name: QDir(fixtureRoot + "/config/groups").entryList({"*.json"}, QDir::Files)) {
            const auto group = readObject(fixtureRoot + "/config/groups/" + name);
            if (group.contains("id") && !group.value("skip_auto_update").toBool()) return false;
        }
        for (const auto &name: QDir(fixtureRoot + "/config/profiles").entryList({"*.json"}, QDir::Files)) {
            const auto node = readObject(fixtureRoot + "/config/profiles/" + name);
            if (node.value("bean").toObject().value("addr").toString() != "127.0.0.1") return false;
        }
        return true;
    }

    QJsonObject snapshot() {
        QJsonObject profiles;
        QJsonObject groups;
        QJsonArray groupOrder;
        for (const auto &[id, node]: NekoGui::profileManager->profiles) profiles.insert(QString::number(id), node->ToJson());
        for (const auto &[id, group]: NekoGui::profileManager->groups) groups.insert(QString::number(id), group->ToJson());
        for (int id: NekoGui::profileManager->groupsTabOrder) groupOrder.append(id);
        return {{"settings", NekoGui::dataStore->ToJson()}, {"settings_file", NekoGui::dataStore->fn},
                {"source_core", NekoGui::coreType}, {"effective_default_core", NekoGui::PreferredCore()},
                {"manager", QJsonObject{{"groups", groupOrder}}}, {"profiles", profiles}, {"groups", groups}};
    }
}

void PackageAcceptanceInitMainWindow();
#define main NekorayProductionMainForPackageAcceptance
#define UI_InitMainWindow PackageAcceptanceInitMainWindow
#include "../../main/main.cpp"
#undef UI_InitMainWindow
#undef main
#undef MW_INTERFACE

void PackageAcceptanceInitMainWindow() {
    UI_InitMainWindow();
    auto window = GetMainWindow();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, window, [] {
        exitedNormally = NekoGui::dataStore->prepare_exit && NekoGui::dataStore->started_id < 0 &&
                         !NekoGui::dataStore->spmode_system_proxy && !NekoGui::dataStore->spmode_vpn;
    });
    auto timer = new QTimer(window);
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    QObject::connect(timer, &QTimer::timeout, window, [window, timer, elapsed] {
        if (elapsed->elapsed() > 15000) {
            timer->stop();
            result.insert("error", "The packaged core did not become ready within 15 seconds");
            window->on_menu_exit_triggered();
            return;
        }
        if (!NekoGui::dataStore->core_running) return;
        timer->stop();
        if (NekoGui::dataStore->started_id >= 0 || NekoGui::dataStore->spmode_system_proxy ||
            NekoGui::dataStore->spmode_vpn) {
            result.insert("error", "The synthetic fixture unexpectedly started a connection or system mode");
            window->on_menu_exit_triggered();
            return;
        }
        result.insert("loaded", snapshot());
        if (action == "preferences") {
            NekoGui::dataStore->preferred_core = fixtureMarker.value("default_preference_after_edit").toInt();
            const auto preferences = fixtureMarker.value("node_preferences_after_edit").toObject();
            for (auto it = preferences.begin(); it != preferences.end(); ++it) {
                const auto node = NekoGui::profileManager->GetProfile(it.key().toInt());
                if (node == nullptr) {
                    result.insert("error", "A fixture node disappeared before saving its preference");
                    window->on_menu_exit_triggered();
                    return;
                }
                node->core_preference = it.value().toInt();
            }
        }
        // Force an actual production serialization of every fixture entity so
        // loading alone cannot conceal lost fields or duplicate legacy history.
        for (const auto &[id, node]: NekoGui::profileManager->profiles) node->Save();
        for (const auto &[id, group]: NekoGui::profileManager->groups) group->Save();
        NekoGui::dataStore->routing->Save();
        NekoGui::dataStore->Save();
        NekoGui::profileManager->SaveManager();
        result.insert("saved", snapshot());
        actionCompleted = true;
        window->on_menu_exit_triggered();
    });
    timer->start(25);
}

int main(int argc, char **argv) {
    int argumentCount = 0;
    auto wideArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (wideArguments == nullptr) return 2;
    QStringList arguments;
    for (int i = 0; i < argumentCount; ++i) arguments.append(QString::fromWCharArray(wideArguments[i]));
    LocalFree(wideArguments);
    const auto option = [&arguments](const QString &name) {
        const auto index = arguments.indexOf(name);
        return arguments.count(name) == 1 && index > 0 && index + 1 < arguments.size() ? arguments[index + 1] : QString{};
    };
    // Keep the native command line intact for Qt's Unicode argument handling.
    if (arguments.size() != 9 || !arguments.contains("-tray") || !arguments.contains("-many") ||
        option("-appdata").isEmpty()) return 2;
    fixtureRoot = QFileInfo(option("-appdata")).absoluteFilePath();
    action = option("--package-action");
    const auto reportName = option("--package-report");
    if ((action != "save" && action != "preferences") ||
        !QRegularExpression("^[a-zA-Z0-9_-]+[.]json$").match(reportName).hasMatch()) return 3;
    const auto executable = QFileInfo(arguments.first()).absoluteFilePath();
    if (!verifyFixture(executable)) return 4;
    reportPath = fixtureRoot + "/" + reportName;
    result = {{"fixture_run_id", fixtureMarker.value("run_id")}, {"action", action}};
    const int productionExit = NekorayProductionMainForPackageAcceptance(argc, argv);
    result.insert("action_completed", actionCompleted);
    result.insert("normal_exit", exitedNormally);
    result.insert("production_exit_code", productionExit);
    QSaveFile report(reportPath);
    if (!report.open(QIODevice::WriteOnly)) return 5;
    report.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    if (!report.commit()) return 6;
    return productionExit == 0 && actionCompleted && exitedNormally && !result.contains("error") ? 0 : 7;
}
