// Included inside GuiRegressionTest. Every endpoint and listener is loopback;
// none of these cases changes the host's system proxy or TUN routes.

    void manualCoreSwitchAndSameConfigurationNoop() {
        FixtureServer server;
        auto profile = addNode(server.serverPort());
        profile->core_preference = 1;
        window->refresh_proxy_list();
        const auto settingsOwner = NekoGui::coreType;
        window->neko_start(profile->id);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
        QCOMPARE(window->running, profile);
        QCOMPARE(window->active_core(), int(NekoGui::CoreType::V2RAY));
        const auto original = window->core_session;
        const auto rayGeneration = window->core_manager->Generation(0);
        window->set_core_preference(profile->id, 1, true);
        QVERIFY(!window->core_switch_busy);
        QCOMPARE(window->core_session, original);
        window->set_core_preference(profile->id, 2, true);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy && window->active_core() == 1, 12000);
        QVERIFY(window->core_session != original);
        QCOMPARE(window->running, profile);
        QCOMPARE(profile->core_preference, 2);
        QCOMPARE(NekoGui::coreType, settingsOwner);
        QCOMPARE(window->core_manager->Generation(0), rayGeneration);
        QVERIFY(window->core_manager->Ready(0));
        QVERIFY(window->core_manager->Ready(1));
        const auto box = window->core_session;
        window->set_core_preference(profile->id, 2, true);
        QVERIFY(!window->core_switch_busy);
        QCOMPARE(window->core_session, box);
    }

    void bulkPreferenceKeepsConnectionUntilExplicitClick() {
        FixtureServer server;
        auto active = addNode(server.serverPort());
        auto idle = addNode(server.serverPort());
        active->core_preference = 1;
        window->neko_start(active->id);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
        const auto saved = window->core_session;
        window->refresh_proxy_list();
        child<QTableWidget>(window, "proxyListTable")->selectAll();
        child<QAction>(window, "core_bulk_2")->trigger();
        QCOMPARE(active->core_preference, 2);
        QCOMPARE(idle->core_preference, 2);
        QCOMPARE(window->core_session, saved);
        QVERIFY(!window->core_switch_busy);
        // The saved preference already matches the click; the actual session
        // still needs to change, so this must not short-circuit.
        window->set_core_preference(active->id, 2, true);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy && window->active_core() == 1, 12000);
        const auto switched = window->core_session;
        window->set_core_preference(idle->id, 1, true);
        QCOMPARE(idle->core_preference, 1);
        QCOMPARE(window->core_session, switched);
        QCOMPARE(window->running, active);
    }

    void mixedCoreBatchKeepsOneBudgetAndSurvivesSwitch() {
        ProbeCount count;
        FixtureServer server(220, 204, {}, &count);
        auto activeGroup = addGroup("active connection outside test group");
        auto active = addNode(server.serverPort(), activeGroup->id);
        active->core_preference = 1;
        QList<std::shared_ptr<NekoGui::ProxyEntity>> profiles;
        for (int n = 0; n < 12; ++n) {
            auto profile = addNode(server.serverPort());
            profile->core_preference = n % 2 + 1;
            profiles.append(profile);
        }
        window->neko_start(active->id);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
        const auto rayGeneration = window->core_manager->Generation(0);
        startBatch();
        QTRY_VERIFY_WITH_TIMEOUT(count.active >= 3 && window->core_manager->Ready(1), 8000);
        const auto boxGeneration = window->core_manager->Generation(1);
        window->set_core_preference(active->id, 2, true);
        QTRY_VERIFY_WITH_TIMEOUT(!speedtest_batch && !window->core_switch_busy, 15000);
        QCOMPARE(window->running, active);
        QCOMPARE(window->active_core(), 1);
        QCOMPARE(window->core_manager->Generation(0), rayGeneration);
        QCOMPARE(window->core_manager->Generation(1), boxGeneration);
        for (const auto &profile : profiles) {
            const auto core = profile->core_preference - 1;
            const auto result = profile->LastTestResult(core);
            QCOMPARE(result.status, NekoGui::TestResultStatus::Success);
            QCOMPARE(result.core, core);
            QCOMPARE(result.build_id, window->core_manager->Identity(core));
            QVERIFY(!result.generated_config_digest.isEmpty());
            QVERIFY(profile->HasValidTestResult(core, result.build_id, window->test_fingerprint(profile, core)));
            QVERIFY(profile->latency > 0);
        }
        QVERIFY2(count.peak >= 3, "Mixed-core probes unexpectedly ran serially");
        QVERIFY2(count.peak <= NekoGui::dataStore->test_concurrent, "Each core used a separate concurrency budget");
        qInfo("Mixed-core batch: peak %d, configured total %d", count.peak, NekoGui::dataStore->test_concurrent);
    }

    void targetStartFailureRestoresSavedConfiguration() {
        FixtureServer server;
        auto profile = addNode(server.serverPort());
        profile->core_preference = 1;
        window->neko_start(profile->id);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
        const auto saved = window->core_session;
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        const auto oldPort = NekoGui::dataStore->inbound_http_port;
        const auto restore = qScopeGuard([oldPort] { NekoGui::dataStore->inbound_http_port = oldPort; });
        NekoGui::dataStore->inbound_http_port = occupied.serverPort();
        window->set_core_preference(profile->id, 2, true);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy, 20000);
        QCOMPARE(window->core_session, saved);
        QCOMPARE(window->running, profile);
        QCOMPARE(window->active_core(), 0);
        QCOMPARE(profile->core_preference, 1);
        QCOMPARE(saved->context.settings->inbound_http_port, oldPort);
        QVERIFY(saved->instance->ready && saved->instance->alive);
        QVERIFY(window->core_notice.contains("restored", Qt::CaseInsensitive));
        // Prove the restored instance is usable, rather than only checking UI.
        window->speedtest_current();
        QTRY_COMPARE_WITH_TIMEOUT(profile->LastTestResult(0, "current").status, NekoGui::TestResultStatus::Success, 5000);
        QVERIFY(profile->LastTestResult(0).timestamp_ms == 0);
    }

    void invalidTargetPreflightKeepsCurrentConnection() {
        FixtureServer server;
        auto active = addNode(server.serverPort());
        active->core_preference = 1;
        window->neko_start(active->id);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
        const auto saved = window->core_session;
        auto invalid = NekoGui::ProfileManager::NewProxyEntity("vless");
        invalid->bean->serverAddress = "127.0.0.1";
        invalid->bean->serverPort = server.serverPort();
        invalid->TrojanVLESSBean()->password = QString(80, 'x');
        invalid->core_preference = 1;
        NekoGui::profileManager->AddProfile(invalid, baseGroup);
        window->neko_start(invalid->id);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy, 12000);
        QCOMPARE(window->core_session, saved);
        QCOMPARE(window->running, active);
        QVERIFY(!window->core_notice.isEmpty());
        QVERIFY(!invalid->IsRemovalCandidate(0, window->core_manager->Identity(0), window->test_fingerprint(invalid, 0)));
    }

    void latestSelectionWinsAndDeletedPreparationCannotStart() {
        FixtureServer server;
        auto active = addNode(server.serverPort());
        active->core_preference = 1;
        window->neko_start(active->id);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
        window->set_core_preference(active->id, 2, true);
        window->set_core_preference(active->id, 1, true);
        window->set_core_preference(active->id, 2, true);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy && !window->pending_core_intent, 12000);
        QCOMPARE(active->core_preference, 2);
        QCOMPARE(window->active_core(), 1);
        const auto saved = window->core_session;
        auto removed = addNode(server.serverPort());
        removed->core_preference = 1;
        window->neko_start(removed->id);
        const int id = removed->id;
        NekoGui::profileManager->DeleteProfile(id);
        auto replacement = addNode(server.serverPort());
        QCOMPARE(replacement->id, id);
        window->refresh_proxy_list();
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy && !window->pending_core_intent, 12000);
        QCOMPARE(window->core_session, saved);
        QCOMPARE(window->running, active);
        QCOMPARE(NekoGui::dataStore->started_id, active->id);
        QVERIFY(window->core_session->profile != replacement);
    }

    void ownedCoreCrashRestoresSavedSession() {
        FixtureServer server;
        auto profile = addNode(server.serverPort());
        profile->core_preference = 1;
        window->last_crashed_core = -1;
        window->last_core_crash = {};
        window->neko_start(profile->id);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
        const auto saved = window->core_session;
        const auto generation = saved->generation;
        const auto instance = saved->instance;
        runOnUiThread([instance] { instance->process->kill(); }, DS_cores);
        QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy &&
                                window->core_session->generation > generation && instance->ready && instance->alive, 15000);
        QCOMPARE(window->core_session->fingerprint, saved->fingerprint);
        QCOMPARE(window->running, profile);
        QCOMPARE(profile->core_preference, 1);
        window->speedtest_current();
        QTRY_COMPARE_WITH_TIMEOUT(profile->LastTestResult(0, "current").status, NekoGui::TestResultStatus::Success, 5000);
    }
