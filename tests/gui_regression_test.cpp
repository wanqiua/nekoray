#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#endif

#include <QtTest>
#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGroupBox>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <future>

#include "ui/mainwindow.h"
#include "ui/CoreRuntime.hpp"
#include "ui/SpeedtestBatch.hpp"
#include "ui/UrlImportDialog.hpp"
#include "ui/dialog_basic_settings.h"
#include "ui/edit/dialog_edit_group.h"
#include "db/Database.hpp"
#include "db/ConfigBuilder.hpp"
#include "fmt/includes.h"
#include "rpc/gRPC.h"
#include "sub/GroupUpdater.hpp"
#include "tests/subscription_metadata_test.hpp"
#include "tests/profile_editor_core_test.hpp"
#include "tests/hysteria2_editor_test.hpp"

extern std::shared_ptr<SpeedtestBatch> speedtest_batch;

struct ProbeCount {
    int active = 0;
    int peak = 0;
};

// A loopback HTTP endpoint, also accepting CONNECT as a fake proxy node. All
// fixtures run on Qt's event loop, while the actual GUI workers call the core.
class FixtureServer : public QTcpServer {
public:
    int requests = 0;
    QByteArray requestMarker;
    int matchingRequests = 0;
    int delay;
    QByteArray body;
    int status;

    FixtureServer(int delayMS = 0, int responseStatus = 204, QByteArray responseBody = {}, ProbeCount *count = nullptr)
        : delay(delayMS), body(std::move(responseBody)), status(responseStatus) {
        if (!listen(QHostAddress::LocalHost, 0)) qFatal("cannot listen on loopback");
        connect(this, &QTcpServer::newConnection, this, [this, count] {
            while (hasPendingConnections()) {
                auto socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, [socket, count] {
                    if (count != nullptr && socket->property("counted").toBool()) --count->active;
                    socket->deleteLater();
                });
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, count] {
                    *buffer += socket->readAll();
                    while (buffer->contains("\r\n\r\n")) {
                        const auto end = buffer->indexOf("\r\n\r\n") + 4;
                        const auto request = buffer->left(end);
                        buffer->remove(0, end);
                        if (request.startsWith("CONNECT ")) {
                            socket->write("HTTP/1.1 200 Connection Established\r\n\r\n");
                            continue;
                        }
                        ++requests;
                        if (!requestMarker.isEmpty() && request.toLower().contains(requestMarker.toLower())) ++matchingRequests;
                        if (count != nullptr) {
                            socket->setProperty("counted", true);
                            count->peak = qMax(count->peak, ++count->active);
                        }
                        if (delay < 0) return; // Stall until the caller cancels.
                        const QPointer<QTcpSocket> guarded(socket);
                        QTimer::singleShot(delay, socket, [this, guarded, count] {
                            if (!guarded || guarded->state() != QAbstractSocket::ConnectedState) return;
                            guarded->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Length: " +
                                           QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                            // The request has a response now; a later TCP-close
                            // event must not inflate the active request count.
                            if (count != nullptr && guarded->property("counted").toBool()) {
                                guarded->setProperty("counted", false);
                                --count->active;
                            }
                            guarded->disconnectFromHost();
                        });
                    }
                });
            }
        });
    }

    QString url() const { return QString("http://127.0.0.1:%1/subscription").arg(serverPort()); }
};

class ModalGuard : public QTimer {
public:
    ModalGuard() {
        connect(this, &QTimer::timeout, [] { qFatal("modal interaction did not complete"); });
        start(5000);
    }
};

template<typename T>
T *child(QObject *parent, const char *name) {
    auto value = parent->findChild<T *>(name);
    if (value == nullptr) qFatal("missing UI control: %s", name);
    return value;
}

class GuiRegressionTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory;
    QString previousDirectory;
    MainWindow *window = nullptr;
    int baseGroup = -1;

    std::shared_ptr<NekoGui::Group> addGroup(const QString &name) {
        auto group = NekoGui::ProfileManager::NewGroup();
        group->name = name;
        NekoGui::profileManager->AddGroup(group);
        window->refresh_groups();
        return group;
    }

    std::shared_ptr<NekoGui::ProxyEntity> addNode(int port, int gid = -1) {
        auto profile = NekoGui::ProfileManager::NewProxyEntity("http");
        profile->bean->name = QString("node-%1").arg(port);
        profile->bean->serverAddress = "127.0.0.1";
        profile->bean->serverPort = port;
        NekoGui::profileManager->AddProfile(profile, gid < 0 ? baseGroup : gid);
        return profile;
    }

    void chooseURL(const char *button, const std::function<void(DialogEditGroup *)> &edit = {}) {
        QTimer::singleShot(0, window, [=] {
            auto choices = child<QDialog>(window, "urlImportDialog");
            if (edit) {
                QTimer::singleShot(0, window, [=] { edit(child<DialogEditGroup>(window, "DialogEditGroup")); });
            }
            child<QPushButton>(choices, button)->click();
        });
    }

    void contextMenu(int groupId, const std::function<void(QMenu *)> &action) {
        auto tabs = child<QTabWidget>(window, "tabWidget")->tabBar();
        int index = -1;
        for (int n = 0; n < tabs->count(); ++n) {
            if (tabs->tabData(n).toInt() == groupId) index = n;
        }
        if (index < 0) qFatal("missing group tab");
        ModalGuard guard;
        QTimer::singleShot(0, window, [=] { action(child<QMenu>(window, "groupContextMenu")); });
        QMetaObject::invokeMethod(window, "show_group_context_menu", Qt::DirectConnection,
                                  Q_ARG(QPoint, tabs->tabRect(index).center()));
    }

    void startBatch() {
        child<QMenu>(window, "menu_server")->setProperty("selected_or_group", 0);
        window->show_group(baseGroup);
        child<QAction>(window, "menu_url_test")->trigger();
    }

    bool stopActiveRuntime() {
        if (speedtest_batch) speedtest_batch->cancel();
        if (!QTest::qWaitFor([] { return !speedtest_batch; }, 10000)) return false;
        if (window == nullptr) return true;
        if (window->core_session || window->core_switch_busy || window->pending_core_intent ||
            window->active_core_intent || NekoGui::dataStore->started_id >= 0) {
            window->neko_stop();
        }
        return QTest::qWaitFor([this] {
            return !window->core_session && !window->core_switch_busy && !window->pending_core_intent &&
                   !window->active_core_intent && NekoGui::dataStore->started_id < 0;
        }, 15000);
    }

private slots:
    void initTestCase() {
        directory = std::make_unique<QTemporaryDir>();
        QVERIFY(directory->isValid());
        previousDirectory = QDir::currentPath();
        QVERIFY(QDir::setCurrent(directory->path()));
        for (const auto &name: {"groups", "profiles", "routes", "routes_box"}) QVERIFY(QDir().mkpath(name));
        NekoGui::coreType = qEnvironmentVariable("NEKORAY_TEST_CORE") == "box" ? NekoGui::CoreType::SING_BOX : NekoGui::CoreType::V2RAY;
        auto data = NekoGui::dataStore;
        data->fn = "groups/test-settings.json";
        data->routing = std::make_unique<NekoGui::Routing>();
        data->flag_tray = true;
        data->remember_enable = false;
        data->sub_use_proxy = false;
        data->sub_auto_update = 0;
        data->traffic_loop_interval = 0;
        data->test_concurrent = 4;
        data->test_latency_timeout = 1000;
        data->test_latency_url = "http://127.0.0.1:1/generate_204";
        data->inbound_address = "127.0.0.1";
        data->inbound_socks_port = MkPort();
        do { data->inbound_http_port = MkPort(); } while (data->inbound_http_port == data->inbound_socks_port);
        QVERIFY(data->inbound_socks_port > 0 && data->inbound_http_port > 0);
        data->log_level = IS_NEKO_BOX ? "warn" : "warning";
        data->mux_protocol = IS_NEKO_BOX ? "h2mux" : "";
        data->extraCore->core_map = "{}";
        DS_cores = new QThread;
        DS_cores->start();
        MF_release_runguard = [] {};
        window = new MainWindow;
        window->show();
        baseGroup = NekoGui::dataStore->current_group;
        QTRY_VERIFY_WITH_TIMEOUT(NekoGui::dataStore->core_running && NekoGui_rpc::defaultClient != nullptr, 10000);
    }

    void cleanup() {
        const bool systemAcceptance = qEnvironmentVariable("NEKORAY_SYSTEM_ACCEPTANCE") == "1";
        if (systemAcceptance) qInfo("System acceptance cleanup: stopping the current node");
        QVERIFY2(stopActiveRuntime(), "test runtime did not stop before removing fixture profiles");
        if (systemAcceptance) qInfo("System acceptance cleanup: removing synthetic profiles");
        window->show_group(baseGroup);
        const auto groups = NekoGui::profileManager->groupsTabOrder;
        for (int id: groups) {
            if (id != baseGroup) NekoGui::profileManager->DeleteGroup(id);
        }
        const auto profiles = NekoGui::profileManager->GetGroup(baseGroup)->Profiles();
        for (const auto &profile: profiles) NekoGui::profileManager->DeleteProfile(profile->id);
        window->refresh_groups();
        NekoGui::dataStore->Save();
        if (systemAcceptance) qInfo("System acceptance cleanup: complete");
    }

    void testResultProvenance() {
        SubscriptionMetadataRegression::TestResultProvenance();
    }

    void legacyResultsAreHistory() {
        SubscriptionMetadataRegression::LegacyResultsAreHistory();
    }

    void subscriptionRenameReorderAndClear() {
        SubscriptionMetadataRegression::SubscriptionRenameReorderAndClear();
    }

    void failedSubscriptionKeepsExistingGroup() {
        SubscriptionMetadataRegression::FailedSubscriptionKeepsExistingGroup();
    }

    void subscriptionDuplicateIdentityIsNotGuessed() {
        SubscriptionMetadataRegression::SubscriptionDuplicateIdentityIsNotGuessed();
    }

    void subscriptionIdentityIgnoresNameButNotConfigurationOrGroup() {
        SubscriptionMetadataRegression::SubscriptionIdentityIgnoresNameButNotConfigurationOrGroup();
    }

    void subscriptionKeepsActiveNodeVisible() {
        SubscriptionMetadataRegression::SubscriptionKeepsActiveNodeVisible();
    }

    void editorPreferenceIsDraftOnly() {
        ProfileEditorCoreRegression::EditorPreferenceIsDraftOnly(window);
    }

    void newNodeKeepsPreferenceAcrossTypeChanges() {
        ProfileEditorCoreRegression::NewNodeKeepsPreferenceAcrossTypeChanges(window);
    }

    void editorExposesBothCoreOptions() {
        ProfileEditorCoreRegression::EditorExposesBothCoreOptions(window);
    }

    void certificatePinEditorRoundTrip() {
        using ProfileEditorCoreRegression::Control;
        SubscriptionMetadataRegression::GroupFixture fixture;
        auto node = NekoGui::ProfileManager::NewProxyEntity("vless");
        node->bean->serverAddress = "example.invalid";
        node->bean->serverPort = 443;
        node->TrojanVLESSBean()->password = "75f2a015-3611-4c7f-bf7c-a948276b179f";
        node->TrojanVLESSBean()->stream->security = "tls";
        QVERIFY(NekoGui::profileManager->AddProfile(node, fixture.group->id));
        {
            DialogEditProfile editor("", node->id, window);
            Control<QLineEdit>(&editor, "certificate_pin")->setText(QString("AA:").repeated(31) + "AA");
            QVERIFY(Control<QRadioButton>(&editor, "edit_core_xray")->isEnabled());
            QVERIFY(!Control<QRadioButton>(&editor, "edit_core_singbox")->isEnabled());
            QVERIFY(node->TrojanVLESSBean()->stream->certificate_pin.isEmpty());
            const auto screenshotDirectory = qEnvironmentVariable("NEKORAY_TEST_SCREENSHOT_DIR");
            if (!screenshotDirectory.isEmpty()) {
                editor.show();
                QTest::qWait(50);
                QVERIFY(QDir().mkpath(screenshotDirectory));
                QVERIFY(editor.grab().save(screenshotDirectory + "/certificate-pin-editor.png"));
            }
            editor.accept();
            QCOMPARE(editor.result(), int(QDialog::Accepted));
        }
        QCOMPARE(node->TrojanVLESSBean()->stream->certificate_pin, QString(64, 'a'));
        {
            DialogEditProfile editor("", node->id, window);
            QCOMPARE(Control<QLineEdit>(&editor, "certificate_pin")->text(), QString(64, 'a'));
            Control<QLineEdit>(&editor, "certificate_pin")->clear();
            QVERIFY(Control<QRadioButton>(&editor, "edit_core_singbox")->isEnabled());
            editor.reject();
        }
        QCOMPARE(node->TrojanVLESSBean()->stream->certificate_pin, QString(64, 'a'));
    }

    void groupTypeAndDraftCancellation() {
        auto draft = NekoGui::ProfileManager::NewGroup();
        DialogEditGroup dialog(draft, window);
        dialog.show();
        auto basic = child<QRadioButton>(&dialog, "type_basic");
        auto subscription = child<QRadioButton>(&dialog, "type_subscription");
        QVERIFY(basic->isChecked());
        QVERIFY(child<QGroupBox>(&dialog, "cat_sub")->isHidden());
        subscription->click();
        QVERIFY(!basic->isChecked());
        QVERIFY(!child<QGroupBox>(&dialog, "cat_sub")->isHidden());
        child<QLineEdit>(&dialog, "url")->setText("https://example.invalid/sub");
        basic->click();
        child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Ok)->click();
        QVERIFY(draft->url.isEmpty());
        QCOMPARE(draft->id, -1);
    }

    void cancelledNewSubscriptionCreatesNothing() {
        FixtureServer server;
        const auto before = NekoGui::profileManager->groups.size();
        ModalGuard guard;
        bool inspected = false;
        chooseURL("importSubscriptionNew", [&](DialogEditGroup *editor) {
            QCOMPARE(child<QLineEdit>(editor, "url")->text(), server.url());
            QCOMPARE(child<QLineEdit>(editor, "name")->text(), QString("127.0.0.1"));
            QVERIFY(child<QRadioButton>(editor, "type_subscription")->isChecked());
            QCOMPARE(NekoGui::profileManager->groups.size(), before);
            inspected = true;
            editor->reject();
        });
        NekoGui_sub::groupUpdater->AsyncUpdate(server.url());
        QVERIFY(inspected);
        QCOMPARE(NekoGui::profileManager->groups.size(), before);
        QTest::qWait(50);
        QCOMPARE(server.requests, 0);
    }

    void newSubscriptionEditsSelectsAndUpdates() {
        FixtureServer original;
        FixtureServer server(0, 200, "socks://127.0.0.1:1080#subscription-node\n");
        QSignalSpy finished(NekoGui_sub::groupUpdater, &NekoGui_sub::GroupUpdater::asyncUpdateCallback);
        ModalGuard guard;
        chooseURL("importSubscriptionNew", [&](DialogEditGroup *editor) {
            child<QLineEdit>(editor, "name")->setText("Edited subscription");
            child<QLineEdit>(editor, "url")->setText(server.url());
            child<QDialogButtonBox>(editor, "buttonBox")->button(QDialogButtonBox::Ok)->click();
        });
        NekoGui_sub::groupUpdater->AsyncUpdate(original.url());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        auto group = NekoGui::profileManager->CurrentGroup();
        QCOMPARE(group->name, QString("Edited subscription"));
        QCOMPARE(group->url, server.url());
        QCOMPARE(group->Profiles().size(), 1);
        QCOMPARE(finished.at(0).at(0).toInt(), group->id);
        QCOMPARE(original.requests, 0);
    }

    void appendStaysInOriginalGroupAfterTabSwitch() {
        auto other = addGroup("Other");
        auto existing = addNode(1080);
        FixtureServer server(100, 200, "socks://127.0.0.1:2080#appended-node\n");
        QSignalSpy finished(NekoGui_sub::groupUpdater, &NekoGui_sub::GroupUpdater::asyncUpdateCallback);
        ModalGuard guard;
        chooseURL("importSubscriptionCurrent");
        NekoGui_sub::groupUpdater->AsyncUpdate(server.url());
        window->show_group(other->id);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QCOMPARE(NekoGui::profileManager->GetGroup(baseGroup)->Profiles().size(), 2);
        QVERIFY(NekoGui::profileManager->GetProfile(existing->id) == existing);
        QVERIFY(other->Profiles().isEmpty());
        QVERIFY(NekoGui::profileManager->GetGroup(baseGroup)->url.isEmpty());
    }

    void asLinkDoesNotDownload() {
        FixtureServer server;
        QSignalSpy finished(NekoGui_sub::groupUpdater, &NekoGui_sub::GroupUpdater::asyncUpdateCallback);
        ModalGuard guard;
        chooseURL("importLink");
        NekoGui_sub::groupUpdater->AsyncUpdate(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QCOMPARE(server.requests, 0);
        QCOMPARE(NekoGui::profileManager->CurrentGroup()->Profiles().size(), 1);
    }

    void groupMenuEditsClickedTabAndProtectsLastGroup() {
        contextMenu(baseGroup, [&](QMenu *menu) {
            QVERIFY(!child<QAction>(menu, "deleteGroup")->isEnabled());
            QVERIFY(!child<QAction>(menu, "updateGroupSubscription")->isEnabled());
            menu->close();
        });
        auto other = addGroup("Unselected group");
        contextMenu(other->id, [&](QMenu *menu) {
            QVERIFY(child<QAction>(menu, "deleteGroup")->isEnabled());
            QTimer::singleShot(0, window, [&] {
                auto editor = child<DialogEditGroup>(window, "DialogEditGroup");
                QCOMPARE(child<QLineEdit>(editor, "name")->text(), QString("Unselected group"));
                QVERIFY(!child<QRadioButton>(editor, "type_basic")->isEnabled());
                child<QLineEdit>(editor, "name")->setText("Changed via tab menu");
                child<QDialogButtonBox>(editor, "buttonBox")->button(QDialogButtonBox::Ok)->click();
            });
            auto edit = child<QAction>(menu, "editGroup");
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(edit).center());
        });
        QCOMPARE(other->name, QString("Changed via tab menu"));
        QCOMPARE(NekoGui::dataStore->current_group, baseGroup);

        FixtureServer server(0, 200, "socks://127.0.0.1:1080#menu-update\n");
        other->url = server.url();
        QSignalSpy finished(NekoGui_sub::groupUpdater, &NekoGui_sub::GroupUpdater::asyncUpdateCallback);
        contextMenu(other->id, [&](QMenu *menu) {
            auto update = child<QAction>(menu, "updateGroupSubscription");
            QVERIFY(update->isEnabled());
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(update).center());
        });
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QCOMPARE(other->Profiles().size(), 1);
        QVERIFY(NekoGui::profileManager->GetGroup(baseGroup)->Profiles().isEmpty());
        for (bool confirmed: {false, true}) {
            contextMenu(other->id, [&](QMenu *menu) {
                QTimer::singleShot(0, window, [=] {
                    auto confirmation = window->findChild<QMessageBox *>();
                    if (!confirmation) qFatal("missing group removal confirmation");
                    confirmation->button(confirmed ? QMessageBox::Yes : QMessageBox::No)->click();
                });
                auto remove = child<QAction>(menu, "deleteGroup");
                QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(remove).center());
            });
            QCOMPARE(NekoGui::profileManager->GetGroup(other->id) == nullptr, confirmed);
        }
        QCOMPARE(NekoGui::dataStore->current_group, baseGroup);
    }

    void configBuildDoesNotRewriteNodeOrTrafficState() {
        auto profile = NekoGui::ProfileManager::NewProxyEntity("vless");
        profile->bean->serverAddress = "127.0.0.1";
        profile->bean->serverPort = 443;
        profile->TrojanVLESSBean()->password = "91c19358-b5da-49d6-a1d3-cc47973baed0";
        profile->TrojanVLESSBean()->flow = "xtls-rprx-vision";
        profile->TrojanVLESSBean()->stream->network = "tcp";
        profile->TrojanVLESSBean()->stream->security = "tls";
        profile->TrojanVLESSBean()->stream->sni = "example.invalid";
        NekoGui::profileManager->AddProfile(profile, baseGroup);
        profile->traffic_data->tag = "running-proxy";
        const auto before = profile->bean->ToJsonBytes();
        auto config = NekoGui::BuildConfig(profile, true, false);
        QVERIFY2(config->error.isEmpty(), qPrintable(config->error));
        QCOMPARE(profile->bean->ToJsonBytes(), before);
        QCOMPARE(profile->traffic_data->tag, std::string("running-proxy"));
        QVERIFY(config->outboundStats.isEmpty());
    }

    void defaultCorePreferenceOnlySavesForNextConnection_data() {
        QTest::addColumn<QString>("externalCoreMap");
        QTest::addColumn<QString>("muxProtocol");
        QTest::addColumn<bool>("changeInbound");
        QTest::newRow("empty-map-and-default-mux") << QString("{}") << QString() << false;
        QTest::newRow("blank-map-and-preserved-mux") << QString() << QString("smux") << false;
        QTest::newRow("formatted-map") << QString("{\n  \"custom-helper\": \"C:/fixture/helper.exe\"\n}")
                                        << QString("yamux") << false;
        QTest::newRow("changed-inbound-still-prompts") << QString("{}") << QString("smux") << true;
    }

    void defaultCorePreferenceOnlySavesForNextConnection() {
        QFETCH(QString, externalCoreMap);
        QFETCH(QString, muxProtocol);
        QFETCH(bool, changeInbound);
        auto data = NekoGui::dataStore;
        QScopedValueRollback<int> restorePreference(data->preferred_core);
        QScopedValueRollback<QString> restoreExternalCores(data->extraCore->core_map, externalCoreMap);
        QScopedValueRollback<QString> restoreMuxProtocol(data->mux_protocol, muxProtocol);
        QScopedValueRollback<QString> restoreInbound(data->inbound_address);
        const int originalCore = NekoGui::coreType;
        const int nextCore = NekoGui::PreferredCore() == NekoGui::CoreType::V2RAY
                                 ? NekoGui::CoreType::SING_BOX : NekoGui::CoreType::V2RAY;
        const auto sourceFile = data->fn;
        const auto priorSettings = data->ToJson({"preferred_core"});
        auto expectedSettings = priorSettings;
        data->Save();
        FixtureServer server;
        auto profile = addNode(server.serverPort());
        window->neko_start(profile->id);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy && window->core_session &&
                                window->core_session->profile == profile, 15000);
        const auto activeSession = window->core_session;
        const auto xrayGeneration = window->core_manager->Generation(NekoGui::CoreType::V2RAY);
        const auto boxGeneration = window->core_manager->Generation(NekoGui::CoreType::SING_BOX);
        const auto intentSequence = window->core_intent_sequence.load();
        QStringList shownPrompts;
        QTimer dismissPrompt;
        connect(&dismissPrompt, &QTimer::timeout, window, [&] {
            for (auto widget: QApplication::topLevelWidgets()) {
                auto message = qobject_cast<QMessageBox *>(widget);
                if (message == nullptr || !message->isVisible()) continue;
                shownPrompts.append(message->text());
                message->done(QMessageBox::No);
            }
        });
        dismissPrompt.start(10);
        DialogBasicSettings dialog(window);
        dialog.show();
        child<QRadioButton>(&dialog, nextCore == NekoGui::CoreType::SING_BOX
                                          ? "switch_core_sing_box" : "switch_core_v2ray")->click();
        if (changeInbound) {
            child<QLineEdit>(&dialog, "inbound_address")->setText("127.0.0.2");
            expectedSettings.insert("inbound_address", "127.0.0.2");
        }
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QTRY_COMPARE_WITH_TIMEOUT(QJsonDocument::fromJson(ReadFile(data->fn)).object()
                                      .value("preferred_core").toInt(-1), nextCore, 3000);
        const auto savedSettings = data->ToJson({"preferred_core"});
        QJsonObject unexpectedChanges;
        for (auto it = expectedSettings.begin(); it != expectedSettings.end(); ++it) {
            if (savedSettings.value(it.key()) != it.value()) {
                unexpectedChanges.insert(it.key(), QJsonObject{{"expected", it.value()},
                                                               {"actual", savedSettings.value(it.key())}});
            }
        }
        QVERIFY2(unexpectedChanges.isEmpty(), qPrintable(QJsonDocument(unexpectedChanges).toJson(QJsonDocument::Compact)));
        QCOMPARE(shownPrompts.size(), changeInbound ? 1 : 0);
        if (changeInbound) {
            QCOMPARE(shownPrompts.first(), QCoreApplication::translate("MainWindow", "Settings changed, restart proxy?"));
        }
        QCOMPARE(NekoGui::PreferredCore(), nextCore);
        QCOMPARE(NekoGui::coreType, originalCore);
        QCOMPARE(data->fn, sourceFile);
        QCOMPARE(savedSettings, expectedSettings);
        QCOMPARE(data->started_id, profile->id);
        QVERIFY(window->core_session == activeSession);
        QVERIFY(!window->core_switch_busy && !window->pending_core_intent);
        QCOMPARE(window->core_intent_sequence.load(), intentSequence);
        QCOMPARE(window->core_manager->Generation(NekoGui::CoreType::V2RAY), xrayGeneration);
        QCOMPARE(window->core_manager->Generation(NekoGui::CoreType::SING_BOX), boxGeneration);
    }

    void tcpUrlAndFullResultsKeepSeparateProvenance() {
        using namespace NekoGui;
        auto profile = addNode(1080);
        const int core = PreferredCore();
        profile->core_preference = core + 1;
        const auto identity = window->core_manager->Identity(core);
        QVERIFY(!identity.isEmpty());
        auto context = CaptureBuildConfigContext(core, true);
        context.tunProxyPort = window->tun_proxy_port;
        const auto config = BuildConfig(profile, true, false, context);
        QVERIFY2(config->error.isEmpty(), qPrintable(config->error));
        const auto record = [&](int mode, TestResultStatus status, int latency) {
            ProfileTestResult result;
            result.core = core;
            result.mode = mode == libcore::TcpPing ? "tcp" : mode == libcore::FullTest ? "full" : "url";
            result.build_id = identity;
            result.config_fingerprint = window->test_fingerprint(profile, core, mode);
            result.generated_config_digest = mode == libcore::TcpPing
                                                  ? CoreConfigDigest({{"address", profile->bean->DisplayAddress()}})
                                                  : QString::fromLatin1(config->Fingerprint());
            result.status = status;
            result.latency_ms = latency;
            profile->RecordTestResult(result);
            window->refresh_test_validity(profile);
        };
        const auto urlFingerprint = window->test_fingerprint(profile, core, libcore::UrlTest);
        QVERIFY(urlFingerprint != window->test_fingerprint(profile, core, libcore::TcpPing));
        QVERIFY(urlFingerprint != window->test_fingerprint(profile, core, libcore::FullTest));

        record(libcore::TcpPing, TestResultStatus::Success, 5);
        record(libcore::FullTest, TestResultStatus::ConnectionFailure, 0);
        QCOMPARE(window->current_test_result(profile).status, TestResultStatus::Untested);
        QCOMPARE(profile->latency, 0);
        QVERIFY(!profile->IsRemovalCandidate(core, identity, urlFingerprint));

        record(libcore::UrlTest, TestResultStatus::Success, 37);
        record(libcore::TcpPing, TestResultStatus::ConnectionFailure, 0);
        record(libcore::FullTest, TestResultStatus::Success, 88);
        QCOMPARE(window->current_test_result(profile).mode, QString("url"));
        QCOMPARE(window->current_test_result(profile).latency_ms, 37);
        QCOMPARE(profile->latency, 37);
        QCOMPARE(profile->LastTestResult(core, "next", "tcp").status, TestResultStatus::ConnectionFailure);
        QCOMPARE(profile->LastTestResult(core, "next", "full").latency_ms, 88);
        QVERIFY(!profile->IsRemovalCandidate(core, identity, urlFingerprint));

        auto currentFailure = profile->LastTestResult(core);
        currentFailure.scope = "current";
        currentFailure.status = TestResultStatus::ConnectionFailure;
        profile->RecordTestResult(currentFailure);
        window->refresh_test_validity(profile);
        QCOMPARE(profile->latency, 37);
        QVERIFY(!profile->IsRemovalCandidate(core, identity, urlFingerprint));

        auto restored = ProfileManager::NewProxyEntity("http");
        restored->FromJson(profile->ToJson());
        QCOMPARE(restored->LastTestResult(core).latency_ms, 37);
        QCOMPARE(restored->LastTestResult(core, "next", "tcp").status, TestResultStatus::ConnectionFailure);
        QCOMPARE(restored->LastTestResult(core, "next", "full").latency_ms, 88);
        QVERIFY(restored->HasValidTestResult(core, identity, urlFingerprint));

        record(libcore::UrlTest, TestResultStatus::ConnectionFailure, 0);
        record(libcore::TcpPing, TestResultStatus::Success, 4);
        record(libcore::FullTest, TestResultStatus::Success, 91);
        QCOMPARE(profile->latency, -1);
        QVERIFY(profile->IsRemovalCandidate(core, identity, urlFingerprint));
    }

    void realBatchMatchesSingleNodeProbesAndRunsConcurrently() {
        ProbeCount count;
        std::vector<std::unique_ptr<FixtureServer>> servers;
        QList<std::shared_ptr<NekoGui::ProxyEntity>> profiles;
        QList<bool> expected;
        for (int n = 0; n < 8; ++n) {
            const int kind = n % 4;
            servers.push_back(std::make_unique<FixtureServer>(kind == 3 ? -1 : 80 + 40 * kind,
                                                             kind == 2 ? 503 : 204, QByteArray(), &count));
            profiles += addNode(servers.back()->serverPort());
            expected += kind < 2;
        }
        for (int n = 0; n < profiles.size(); ++n) {
            auto config = NekoGui::BuildConfig(profiles[n], true, false);
            QVERIFY2(config->error.isEmpty(), qPrintable(config->error));
            libcore::TestReq request;
            request.set_mode(libcore::UrlTest);
            request.set_timeout(NekoGui::dataStore->test_latency_timeout);
            request.set_url(NekoGui::dataStore->test_latency_url.toStdString());
            request.mutable_config()->set_core_config(QJsonObject2QString(config->coreConfig, true).toStdString());
            auto probe = std::async(std::launch::async, [request] {
                bool ok;
                auto result = NekoGui_rpc::defaultClient->Test(&ok, request);
                return std::make_pair(ok, result);
            });
            QTRY_VERIFY_WITH_TIMEOUT(probe.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
            auto result = probe.get();
            QVERIFY(result.first);
            QCOMPARE(result.second.error().empty() && result.second.ms() > 0, expected[n]);
        }
        QTRY_COMPARE_WITH_TIMEOUT(count.active, 0, 2000);
        count.peak = 0;
        startBatch();
        QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch, 10000);
        for (int n = 0; n < profiles.size(); ++n) {
            QCOMPARE(profiles[n]->latency > 0, expected[n]);
            QVERIFY(servers[n]->requests >= 2);
        }
        QVERIFY2(count.peak >= 3, "GUI batch requests were serialized");
        qInfo("Real GUI/core batch peak concurrency: %d", count.peak);
    }

    void cancellationStopsInflightRequestsAndKeepsOldResults() {
        ProbeCount count;
        FixtureServer server(-1, 204, {}, &count);
        QList<std::shared_ptr<NekoGui::ProxyEntity>> profiles;
        for (int n = 0; n < 12; ++n) profiles += addNode(server.serverPort());
        startBatch();
        QTRY_VERIFY_WITH_TIMEOUT(count.active >= 3, 4000);
        child<QAction>(window, "menu_stop_testing")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(count.active, 0, 3000);
        for (const auto &profile: profiles) QCOMPARE(profile->latency, 0);
        QVERIFY(server.requests < profiles.size());
    }

    void changedAndReplacedNodesIgnoreOldResults() {
        FixtureServer server(350);
        auto profile = addNode(server.serverPort());
        startBatch();
        QTRY_VERIFY_WITH_TIMEOUT(server.requests >= 1, 3000);
        profile->bean->name = "edited during probe";
        profile->Save();
        QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch, 3000);
        QCOMPARE(profile->latency, 0);

        startBatch();
        QTRY_VERIFY_WITH_TIMEOUT(server.requests >= 2, 3000);
        const int oldId = profile->id;
        NekoGui::profileManager->DeleteProfile(oldId);
        auto replacement = addNode(server.serverPort());
        QCOMPARE(replacement->id, oldId);
        window->refresh_proxy_list();
        QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch, 3000);
        QCOMPARE(replacement->latency, 0);

        startBatch();
        QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch, 3000);
        QVERIFY(replacement->latency > 0);
    }

    void writeOptionalPreviews() {
        const auto output = qEnvironmentVariable("NEKORAY_TEST_SCREENSHOT_DIR");
        if (output.isEmpty()) return;
        QVERIFY(QDir().mkpath(output));
        QTranslator translator;
        QVERIFY(translator.load(":/translations/zh_CN.qm"));
        QCoreApplication::installTranslator(&translator);
        UrlImportDialog choices("https://example.invalid/subscription", window);
        choices.show();
        QTest::qWait(20);
        QVERIFY(choices.grab().save(output + "/url-import.png"));
        choices.hide();
        auto draft = NekoGui::ProfileManager::NewGroup();
        draft->name = QString::fromUtf8("新订阅组");
        draft->url = "https://example.invalid/subscription";
        DialogEditGroup editor(draft, window);
        editor.show();
        QTest::qWait(20);
        QVERIFY(editor.grab().save(output + "/group-editor.png"));
        editor.hide();
        contextMenu(baseGroup, [&](QMenu *menu) {
            QVERIFY(menu->grab().save(output + "/group-menu.png"));
            menu->close();
        });
        QCoreApplication::removeTranslator(&translator);
    }

    void manualCoreSwitchAndSameConfigurationNoop();
    void bulkPreferenceKeepsConnectionUntilExplicitClick();
    void mixedCoreBatchKeepsOneBudgetAndSurvivesSwitch();
    void targetStartFailureRestoresSavedConfiguration();
    void invalidTargetPreflightKeepsCurrentConnection();
    void latestSelectionWinsAndDeletedPreparationCannotStart();
    void ownedCoreCrashRestoresSavedSession();
    void exitPreparationRejectsNewTests();
    void socksUdpFixtureSanity();
    void systemProxyRuntimeAcceptance();
    void tunSystemRuntimeAcceptance();
    void newProtocolsImportNativeAndFullLinks();
    void newProtocolsRejectInvalidSubscription();
    void newProtocolsResolveCoreAndEditorChoices();
    void anyTlsEditorDraftAndAdvancedSave();
    void snellEditorVersionDraftsAndSave();
    void newProtocolsSubscriptionKeepsMetadata();
    void anyTlsResolveDomainPreservesSni();
    void snellUrlBatchRespectsNetwork();
    void newProtocolsClashImportPreservesOptions();
    void newProtocolPeerRuntimeAcceptance();
    void removeUnavailableUsesValidResultsAndReportsEmpty();
    void coreSelectionKeepsOneRow();
    void subscriptionAliasesAndCertificatePins();
    void subscriptionUserAgentDoesNotDependOnCore();
    void optionalSubscriptionImportFixture();

    void hysteria2AdvancedRoundTrip() {
        Hysteria2EditorRegression::AdvancedRoundTrip(window);
    }
    void hysteria2CancelAndLegacyEditors() {
        Hysteria2EditorRegression::CancelAndLegacyEditors(window);
    }
    void hysteria2InvalidAdvancedOptionsCannotSave() {
        Hysteria2EditorRegression::InvalidAdvancedOptionsCannotSave(window);
    }

    void cleanupTestCase() {
        QVERIFY2(stopActiveRuntime(), "test runtime did not stop before daemon shutdown");
        NekoGui::dataStore->prepare_exit = true;
        if (window != nullptr && window->core_manager != nullptr) {
            const auto xray = window->core_manager->Get(NekoGui::CoreType::V2RAY);
            const auto box = window->core_manager->Get(NekoGui::CoreType::SING_BOX);
            auto shutdown = std::async(std::launch::async, [this] { window->stop_core_daemon(); });
            QTRY_VERIFY_WITH_TIMEOUT(shutdown.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
            shutdown.get();
            QVERIFY(!xray->alive.load() && !xray->ready.load());
            QVERIFY(!box->alive.load() && !box->ready.load());
            QTRY_VERIFY_WITH_TIMEOUT(!NekoGui::dataStore->core_running, 3000);
        }
        if (DS_cores != nullptr) {
            DS_cores->quit();
            QVERIFY(DS_cores->wait(3000));
        }
        QVERIFY(QDir::setCurrent(previousDirectory));
    }
};

#include "dynamic_core_runtime_test.hpp"
#include "system_mode_acceptance_test.hpp"
#include "new_protocol_regression_test.hpp"
#include "protocol_gui_runtime_test.hpp"
#include "release_0_1_regression_test.hpp"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
#ifdef Q_OS_WIN
    // The offscreen platform does not discover Windows' system font directory.
    QFontDatabase::addApplicationFont(qEnvironmentVariable("SystemRoot") + "/Fonts/msyh.ttc");
    QApplication::setFont(QFont("Microsoft YaHei", 9));
#endif
    GuiRegressionTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "gui_regression_test.moc"
