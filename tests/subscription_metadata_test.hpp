#pragma once

#include <QtTest>

#include "db/Database.hpp"
#include "db/ProfileFilter.hpp"
#include "sub/GroupUpdater.hpp"

// Called from the GUI suite, which already runs in an isolated QTemporaryDir.
// These fixtures use synthetic loopback addresses and never make a request.
namespace SubscriptionMetadataRegression {
    inline std::shared_ptr<NekoGui::ProxyEntity> Node(int port, const QString &name = {}) {
        auto node = NekoGui::ProfileManager::NewProxyEntity("socks");
        node->bean->serverAddress = "127.0.0.1";
        node->bean->serverPort = port;
        node->bean->name = name;
        return node;
    }

    inline NekoGui::ProfileTestResult Result(NekoGui::TestResultStatus status, int core = 0) {
        NekoGui::ProfileTestResult result;
        result.status = status;
        result.core = core;
        result.build_id = "synthetic-build";
        result.config_fingerprint = "synthetic-config-url-and-dns";
        result.generated_config_digest = "synthetic-generated-config";
        result.latency_ms = status == NekoGui::TestResultStatus::Success ? 45 : 0;
        return result;
    }

    inline void TestResultProvenance() {
        using namespace NekoGui;
        auto node = Node(1080);
        node->core_preference = 2;
        node->RecordTestResult(Result(TestResultStatus::ConnectionFailure));
        QVERIFY(node->IsRemovalCandidate(0, "synthetic-build", "synthetic-config-url-and-dns"));
        QVERIFY(!node->IsRemovalCandidate(1, "synthetic-build", "synthetic-config-url-and-dns"));
        QVERIFY(!node->IsRemovalCandidate(0, "updated-build", "synthetic-config-url-and-dns"));
        QVERIFY(!node->IsRemovalCandidate(0, "synthetic-build", "changed-dns-or-url"));
        QVERIFY(!node->HasValidTestResult(0, "", "synthetic-config-url-and-dns"));
        QVERIFY(!node->HasValidTestResult(0, "synthetic-build", ""));
        QVERIFY(!node->HasValidTestResult(0, "synthetic-build", "synthetic-config-url-and-dns", "current"));
        QVERIFY(!node->HasValidTestResult(0, "synthetic-build", "synthetic-config-url-and-dns", "next", "full"));

        for (auto status: {TestResultStatus::Unsupported, TestResultStatus::CoreError, TestResultStatus::Untested}) {
            node->RecordTestResult(Result(status));
            QVERIFY(!node->IsRemovalCandidate(0, "synthetic-build", "synthetic-config-url-and-dns"));
        }
        node->RecordTestResult(Result(TestResultStatus::Success));
        node->RecordTestResult(Result(TestResultStatus::Cancelled));
        node->RecordTestResult(Result(TestResultStatus::Stale));
        QCOMPARE(node->LastTestResult(0).status, TestResultStatus::Success);
        QCOMPARE(node->LastTestResult(0).latency_ms, 45);
        QVERIFY(!node->IsRemovalCandidate(0, "synthetic-build", "synthetic-config-url-and-dns"));

        auto current = Result(TestResultStatus::ConnectionFailure);
        current.scope = "current";
        node->RecordTestResult(current);
        QCOMPARE(node->LastTestResult(0, "current").status, TestResultStatus::ConnectionFailure);
        QVERIFY(!node->IsRemovalCandidate(0, "synthetic-build", "synthetic-config-url-and-dns"));
        node->RecordTestResult(Result(TestResultStatus::ConnectionFailure, 1));
        QVERIFY(node->IsRemovalCandidate(1, "synthetic-build", "synthetic-config-url-and-dns"));

        auto full = Result(TestResultStatus::Success);
        full.mode = "full";
        full.report = "Latency: 45 ms\nDownload test was not selected";
        full.parameters = {{"latency", true}, {"udp_latency", false}, {"speed", false}, {"in_out", false}};
        node->RecordTestResult(full);
        QCOMPARE(node->LastTestResult(0, "next", "full").report, full.report);
        QCOMPARE(node->LastTestResult(0, "next", "full").parameters, full.parameters);
        QVERIFY(node->LastTestResult(0, "next", "full").error.isEmpty());

        auto restored = Node(1080);
        restored->FromJson(node->ToJson());
        QCOMPARE(restored->core_preference, 2);
        QCOMPARE(restored->test_results, node->test_results);
        QCOMPARE(restored->LastTestResult(0, "next", "full").report, full.report);
        QCOMPARE(restored->LastTestResult(0, "next", "full").parameters, full.parameters);
        QVERIFY(restored->LastTestResult(0, "next", "full").error.isEmpty());
        QVERIFY(restored->IsRemovalCandidate(1, "synthetic-build", "synthetic-config-url-and-dns"));
        QVERIFY(!node->bean->ToJson().contains("core_preference"));
        QVERIFY(!node->bean->ToJson().contains("test_results"));
        restored->core_preference = 1;
        QCOMPARE(ProfileFilter::SubscriptionIdentity(node), ProfileFilter::SubscriptionIdentity(restored));
        QList<std::shared_ptr<ProxyEntity>> unique;
        ProfileFilter::Uniq({node, restored}, unique);
        QCOMPARE(unique.size(), 1);
    }

    inline void LegacyResultsAreHistory() {
        using namespace NekoGui;
        for (int oldLatency: {-1, -200, 79}) {
            auto node = Node(1080);
            node->FromJson({{"yc", oldLatency}, {"report", "old result with unknown origin"}});
            QCOMPARE(node->core_preference, 0);
            QCOMPARE(node->latency, 0);
            QVERIFY(node->full_test_report.isEmpty());
            QCOMPARE(node->LastTestResult(0).status, TestResultStatus::Untested);
            QVERIFY(!node->IsRemovalCandidate(0, "any", "any"));
            const auto history = node->test_results.value("history").toArray();
            QCOMPARE(history.size(), 1);
            const auto legacy = ProfileTestResult::FromJson(history.first().toObject());
            QCOMPARE(legacy.status, TestResultStatus::Legacy);
            QCOMPARE(legacy.core, -1);
            QCOMPARE(legacy.latency_ms, oldLatency);
            QCOMPARE(legacy.timestamp_ms, qint64(0));
            QVERIFY(legacy.report.isEmpty());
            QVERIFY(legacy.parameters.isEmpty());
            auto restored = Node(1080);
            restored->FromJson(node->ToJson());
            QCOMPARE(restored->test_results.value("history").toArray().size(), 1);
        }
    }

    struct GroupFixture {
        std::shared_ptr<NekoGui::Group> group;
        bool oldClear = NekoGui::dataStore->sub_clear;
        int oldStarted = NekoGui::dataStore->started_id;

        GroupFixture() {
            group = NekoGui::ProfileManager::NewGroup();
            group->name = "Synthetic subscription metadata fixture";
            if (!NekoGui::profileManager->AddGroup(group)) qFatal("cannot create test group");
        }

        ~GroupFixture() {
            NekoGui::dataStore->started_id = oldStarted;
            NekoGui::dataStore->sub_clear = oldClear;
            NekoGui::profileManager->DeleteGroup(group->id);
        }

        std::shared_ptr<NekoGui::ProxyEntity> add(int port, const QString &name, int preference = 0) const {
            auto node = Node(port, name);
            node->core_preference = preference;
            if (!NekoGui::profileManager->AddProfile(node, group->id)) qFatal("cannot create test node");
            return node;
        }

        NekoGui_sub::RawUpdater stage(const QString &content) const {
            NekoGui_sub::RawUpdater parsed;
            parsed.stage_only = true;
            parsed.gid_add_to = group->id;
            parsed.update(content);
            return parsed;
        }
    };

    inline void SubscriptionRenameReorderAndClear() {
        for (bool clear: {false, true}) {
            GroupFixture fixture;
            NekoGui::dataStore->sub_clear = clear;
            auto first = fixture.add(1080, "first", 1);
            auto second = fixture.add(1081, "second", 2);
            first->RecordTestResult(Result(NekoGui::TestResultStatus::Success));
            const auto history = first->test_results;
            auto parsed = fixture.stage("socks://127.0.0.1:1081#renamed-second\nsocks://127.0.0.1:1080#renamed-first\n");
            QCOMPARE(fixture.group->Profiles().size(), 2);
            for (const auto &node: parsed.updated_order) QCOMPARE(node->id, -1);
            const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, "new subscription info");
            QVERIFY2(result.applied, qPrintable(result.error));
            QCOMPARE(result.preserved, 2);
            QCOMPARE(result.added, 0);
            QCOMPARE(result.removed, 0);
            QCOMPARE(fixture.group->order, (QList<int>{second->id, first->id}));
            QVERIFY(NekoGui::profileManager->GetProfile(first->id) == first);
            QVERIFY(NekoGui::profileManager->GetProfile(second->id) == second);
            QCOMPARE(first->core_preference, 1);
            QCOMPARE(second->core_preference, 2);
            QCOMPARE(first->bean->name, QString("renamed-first"));
            QCOMPARE(first->test_results, history);
            QCOMPARE(fixture.group->info, QString("new subscription info"));
            auto loaded = Node(1080);
            loaded->fn = first->fn;
            QVERIFY(loaded->Load());
            QCOMPARE(loaded->id, first->id);
            QCOMPARE(loaded->core_preference, 1);
            QCOMPARE(loaded->test_results, history);
        }
    }

    inline void FailedSubscriptionKeepsExistingGroup() {
        GroupFixture fixture;
        NekoGui::dataStore->sub_clear = true;
        auto first = fixture.add(1080, "first", 2);
        fixture.group->order = {first->id};
        fixture.group->info = "old subscription info";
        fixture.group->sub_last_update = 42;
        const auto oldGroup = fixture.group->ToJson();
        for (const auto &content: {QString("<html>server error</html>"), QString("socks://:0"),
                                  QString("# no valid nodes\n\n"), QString("proxies: []"),
                                  QString("proxies:\n - {type: socks5, server: 127.0.0.1, port: 1080}\n - [\n")}) {
            const auto parsed = fixture.stage(content);
            const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, "must not replace old info");
            QVERIFY(!result.applied);
            QVERIFY(!result.error.isEmpty());
            QCOMPARE(fixture.group->ToJson(), oldGroup);
            QCOMPARE(fixture.group->Profiles().size(), 1);
            QVERIFY(NekoGui::profileManager->GetProfile(first->id) == first);
            QCOMPARE(first->core_preference, 2);
        }
    }

    inline void SubscriptionDuplicateIdentityIsNotGuessed() {
        {
            GroupFixture fixture;
            auto first = fixture.add(1080, "duplicate-a", 1);
            auto second = fixture.add(1080, "duplicate-b", 2);
            auto parsed = fixture.stage("socks://127.0.0.1:1080#duplicate-a");
            const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, {});
            QVERIFY(result.applied);
            QCOMPARE(result.ambiguous, 1);
            QCOMPARE(result.preserved, 0);
            const auto now = fixture.group->Profiles();
            QCOMPARE(now.size(), 1);
            QCOMPARE(now.first()->core_preference, 0);
            QVERIFY(now.first()->id > second->id);
            QVERIFY(NekoGui::profileManager->GetProfile(first->id) == nullptr);
        }
        {
            GroupFixture fixture;
            auto old = fixture.add(1080, "duplicate-a", 2);
            auto parsed = fixture.stage("socks://127.0.0.1:1080#duplicate-a\nsocks://127.0.0.1:1080#duplicate-b");
            const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, {});
            QVERIFY(result.applied);
            QCOMPARE(result.ambiguous, 2);
            QCOMPARE(result.preserved, 0);
            QCOMPARE(fixture.group->Profiles().size(), 2);
            for (const auto &node: fixture.group->Profiles()) {
                QCOMPARE(node->core_preference, 0);
                QVERIFY(node->id > old->id);
            }
        }
    }

    inline void SubscriptionIdentityIgnoresNameButNotConfigurationOrGroup() {
        GroupFixture target;
        GroupFixture other;
        auto old = target.add(1080, "same name", 1);
        auto unrelated = other.add(1081, "same name", 2);
        const auto before = target.group->ToJson();
        auto parsed = target.stage("socks://127.0.0.1:1081#same%20name\nunknown://invalid");
        QCOMPARE(parsed.rejected_count, 1);
        QCOMPARE(parsed.updated_order.size(), 1);
        const auto rejected = NekoGui_sub::CommitSubscriptionUpdate(target.group, parsed, {});
        QVERIFY(!rejected.applied);
        QCOMPARE(target.group->ToJson(), before);
        QVERIFY(NekoGui::profileManager->GetProfile(old->id) == old);
        QCOMPARE(old->core_preference, 1);
        QVERIFY(NekoGui::profileManager->GetProfile(unrelated->id) == unrelated);
        parsed = target.stage("socks://127.0.0.1:1081#same%20name");
        QCOMPARE(parsed.rejected_count, 0);
        const auto result = NekoGui_sub::CommitSubscriptionUpdate(target.group, parsed, {});
        QVERIFY(result.applied);
        QCOMPARE(result.preserved, 0);
        QCOMPARE(target.group->Profiles().first()->core_preference, 0);
        QVERIFY(NekoGui::profileManager->GetProfile(old->id) == nullptr);
        QVERIFY(NekoGui::profileManager->GetProfile(unrelated->id) == unrelated);
        QCOMPARE(unrelated->core_preference, 2);

        auto first = Node(1080, "old name");
        auto changed = Node(1080, "new name");
        QCOMPARE(NekoGui::ProfileFilter::SubscriptionIdentity(first), NekoGui::ProfileFilter::SubscriptionIdentity(changed));
        changed->bean->custom_outbound = "{\"tag\":\"different-custom-settings\"}";
        QVERIFY(NekoGui::ProfileFilter::SubscriptionIdentity(first) != NekoGui::ProfileFilter::SubscriptionIdentity(changed));
    }

    inline void SubscriptionKeepsActiveNodeVisible() {
        GroupFixture fixture;
        auto active = fixture.add(1080, "active synthetic node", 2);
        NekoGui::dataStore->started_id = active->id;
        auto parsed = fixture.stage("socks://127.0.0.1:1081#replacement");
        const auto result = NekoGui_sub::CommitSubscriptionUpdate(fixture.group, parsed, {});
        QVERIFY(result.applied);
        QCOMPARE(result.retained_running, 1);
        QVERIFY(NekoGui::profileManager->GetProfile(active->id) == active);
        QVERIFY(fixture.group->order.contains(active->id));
        QCOMPARE(active->core_preference, 2);
        QCOMPARE(fixture.group->Profiles().size(), 2);
    }
} // namespace SubscriptionMetadataRegression
