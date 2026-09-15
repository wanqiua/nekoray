#include "sys/SystemProxyLease.hpp"
#include <QCoreApplication>
#include <QtTest>
#include <functional>

using namespace NekoGui_sys;

class MemoryProxyBackend : public SystemProxyBackend {
public:
    QMap<QString, SystemProxyState> states;
    QString readFailure, writeFailure;
    bool enumerationFailure = false;
    int writes = 0;
    int reads = 0;
    QSet<int> failReads;
    std::function<bool(const QString &, const SystemProxyState &, QString &)> writeHook;
    bool Connections(QStringList &connections, QString &error) override {
        if (enumerationFailure) { error = "enumeration failure"; return false; }
        connections = states.keys();
        return true;
    }
    bool Read(const QString &connection, SystemProxyState &state, QString &error) override {
        if (failReads.contains(++reads)) { error = "readback failure"; return false; }
        if (!readFailure.isEmpty() && connection == readFailure) { error = "read failure"; return false; }
        state = states[connection];
        return true;
    }
    bool Write(const QString &connection, const SystemProxyState &state, QString &error) override {
        ++writes;
        if (writeHook) return writeHook(connection, state, error);
        if (!writeFailure.isEmpty() && connection == writeFailure) { error = "write failure"; return false; }
        states[connection] = state;
        return true;
    }
};

class SystemProxyLeaseTest : public QObject {
    Q_OBJECT
private slots:
    void restoresPacProxyAndBypass() {
        MemoryProxyBackend backend;
        backend.states[""] = {13, "old-proxy:8080", "<local>;*.example.test", "https://example.test/proxy.pac"};
        backend.states["vpn"] = {3, "vpn-proxy:3128", "localhost", ""};
        const auto before = backend.states;
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(lease.Apply("127.0.0.1:2080", error));
        QCOMPARE(backend.states[""].server, QString("127.0.0.1:2080"));
        QCOMPARE(backend.states[""].bypass, before[""].bypass);
        QVERIFY(lease.Restore(error));
        QVERIFY(backend.states == before);
        QVERIFY(!lease.HasPendingRestore());
    }
    void leavesLaterChangesIntact() {
        MemoryProxyBackend backend;
        backend.states[""] = {};
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(lease.Apply("127.0.0.1:2080", error));
        backend.states[""] = {3, "other-instance:8080", "changed", ""};
        const auto later = backend.states;
        QVERIFY(lease.Restore(error));
        QVERIFY(backend.states == later);
    }
    void doesNotWriteWithoutCompleteSnapshot() {
        MemoryProxyBackend backend;
        backend.states[""] = {};
        backend.states["vpn"] = {};
        backend.readFailure = "vpn";
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(!lease.Apply("127.0.0.1:2080", error));
        QCOMPARE(backend.writes, 0);
        QVERIFY(!lease.HasPendingRestore());
    }
    void rollsBackPartialApply() {
        MemoryProxyBackend backend;
        backend.states[""] = {5, "", "local", "https://example.test/pac"};
        backend.states["vpn"] = {};
        const auto before = backend.states;
        backend.writeFailure = "vpn";
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(!lease.Apply("127.0.0.1:2080", error));
        QVERIFY(!error.isEmpty());
        QVERIFY(backend.states == before);
        QVERIFY(!lease.HasPendingRestore());
    }
    void failedRestoreCanBeRetried() {
        MemoryProxyBackend backend;
        backend.states["vpn"] = {1, "", "", ""};
        const auto before = backend.states;
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(lease.Apply("127.0.0.1:2080", error));
        backend.writeFailure = "vpn";
        QVERIFY(!lease.Restore(error));
        QVERIFY(lease.HasPendingRestore());
        backend.writeFailure.clear();
        QVERIFY(lease.Restore(error));
        QVERIFY(backend.states == before);
    }
    void inactiveInstanceDoesNotClearProxy() {
        MemoryProxyBackend backend;
        backend.states[""] = {3, "other-instance:8080", "", ""};
        const auto before = backend.states;
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(lease.Restore(error));
        QCOMPARE(backend.writes, 0);
        QVERIFY(backend.states == before);
    }
    void failedPartialWriteIsRolledBack() {
        MemoryProxyBackend backend;
        backend.states[""] = {13, "old-proxy:8080", "<local>", "https://example.test/pac"};
        const auto before = backend.states;
        backend.writeHook = [&](const QString &connection, const SystemProxyState &state, QString &error) {
            if (backend.writes == 1) {
                backend.states[connection].flags = state.flags;
                error = "flags changed but server write failed";
                return false;
            }
            backend.states[connection] = state;
            return true;
        };
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(!lease.Apply("127.0.0.1:2080", error));
        QVERIFY(!error.isEmpty());
        QVERIFY(backend.states == before);
        QCOMPARE(backend.writes, 2);
        QVERIFY(!lease.HasPendingRestore());
    }
    void partialRestoreRetainsOwnershipUntilConfirmed() {
        MemoryProxyBackend backend;
        backend.states[""] = {13, "old-proxy:8080", "<local>", "https://example.test/pac"};
        const auto before = backend.states;
        backend.writeHook = [&](const QString &connection, const SystemProxyState &state, QString &error) {
            if (backend.writes == 2) {
                backend.states[connection].flags = state.flags;
                error = "partial restore";
                return false;
            }
            backend.states[connection] = state;
            return true;
        };
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(lease.Apply("127.0.0.1:2080", error));
        QVERIFY(!lease.Restore(error));
        QVERIFY(lease.HasPendingRestore());
        QCOMPARE(backend.states[""].server, QString("127.0.0.1:2080"));
        QVERIFY(lease.Restore(error));
        QVERIFY(backend.states == before);
        QVERIFY(!lease.HasPendingRestore());
    }
    void unconfirmedApplyDoesNotOverwriteUnrelatedChanges() {
        MemoryProxyBackend backend;
        backend.states[""] = {13, "old-proxy:8080", "<local>", "https://example.test/pac"};
        const auto before = backend.states;
        backend.writeHook = [&](const QString &connection, const SystemProxyState &state, QString &error) {
            backend.states[connection].flags = state.flags;
            backend.states[connection].autoConfigUrl = "https://other.example.test/pac";
            error = "concurrent change during partial write";
            return false;
        };
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(!lease.Apply("127.0.0.1:2080", error));
        QVERIFY(lease.HasPendingRestore());
        const auto after = backend.states;
        QVERIFY(!lease.Restore(error));
        QVERIFY(backend.states == after);
        QCOMPARE(backend.writes, 1);
        // Explicit recovery in Windows settings allows the application to exit.
        backend.states = before;
        QVERIFY(lease.Restore(error));
        QVERIFY(!lease.HasPendingRestore());
    }
    void failedApplyReadbackKeepsRecoveryPending() {
        MemoryProxyBackend backend;
        backend.states[""] = {};
        const auto before = backend.states;
        backend.failReads = {2, 3}; // apply readback and its immediate rollback
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(!lease.Apply("127.0.0.1:2080", error));
        QVERIFY(lease.HasPendingRestore());
        QVERIFY(lease.Restore(error));
        QVERIFY(backend.states == before);
    }
    void successfulWriteStillRequiresReadback() {
        MemoryProxyBackend backend;
        backend.states[""] = {};
        const auto before = backend.states;
        backend.writeHook = [](const QString &, const SystemProxyState &, QString &) { return true; };
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(!lease.Apply("127.0.0.1:2080", error));
        QVERIFY(!error.isEmpty());
        QVERIFY(backend.states == before);
        QVERIFY(!lease.HasPendingRestore());
    }
    void failedRestoreReadbackCanBeRetried() {
        MemoryProxyBackend backend;
        backend.states[""] = {};
        const auto before = backend.states;
        SystemProxyLease lease(backend);
        QString error;
        QVERIFY(lease.Apply("127.0.0.1:2080", error));
        backend.failReads.insert(backend.reads + 2); // restore write readback
        QVERIFY(!lease.Restore(error));
        QVERIFY(lease.HasPendingRestore());
        QVERIFY(backend.states == before);
        const int writes = backend.writes;
        QVERIFY(lease.Restore(error));
        QCOMPARE(backend.writes, writes); // already restored; no second write
        QVERIFY(!lease.HasPendingRestore());
    }
};

QTEST_GUILESS_MAIN(SystemProxyLeaseTest)
#include "system_proxy_lease_test.moc"
