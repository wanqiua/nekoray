#pragma once

#include <QScopedValueRollback>
#include "ui/edit/dialog_edit_profile.h"

void GuiRegressionTest::removeUnavailableUsesValidResultsAndReportsEmpty() {
    using namespace NekoGui;
    const int core = PreferredCore();
    const auto record = [&](const std::shared_ptr<ProxyEntity> &node, const QString &mode, TestResultStatus status,
                            const QString &scope = "next") {
        node->core_preference = core + 1;
        ProfileTestResult result;
        result.core = core;
        result.mode = mode;
        result.scope = scope;
        result.status = status;
        result.build_id = window->core_manager->Identity(core);
        result.config_fingerprint = window->test_fingerprint(node, core, mode == "tcp" ? 0 : mode == "full" ? 2 : 1);
        result.generated_config_digest = CoreConfigDigest({{"fixture", "removal"}});
        result.latency_ms = status == TestResultStatus::Success ? 10 : 0;
        node->RecordTestResult(result);
    };
    QList<std::shared_ptr<ProxyEntity>> failures;
    for (const auto &mode : {QString("url"), QString("tcp"), QString("full")}) {
        auto node = addNode(10100 + failures.size());
        record(node, mode, TestResultStatus::ConnectionFailure);
        failures.append(node);
    }
    const auto working = addNode(10110);
    record(working, "url", TestResultStatus::Success);
    record(working, "tcp", TestResultStatus::ConnectionFailure);
    const auto stale = addNode(10111);
    record(stale, "url", TestResultStatus::ConnectionFailure);
    ++stale->bean->serverPort;
    const auto coreError = addNode(10113);
    record(coreError, "url", TestResultStatus::CoreError);
    const auto untested = addNode(10114);
    const auto running = addNode(10115);
    record(running, "url", TestResultStatus::ConnectionFailure);
    const auto current = addNode(10116);
    record(current, "url", TestResultStatus::ConnectionFailure, "current");
    SubscriptionMetadataRegression::GroupFixture otherGroup;
    const auto other = otherGroup.add(10117, "other group");
    record(other, "url", TestResultStatus::ConnectionFailure);
    window->show_group(baseGroup);
    window->refresh_proxy_list();
    QScopedValueRollback<int> active(dataStore->started_id, running->id);
    ModalGuard guard;
    bool confirmed = false;
    QTimer::singleShot(0, window, [&] {
        auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(box);
        QCOMPARE(box->icon(), QMessageBox::Question);
        QVERIFY(box->text().contains("3"));
        confirmed = true;
        box->button(QMessageBox::Yes)->click();
    });
    child<QAction>(window, "menu_remove_unavailable")->trigger();
    QVERIFY(confirmed);
    for (const auto &node : failures) QVERIFY(!profileManager->GetProfile(node->id));
    for (const auto &node : {working, stale, coreError, untested, running, current, other}) {
        QVERIFY(profileManager->GetProfile(node->id) == node);
    }
    bool explained = false;
    QTimer::singleShot(0, window, [&] {
        auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(box);
        QCOMPARE(box->icon(), QMessageBox::Information);
        QVERIFY(box->text().contains(QString::fromUtf8("没有可删除")));
        explained = true;
        box->button(QMessageBox::Ok)->click();
    });
    child<QAction>(window, "menu_remove_unavailable")->trigger();
    QVERIFY(explained);
}

void GuiRegressionTest::coreSelectionKeepsOneRow() {
    const auto node = addNode(1080);
    window->show_group(baseGroup);
    window->refresh_proxy_list();
    const auto table = child<QTableWidget>(window, "proxyListTable");
    QCOMPARE(table->rowCount(), 1);
    QTest::qWait(20);
    const auto panel = child<QWidget>(window, "coreSelectionPanel");
    const int height = panel->height();
    const int windowHeight = window->height();
    table->selectRow(0);
    QTRY_COMPARE(child<QLabel>(window, "coreSelectedNode")->text(), node->bean->DisplayName());
    QCOMPARE(panel->height(), height);
    const QStringList buttons{"core_auto", "core_xray", "core_singbox"};
    for (int preference : {1, 2, 0}) {
        auto button = child<QPushButton>(window, qPrintable(buttons[preference]));
        QVERIFY(button->isEnabled());
        button->click();
        QCOMPARE(node->core_preference, preference);
        QTest::qWait(20);
        QCOMPARE(child<QLabel>(window, "coreSelectedNode")->text(), node->bean->DisplayName());
        QCOMPARE(panel->height(), height);
        QCOMPARE(window->height(), windowHeight);
        QVERIFY(!window->findChild<QLabel *>("coreChoiceReason"));
    }
    const auto output = qEnvironmentVariable("NEKORAY_TEST_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        QVERIFY(window->grab().save(output + "/core-selection-0.1.png"));
    }
}

void GuiRegressionTest::subscriptionAliasesAndCertificatePins() {
    using namespace NekoGui;
    SubscriptionMetadataRegression::GroupFixture fixture;
    const QString pin = QString("a1").repeated(32);
    const QString hy = "type: hysteria2, server: 127.0.0.1, port: 443, password: synthetic-hy2, skip-cert-verify: true, fingerprint: " + pin;
    auto parsed = fixture.stage("proxies:\n  - {name: VLESS, type: vless, server: 127.0.0.1, port: 443, uuid: 11111111-1111-4111-8111-111111111111, tls: true, flow: xtls-rprx-vision}\n  - {name: HY2, " + hy + ", ports: 60000-65530, mport: 60000-65530}\n");
    QVERIFY2(parsed.rejected_count == 0, qPrintable(parsed.rejection_reasons.join("; ")));
    QCOMPARE(parsed.updated_order.size(), 2);
    const auto vless = parsed.updated_order[0];
    const auto resolution = ResolveCore(vless, CoreType::V2RAY);
    QVERIFY2(resolution.xraySupported, qPrintable(resolution.xrayReason));
    QVERIFY2(resolution.singBoxSupported, qPrintable(resolution.singBoxReason));
    const auto mux = vless->bean->BuildCoreObjV2Ray(dataStore).outbound["mux"].toObject();
    QVERIFY(!mux["enabled"].toBool());
    QCOMPARE(mux["xudpConcurrency"].toInt(), 8);
    QCOMPARE(mux["xudpProxyUDP443"].toString(), QString("allow"));
    const auto hy2 = parsed.updated_order[1];
    QCOMPARE(hy2->QUICBean()->pinSHA256, pin);
    QVERIFY(hy2->QUICBean()->hopIntervalMin.isEmpty());
    QVERIFY(hy2->QUICBean()->hopIntervalMax.isEmpty());
    QCOMPARE(hy2->QUICBean()->Hysteria2ServerPorts(), QStringList{"60000:65530"});
    QCOMPARE(hy2->bean->BuildCoreObjSingBox(dataStore).outbound["tls_certificate_sha256"].toString(), pin);
    const auto noHops = fixture.stage("proxies:\n  - {name: HY2 no hopping, " + hy + "}\n");
    QVERIFY2(noHops.rejected_count == 0, qPrintable(noHops.rejection_reasons.join("; ")));
    QCOMPARE(noHops.updated_order.size(), 1);
    QVERIFY(noHops.updated_order.first()->QUICBean()->hopIntervalMin.isEmpty());
    QVERIFY(noHops.updated_order.first()->QUICBean()->hopIntervalMax.isEmpty());
    QVERIFY(noHops.updated_order.first()->QUICBean()->Hysteria2ServerPorts().isEmpty());
    QCOMPARE(noHops.updated_order.first()->QUICBean()->pinSHA256, pin);
    const auto roundTrip = fixture.stage(hy2->bean->ToShareLink());
    QCOMPARE(roundTrip.rejected_count, 0);
    QCOMPARE(roundTrip.updated_order.first()->bean->ToJson(), hy2->bean->ToJson());
    const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, "synthetic metadata");
    QVERIFY2(result.applied, qPrintable(result.error));
    {
        DialogEditProfile editor("", hy2->id, window);
        QCOMPARE(child<QLineEdit>(&editor, "hy2CertificatePin")->text(), pin);
        editor.accept();
        QCOMPARE(hy2->QUICBean()->pinSHA256, pin);
    }
    const auto before = fixture.group->ToJson();
    for (const auto &extra : {QString(", ports: 60000-65530, mport: 50000-51000"), QString(", server-ports: [443], mport: 444")}) {
        const auto invalid = fixture.stage("proxies:\n  - {" + hy + extra + "}\n");
        QVERIFY(invalid.rejected_count > 0);
        QVERIFY(!NekoGui_sub::CommitSubscriptionUpdate(fixture.group, invalid, "changed").applied);
        QCOMPARE(fixture.group->ToJson(), before);
    }
    const auto badPin = fixture.stage("hy2://synthetic@127.0.0.1:443?insecure=1&pinSHA256=chrome");
    QVERIFY(badPin.rejected_count > 0);
    const auto link = fixture.stage("hy2://synthetic@127.0.0.1:443?insecure=1&pinSHA256=" + pin);
    QCOMPARE(link.rejected_count, 0);
    QVERIFY(link.updated_order.first()->bean->ToShareLink().contains("pinSHA256="));
    QCOMPARE(fixture.stage(link.updated_order.first()->bean->ToShareLink()).updated_order.first()->QUICBean()->pinSHA256, pin);
}

void GuiRegressionTest::subscriptionUserAgentDoesNotDependOnCore() {
    using namespace NekoGui;
    QScopedValueRollback<QString> custom(dataStore->user_agent, QString{});
    const auto agent = dataStore->GetUserAgent();
    {
        QScopedValueRollback<int> core(coreType, 1 - coreType);
        QCOMPARE(dataStore->GetUserAgent(), agent);
    }
    QVERIFY(agent.contains("NekoRay_BY_WanQiu"));
    QVERIFY(agent.contains(QString(NKR_VERSION)));
    QVERIFY(agent.contains("Prefer ClashMeta Format"));
    dataStore->user_agent = "Custom subscription agent";
    QCOMPARE(dataStore->GetUserAgent(), dataStore->user_agent);
}

void GuiRegressionTest::optionalSubscriptionImportFixture() {
    const auto path = qEnvironmentVariable("NEKORAY_SUBSCRIPTION_FIXTURE");
    if (path.isEmpty()) QSKIP("No additional subscription fixture supplied");
    QFile input(path);
    QVERIFY(input.open(QIODevice::ReadOnly));
    SubscriptionMetadataRegression::GroupFixture fixture;
    const auto parsed = fixture.stage(QString::fromUtf8(input.readAll()));
    QVERIFY2(parsed.parse_error.isEmpty(), qPrintable(parsed.parse_error));
    QVERIFY2(parsed.rejected_count == 0, qPrintable(parsed.rejection_reasons.join("; ")));
    QVERIFY(!parsed.updated_order.isEmpty());
    int xray = 0, singBox = 0, pinned = 0;
    for (const auto &node : parsed.updated_order) {
        const auto resolution = NekoGui::ResolveCore(node, NekoGui::CoreType::V2RAY);
        xray += resolution.xraySupported;
        singBox += resolution.singBoxSupported;
        QVERIFY(resolution.targetCore >= 0);
        if (node->type == "hysteria2") pinned += !node->QUICBean()->pinSHA256.isEmpty();
    }
    const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, "fixture metadata");
    QVERIFY2(result.applied, qPrintable(result.error));
    const auto core = NekoGui::PreferredCore();
    QList<libcore::LoadConfigReq> requests;
    for (const auto &node : parsed.updated_order) {
        const auto resolution = NekoGui::ResolveCore(node, core);
        if (core == NekoGui::CoreType::V2RAY ? !resolution.xraySupported : !resolution.singBoxSupported) continue;
        const auto config = NekoGui::BuildConfig(node, true, false, core);
        QVERIFY2(config->error.isEmpty(), "Imported subscription could not generate its test configuration");
        libcore::LoadConfigReq request;
        request.set_core_config(QJsonDocument(config->coreConfig).toJson(QJsonDocument::Compact).toStdString());
        requests.append(request);
    }
    const auto instance = window->core_manager->Get(core);
    QVERIFY(instance && instance->client);
    auto checks = std::async(std::launch::async, [instance, requests] {
        for (const auto &request : requests) {
            bool rpcOK = false;
            const auto response = instance->client->Check(&rpcOK, request);
            if (!rpcOK || !response.error().empty()) return false;
        }
        return true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(checks.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
    QVERIFY2(checks.get(), "A generated subscription configuration failed real-core preflight");
    qInfo("Subscription fixture: %lld imported, %d Xray, %d sing-box, %d certificate pins", static_cast<long long>(parsed.updated_order.size()), xray, singBox, pinned);
    qInfo("Subscription fixture: %lld generated configurations accepted by %s", static_cast<long long>(requests.size()), qPrintable(NekoGui_sys::CoreManager::Name(core)));
}
