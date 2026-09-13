#pragma once

#include <QJsonObject>
#include <QString>

namespace NekoGui {
    // These are outcomes of a particular test, not a permanent property of a
    // server. Only a ConnectionFailure with matching provenance is removable.
    enum class TestResultStatus {
        Untested,
        Success,
        ConnectionFailure,
        Unsupported,
        CoreError,
        Cancelled,
        Stale,
        Legacy,
    };

    struct ProfileTestResult {
        TestResultStatus status = TestResultStatus::Untested;
        int core = -1; // CoreType value: 0 Xray, 1 sing-box; -1 unknown.
        QString build_id;
        QString config_fingerprint;
        QString generated_config_digest;
        QString mode = "url";
        QString scope = "next";
        qint64 timestamp_ms = 0;
        QString error;
        QString report;
        QJsonObject parameters;
        int latency_ms = 0;

        [[nodiscard]] static QString StatusName(TestResultStatus status) {
            switch (status) {
                case TestResultStatus::Success: return "success";
                case TestResultStatus::ConnectionFailure: return "connection_failure";
                case TestResultStatus::Unsupported: return "unsupported";
                case TestResultStatus::CoreError: return "core_error";
                case TestResultStatus::Cancelled: return "cancelled";
                case TestResultStatus::Stale: return "stale";
                case TestResultStatus::Legacy: return "legacy";
                default: return "untested";
            }
        }

        [[nodiscard]] QJsonObject ToJson() const {
            return {{"status", StatusName(status)}, {"core", core}, {"build_id", build_id},
                    {"config_fingerprint", config_fingerprint}, {"generated_config_digest", generated_config_digest},
                    {"mode", mode}, {"scope", scope},
                    {"timestamp_ms", timestamp_ms}, {"error", error}, {"report", report},
                    {"parameters", parameters}, {"latency_ms", latency_ms}};
        }

        [[nodiscard]] static ProfileTestResult FromJson(const QJsonObject &object) {
            ProfileTestResult result;
            const auto status = object.value("status").toString();
            for (auto candidate: {TestResultStatus::Success, TestResultStatus::ConnectionFailure,
                                  TestResultStatus::Unsupported, TestResultStatus::CoreError,
                                  TestResultStatus::Cancelled, TestResultStatus::Stale, TestResultStatus::Legacy}) {
                if (status == StatusName(candidate)) result.status = candidate;
            }
            result.core = object.value("core").toInt(-1);
            result.build_id = object.value("build_id").toString();
            result.config_fingerprint = object.value("config_fingerprint").toString();
            result.generated_config_digest = object.value("generated_config_digest").toString();
            result.mode = object.value("mode").toString();
            result.scope = object.value("scope").toString();
            result.timestamp_ms = static_cast<qint64>(object.value("timestamp_ms").toDouble());
            result.error = object.value("error").toString();
            result.report = object.value("report").toString();
            result.parameters = object.value("parameters").toObject();
            result.latency_ms = object.value("latency_ms").toInt();
            return result;
        }

        [[nodiscard]] bool Matches(int expectedCore, const QString &buildId, const QString &fingerprint,
                                   const QString &expectedScope = "next", const QString &expectedMode = "url") const {
            return (expectedCore == 0 || expectedCore == 1) && core == expectedCore &&
                   !buildId.isEmpty() && build_id == buildId && !fingerprint.isEmpty() &&
                   config_fingerprint == fingerprint && !generated_config_digest.isEmpty() &&
                   !expectedScope.isEmpty() && scope == expectedScope &&
                   !expectedMode.isEmpty() && mode == expectedMode && timestamp_ms > 0 &&
                   ((status == TestResultStatus::Success && latency_ms >= 0) ||
                    status == TestResultStatus::ConnectionFailure);
        }
    };
} // namespace NekoGui
