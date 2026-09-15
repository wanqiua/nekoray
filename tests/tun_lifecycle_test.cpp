#include "ui/TunLifecycle.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>
#include <deque>
#include <functional>
#include <utility>

// Deterministic fault injection for the production transitions. These doubles
// never launch a core, create a network interface, or change the system proxy.
namespace {

struct Request {
    QString config;
    QStringList stats;
    bool connectionStats = false;

    bool operator==(const Request &other) const {
        return config == other.config && stats == other.stats && connectionStats == other.connectionStats;
    }
};

struct Reply {
    bool acknowledged = true;
    QString error;
    bool exitProcess = false;
};

struct Process {
    bool running = true;
    bool resourcesMayBeActive = false;
    bool terminateSucceeds = true;
    bool unsafeTermination = false;
    QStringList events;

    bool Running() const { return running; }
    bool WaitForExit(int) {
        events.append("wait");
        return !running;
    }
    bool TerminateOwned(int, QString &error) {
        events.append("terminate");
        unsafeTermination = unsafeTermination || resourcesMayBeActive;
        if (!terminateSucceeds) {
            error = "fixture: process termination failed";
            return false;
        }
        running = false;
        return true;
    }
};

struct Client {
    std::shared_ptr<Process> process;
    std::deque<Reply> starts;
    std::deque<Reply> stops;
    QList<Request> requests;
    std::function<void()> afterStart;
    bool exitSucceeds = true;
    bool unsafeStart = false;
    bool unsafeExit = false;
    bool calledAfterExit = false;

    static Reply Next(std::deque<Reply> &queue) {
        if (queue.empty()) return {};
        const auto reply = queue.front();
        queue.pop_front();
        return reply;
    }

    QString Start(bool *ok, const Request &request) {
        process->events.append("start:" + request.config);
        requests.append(request);
        calledAfterExit = calledAfterExit || !process->running;
        unsafeStart = unsafeStart || process->resourcesMayBeActive;
        const auto reply = Next(starts);
        *ok = reply.acknowledged;
        // A lost reply may mean Start actually succeeded. Stop must reconcile
        // that uncertainty before restoring the saved request or exiting.
        process->resourcesMayBeActive = !reply.acknowledged || reply.error.isEmpty();
        if (reply.exitProcess) {
            process->running = false;
            process->resourcesMayBeActive = false;
        }
        if (afterStart) afterStart();
        return reply.error;
    }

    QString Stop(bool *ok) {
        process->events.append("stop");
        calledAfterExit = calledAfterExit || !process->running;
        const auto reply = Next(stops);
        *ok = reply.acknowledged;
        if (reply.acknowledged && reply.error.isEmpty()) process->resourcesMayBeActive = false;
        if (reply.exitProcess) {
            process->running = false;
            process->resourcesMayBeActive = false;
        }
        return reply.error;
    }

    void Exit() {
        process->events.append("exit");
        unsafeExit = unsafeExit || process->resourcesMayBeActive;
        if (exitSucceeds) process->running = false;
    }
};

struct Daemon {
    std::shared_ptr<Process> process = std::make_shared<Process>();
    std::shared_ptr<Client> client = std::make_shared<Client>();
    Daemon() { client->process = process; }
};

using State = NekoGui::TunLifecycle::State<Daemon, Request>;
using Outcome = NekoGui::TunLifecycle::Outcome<State>;
using NekoGui::TunLifecycle::Stop;

struct Fixture {
    std::shared_ptr<Daemon> daemon = std::make_shared<Daemon>();
    Request original{"original", {"old-proxy", "old-bypass"}, true};
    Request next{"next", {"new-bypass"}, false};
    Outcome out;
    bool cancelled = false;
    std::function<bool()> validate;

    explicit Fixture(bool enabled = true) {
        out.state.daemon = daemon;
        out.state.enabled = enabled;
        out.state.request = original;
        out.state.fingerprint = "original-fingerprint";
        daemon->process->resourcesMayBeActive = enabled;
    }

    void Apply() {
        NekoGui::TunLifecycle::Apply(out, next, QStringLiteral("next-fingerprint"),
                                    [&] { return cancelled; }, [&] { return !validate || validate(); });
    }

    bool Safe() const {
        return !daemon->client->unsafeStart && !daemon->client->unsafeExit &&
               !daemon->client->calledAfterExit && !daemon->process->unsafeTermination;
    }
};

#define CHECK(expression) do { \
    if (!(expression)) { \
        qCritical() << __func__ << "line" << __LINE__ << "failed:" << #expression; \
        return false; \
    } \
} while (false)

bool FreshStart() {
    Fixture fixture(false);
    fixture.Apply();
    CHECK(fixture.out.succeeded);
    CHECK(fixture.out.state.enabled && !fixture.out.state.uncertain);
    CHECK(fixture.out.state.request == fixture.next);
    CHECK(fixture.out.state.fingerprint == "next-fingerprint");
    CHECK(fixture.daemon->process->events == QStringList{"start:next"});
    CHECK(fixture.Safe());
    return true;
}

bool ReplaceKeepsOwnedDaemon() {
    Fixture fixture;
    fixture.Apply();
    CHECK(fixture.out.succeeded);
    CHECK(fixture.out.state.daemon == fixture.daemon);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next"}));
    CHECK(fixture.Safe());
    return true;
}

bool ExitDuringStopCannotStartClearedState() {
    Fixture fixture;
    fixture.daemon->client->stops.push_back({false, {}, true});
    fixture.Apply();
    CHECK(!fixture.out.succeeded);
    CHECK(!fixture.out.state.daemon && !fixture.out.state.enabled && !fixture.out.state.uncertain);
    CHECK(!fixture.out.notice.isEmpty());
    CHECK(fixture.daemon->process->events == QStringList{"stop"});
    CHECK(fixture.daemon->client->requests.isEmpty());
    CHECK(fixture.Safe());
    return true;
}

bool ExitAfterStartAcknowledgementCannotReportSuccess() {
    Fixture fixture;
    fixture.daemon->client->starts.push_back({true, {}, true});
    fixture.Apply();
    CHECK(!fixture.out.succeeded);
    CHECK(!fixture.out.state.daemon && !fixture.out.state.enabled);
    CHECK(!fixture.out.notice.isEmpty());
    CHECK(fixture.daemon->client->requests.size() == 1);
    CHECK(fixture.Safe());
    return true;
}

bool UnacknowledgedStopKeepsOwnership() {
    Fixture fixture;
    fixture.daemon->client->stops = {{false, {}}, {false, {}}};
    fixture.Apply();
    CHECK(!fixture.out.succeeded);
    CHECK(fixture.out.state.daemon == fixture.daemon);
    CHECK(fixture.out.state.enabled && fixture.out.state.uncertain);
    CHECK(fixture.out.state.request == fixture.original);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "stop"}));
    CHECK(fixture.Safe());
    return true;
}

bool RepeatedStopErrorNeverExitsProcess() {
    Fixture fixture;
    fixture.daemon->client->stops = {{true, "fixture: Close failed"}, {true, "fixture: Close failed"}};
    QString error;
    CHECK(!Stop(fixture.out.state, true, error));
    CHECK(!Stop(fixture.out.state, true, error));
    CHECK(fixture.out.state.daemon == fixture.daemon);
    CHECK(fixture.out.state.enabled && fixture.out.state.uncertain);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "stop"}));
    CHECK(fixture.Safe());
    return true;
}

bool FailedStartRestoresExactRequestAndClearsUncertainty() {
    Fixture fixture;
    fixture.out.state.uncertain = true;
    fixture.daemon->client->starts = {{true, "fixture: startup failed"}, {true, {}}};
    fixture.Apply();
    CHECK(!fixture.out.succeeded);
    CHECK(fixture.out.state.daemon == fixture.daemon);
    CHECK(fixture.out.state.enabled && !fixture.out.state.uncertain);
    CHECK(fixture.out.state.request == fixture.original);
    CHECK(fixture.out.state.fingerprint == "original-fingerprint");
    CHECK(fixture.daemon->client->requests.size() == 2);
    CHECK(fixture.daemon->client->requests.last() == fixture.original);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next", "stop", "start:original"}));
    CHECK(fixture.Safe());
    return true;
}

bool LostStartReplyIsReconciledBeforeRollback() {
    Fixture fixture;
    fixture.daemon->client->starts = {{false, {}}, {true, {}}};
    fixture.Apply();
    CHECK(!fixture.out.succeeded && fixture.out.state.enabled && !fixture.out.state.uncertain);
    CHECK(fixture.out.state.request == fixture.original);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next", "stop", "start:original"}));
    CHECK(fixture.Safe());
    return true;
}

bool ExitDuringFailedStartCleanupCannotRestoreDeadDaemon() {
    Fixture fixture;
    fixture.daemon->client->starts.push_back({true, "fixture: startup failed"});
    fixture.daemon->client->stops = {{true, {}}, {false, {}, true}};
    fixture.Apply();
    CHECK(!fixture.out.succeeded && !fixture.out.state.daemon);
    CHECK(!fixture.out.state.enabled && !fixture.out.state.uncertain);
    CHECK(fixture.daemon->client->requests.size() == 1);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next", "stop"}));
    CHECK(fixture.Safe());
    return true;
}

bool FailedStartAndRollbackCleanUpOwnedSession() {
    Fixture fixture;
    fixture.daemon->client->starts = {{true, "fixture: startup failed"}, {false, {}}};
    fixture.Apply();
    CHECK(!fixture.out.succeeded && !fixture.out.state.daemon);
    CHECK(!fixture.out.state.enabled && !fixture.out.state.uncertain);
    CHECK(!fixture.daemon->process->running);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next", "stop", "start:original", "stop", "exit", "wait"}));
    CHECK(fixture.Safe());
    return true;
}

bool CancelAfterStartClosesWithoutRestoring() {
    Fixture fixture;
    fixture.daemon->client->afterStart = [&] { fixture.cancelled = true; };
    fixture.Apply();
    CHECK(!fixture.out.succeeded && !fixture.out.state.daemon);
    CHECK(!fixture.daemon->process->running);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next", "stop", "exit", "wait"}));
    CHECK(fixture.Safe());
    return true;
}

bool ChangedSettingsRestoreOriginal() {
    Fixture fixture;
    fixture.validate = [&] { fixture.out.notice = "fixture: settings changed"; return false; };
    fixture.Apply();
    CHECK(!fixture.out.succeeded && fixture.out.state.enabled && !fixture.out.state.uncertain);
    CHECK(fixture.out.state.request == fixture.original);
    CHECK(fixture.out.state.fingerprint == "original-fingerprint");
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next", "stop", "start:original"}));
    CHECK(fixture.Safe());
    return true;
}

bool FailedCleanupRetainsNewUncertainSession() {
    Fixture fixture;
    fixture.validate = [] { return false; };
    fixture.daemon->client->stops = {{true, {}}, {false, {}}, {false, {}}};
    fixture.Apply();
    CHECK(!fixture.out.succeeded);
    CHECK(fixture.out.state.daemon == fixture.daemon);
    CHECK(fixture.out.state.enabled && fixture.out.state.uncertain);
    CHECK(fixture.out.state.request == fixture.next);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "start:next", "stop", "stop"}));
    CHECK(fixture.Safe());
    return true;
}

bool UnstartedDaemonCanBeDiscardedWithoutRpc() {
    Fixture fixture(false);
    fixture.daemon->client->stops.push_back({false, {}});
    QString error;
    CHECK(NekoGui::TunLifecycle::DiscardUnstarted(fixture.out.state, error));
    CHECK(!fixture.out.state.daemon && !fixture.daemon->process->running);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "wait", "terminate"}));
    CHECK(fixture.Safe());
    return true;
}

bool ConfirmedStopAllowsOwnedTermination() {
    Fixture fixture;
    fixture.daemon->client->exitSucceeds = false;
    QString error;
    CHECK(Stop(fixture.out.state, true, error));
    CHECK(!fixture.out.state.daemon && !fixture.daemon->process->running);
    CHECK((fixture.daemon->process->events == QStringList{"stop", "exit", "wait", "terminate"}));
    CHECK(fixture.Safe());
    return true;
}

bool FailedTerminationKeepsHandleForRetry() {
    Fixture fixture;
    fixture.daemon->client->exitSucceeds = false;
    fixture.daemon->process->terminateSucceeds = false;
    QString error;
    CHECK(!Stop(fixture.out.state, true, error));
    CHECK(fixture.out.state.daemon == fixture.daemon && fixture.daemon->process->running);
    CHECK(!fixture.out.state.enabled && !fixture.out.state.uncertain);
    fixture.daemon->process->terminateSucceeds = true;
    CHECK(Stop(fixture.out.state, true, error));
    CHECK(!fixture.out.state.daemon && !fixture.daemon->process->running);
    CHECK(fixture.Safe());
    return true;
}

bool RawDiagnosticsAreNotExposed() {
    Fixture fixture(false);
    fixture.daemon->client->starts.push_back({true, "fixture: https://dns.invalid?password=synthetic-secret"});
    fixture.Apply();
    CHECK(!fixture.out.succeeded && !fixture.out.state.daemon);
    CHECK(!fixture.out.notice.contains("synthetic-secret"));
    CHECK(!fixture.out.notice.contains("dns.invalid"));
    CHECK(!fixture.out.notice.isEmpty());
    CHECK(fixture.Safe());
    return true;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const std::pair<const char *, bool (*)()> tests[] = {
        {"fresh start", FreshStart},
        {"replace keeps daemon", ReplaceKeepsOwnedDaemon},
        {"daemon exits during Stop", ExitDuringStopCannotStartClearedState},
        {"daemon exits after Start acknowledgement", ExitAfterStartAcknowledgementCannotReportSuccess},
        {"unacknowledged Stop retains ownership", UnacknowledgedStopKeepsOwnership},
        {"repeated Stop errors never permit Exit", RepeatedStopErrorNeverExitsProcess},
        {"restore exact request and clear uncertainty", FailedStartRestoresExactRequestAndClearsUncertainty},
        {"reconcile lost Start reply before rollback", LostStartReplyIsReconciledBeforeRollback},
        {"do not restore daemon that exited during cleanup", ExitDuringFailedStartCleanupCannotRestoreDeadDaemon},
        {"failed Start and rollback clean up", FailedStartAndRollbackCleanUpOwnedSession},
        {"cancel after Start", CancelAfterStartClosesWithoutRestoring},
        {"changed settings restore original", ChangedSettingsRestoreOriginal},
        {"failed cleanup keeps uncertain session", FailedCleanupRetainsNewUncertainSession},
        {"discard unstarted daemon without RPC", UnstartedDaemonCanBeDiscardedWithoutRpc},
        {"confirmed Stop permits owned termination", ConfirmedStopAllowsOwnedTermination},
        {"failed termination retains handle", FailedTerminationKeepsHandleForRetry},
        {"redact raw diagnostics", RawDiagnosticsAreNotExposed},
    };
    int failed = 0;
    for (const auto &test : tests) {
        if (!test.second()) {
            ++failed;
            qCritical() << "FAIL:" << test.first;
        } else qInfo() << "PASS:" << test.first;
    }
    qInfo() << "TUN lifecycle:" << (std::size(tests) - failed) << "passed," << failed << "failed";
    return failed ? 1 : 0;
}
