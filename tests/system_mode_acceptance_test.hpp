// Opt-in system acceptance, excluded from ordinary local regression runs.
// Only synthetic SOCKS/HTTP/DNS peers on loopback are used. Reserved destination
// addresses ensure the requests really traverse the Windows TUN interface.
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QUdpSocket>
#include <QTextDocument>
#include "system_mode_fixture.hpp"
#include "fmt/Preset.hpp"

namespace SystemModeAcceptance {

#ifdef Q_OS_WIN
inline std::pair<DWORD, QString> NativeSystemProxy() {
    INTERNET_PER_CONN_OPTIONW options[2]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof(list);
    list.dwOptionCount = 2;
    list.pOptions = options;
    DWORD size = sizeof(list);
    const bool ok = InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size);
    const auto server = ok ? QString::fromWCharArray(options[1].Value.pszValue) : QString{};
    if (options[1].Value.pszValue) GlobalFree(options[1].Value.pszValue);
    return {ok ? options[0].Value.dwValue : 0, server};
}

inline DWORD NativeSystemProxyRequest(const std::wstring &headers) {
    // A fresh native session avoids Qt's process-wide proxy-discovery cache.
    const auto session = InternetOpenW(L"Nekoray local acceptance", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!session) return 0;
    auto closeSession = qScopeGuard([&] { InternetCloseHandle(session); });
    DWORD timeout = 5000;
    for (DWORD option : {DWORD(INTERNET_OPTION_CONNECT_TIMEOUT), DWORD(INTERNET_OPTION_SEND_TIMEOUT), DWORD(INTERNET_OPTION_RECEIVE_TIMEOUT)})
        InternetSetOptionW(session, option, &timeout, sizeof(timeout));
    const auto request = InternetOpenUrlW(session, L"http://203.0.113.33/acceptance", headers.c_str(), DWORD(headers.size()),
                                          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES, 0);
    if (!request) return 0;
    auto closeRequest = qScopeGuard([&] { InternetCloseHandle(request); });
    DWORD code = 0, size = sizeof(code);
    HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &size, nullptr);
    return code;
}
#endif

} // namespace SystemModeAcceptance

void GuiRegressionTest::socksUdpFixtureSanity() {
    const auto error = SystemModeAcceptance::SocksUdpFixtureSanity();
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void GuiRegressionTest::systemProxyRuntimeAcceptance() {
    if (qEnvironmentVariable("NEKORAY_SYSTEM_ACCEPTANCE") != "1")
        QSKIP("opt-in Windows system proxy acceptance; use system_host_acceptance.py");
#ifndef Q_OS_WIN
    QSKIP("Windows acceptance only");
#else
    FixtureServer peer;
    const auto marker = QString("X-Nekoray-Acceptance: %1\r\n").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    peer.requestMarker = marker.toLatin1();
    auto profile = addNode(peer.serverPort());
    profile->core_preference = 1;
    window->neko_start(profile->id);
    QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
    auto restore = qScopeGuard([&] { window->neko_set_spmode_system_proxy(false, false); });
    const auto auditPath = qEnvironmentVariable("NEKORAY_SYSTEM_PROXY_AUDIT");
    QVERIFY2(!auditPath.isEmpty(), "system proxy audit path must be supplied by the host acceptance guardian");
    auto proxyServer = NekoGui::dataStore->system_proxy_format;
    if (proxyServer.isEmpty()) proxyServer = Preset::Windows::system_proxy_format[0];
    proxyServer.replace("{ip}", "127.0.0.1")
               .replace("{http_port}", QString::number(NekoGui::dataStore->inbound_http_port))
               .replace("{socks_port}", QString::number(NekoGui::dataStore->inbound_socks_port));
    QFile audit(auditPath);
    QVERIFY(audit.open(QIODevice::WriteOnly));
    QVERIFY(audit.write(QJsonDocument(QJsonObject{{"servers", QJsonArray{proxyServer}}}).toJson()) > 0);
    audit.close();
    window->neko_set_spmode_system_proxy(true, false);
    QVERIFY(NekoGui::dataStore->spmode_system_proxy);
    for (int preference : {1, 2, 1}) {
        window->set_core_preference(profile->id, preference, true);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy && window->active_core() == preference - 1, 12000);
        const auto actual = SystemModeAcceptance::NativeSystemProxy();
        QCOMPARE(actual.first, DWORD(PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY));
        QCOMPARE(actual.second, proxyServer);
        auto request = std::async(std::launch::async, SystemModeAcceptance::NativeSystemProxyRequest, marker.toStdWString());
        QTRY_VERIFY_WITH_TIMEOUT(request.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
        QCOMPARE(request.get(), DWORD(204));
    }
    QCOMPARE(peer.matchingRequests, 3);
    window->neko_set_spmode_system_proxy(false, false);
    QVERIFY(!NekoGui::dataStore->spmode_system_proxy);
#endif
}

void GuiRegressionTest::tunSystemRuntimeAcceptance() {
    if (qEnvironmentVariable("NEKORAY_SYSTEM_ACCEPTANCE") != "1")
        QSKIP("opt-in Windows TUN acceptance; use system_host_acceptance.py");
#ifndef Q_OS_WIN
    QSKIP("Windows acceptance only");
#else
    using namespace SystemModeAcceptance;
    qInfo("TUN acceptance: entered fixture setup");
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer progress;
    progress.setInterval(10000);
    connect(&progress, &QTimer::timeout, this, [&] {
        qInfo().noquote() << "TUN acceptance progress: elapsed_ms=" << elapsed.elapsed()
                         << "core_switch_busy=" << window->core_switch_busy
                         << "vpn_changing=" << window->vpn_changing
                         << "vpn_enabled=" << NekoGui::dataStore->spmode_vpn
                         << "helper_pid=" << window->vpn_pid
                         << "notice=" << window->core_notice;
    });
    progress.start();
    SocksPeer peer;
    auto data = NekoGui::dataStore;
    const auto saved = data->ToJson();
    auto restore = qScopeGuard([&] {
        const bool stopped = window->StopVPNProcess();
        QCoreApplication::processEvents();
        qInfo().noquote() << "Synthetic TUN cleanup confirmed=" << stopped << peer.Diagnostics();
        qInfo().noquote() << "Synthetic GUI core diagnostic log:\n" << window->qvLogDocument->toPlainText();
        data->FromJson(saved);
    });
    window->qvLogDocument->clear();
    data->log_level = "debug";
    data->log_ignore.clear();
    data->max_log_line = 3000;
    data->vpn_ipv6 = true;
    data->vpn_strict_route = false;
    data->vpn_mtu = 1500;
    data->vpn_implementation = 0;
    data->vpn_rule_white = true;
    data->vpn_rule_process = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    // Test traffic and its explicit DNS destination only. Background system
    // traffic must not be answered by the synthetic DNS/HTTP peer.
    data->vpn_rule_cidr = "203.0.113.0/24\n2001:db8::/32\n198.18.0.0/15\nfc00::/18";
    data->fake_dns = false;
    data->routing->remote_dns = QString("udp://127.0.0.1:%1").arg(peer.dns.localPort());
    data->routing->direct_dns = data->routing->remote_dns;
    data->routing->dns_routing = false;
    data->routing->sniffing_mode = NekoGui::SniffingMode::DISABLE;
    data->routing->block_ip = "203.0.113.34/32";
    auto profile = NekoGui::ProfileManager::NewProxyEntity("socks");
    profile->bean->name = "Synthetic TUN acceptance peer";
    profile->bean->serverAddress = "127.0.0.1";
    profile->bean->serverPort = peer.serverPort();
    profile->core_preference = 1;
    NekoGui::profileManager->AddProfile(profile, baseGroup);
    qInfo("TUN acceptance: starting synthetic Xray node");
    window->neko_start(profile->id);
    QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy, 12000);
    qInfo("TUN acceptance: synthetic Xray node ready");
    if (qEnvironmentVariable("NEKORAY_UAC_DENIAL_FIRST") == "1" && !NekoGui::IsAdmin()) {
        qInfo("UAC acceptance: reject the first Windows consent prompt, then allow the next prompt.");
        window->neko_set_spmode_vpn(true, false);
        QTRY_VERIFY_WITH_TIMEOUT(!window->vpn_changing, 60000);
        if (data->spmode_vpn) {
            window->StopVPNProcess();
            QFAIL("The first UAC prompt was accepted; cancellation acceptance was not exercised.");
        }
        QVERIFY2(window->core_notice.contains(QString::fromUtf8("已取消系统授权")), qPrintable(window->core_notice));
        QCOMPARE(window->vpn_pid, qint64(0));
        qInfo("PASS: native UAC denial leaves TUN disabled and no owned helper PID");
    }
    qInfo("TUN acceptance: requesting TUN, approve the Windows consent prompt");
    window->neko_set_spmode_vpn(true, false);
    QTRY_VERIFY_WITH_TIMEOUT(!window->vpn_changing && data->spmode_vpn, 60000);
    QVERIFY(window->vpn_pid > 0);
    qInfo("TUN acceptance: elevated helper started");
    const auto helperPid = window->vpn_pid;
    for (int preference : {1, 2, 1}) {
        qInfo() << "TUN acceptance: forwarding checks, core preference=" << preference;
        window->set_core_preference(profile->id, preference, true);
        QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy && window->active_core() == preference - 1, 12000);
        QVERIFY(data->spmode_vpn);
        QCOMPARE(window->vpn_pid, helperPid);
        QVERIFY2(HttpViaTun("203.0.113.33"), "IPv4 TCP did not traverse the real TUN and selected core");
        QVERIFY2(!peer.tcpDestinations.isEmpty() && peer.tcpDestinations.last().Is("203.0.113.33", 80), qPrintable(peer.Diagnostics()));
        QVERIFY2(HttpViaTun("2001:db8::33"), "IPv6 TCP did not traverse the real TUN and selected core");
        QVERIFY2(peer.tcpDestinations.last().Is("2001:db8::33", 80), qPrintable(peer.Diagnostics()));
        const auto beforeIPv4 = peer.udpRequests;
        QCOMPARE(UdpViaTun(QHostAddress("203.0.113.33"), 19090, "tun-udp-v4", &peer), QByteArray("tun-udp-v4"));
        QVERIFY2(peer.udpRequests > beforeIPv4 && peer.udpDestinations.last().Is("203.0.113.33", 19090), qPrintable(peer.Diagnostics()));
        const auto beforeIPv6 = peer.udpRequests;
        QCOMPARE(UdpViaTun(QHostAddress("2001:db8::33"), 19090, "tun-udp-v6", &peer), QByteArray("tun-udp-v6"));
        QVERIFY2(peer.udpRequests > beforeIPv6 && peer.udpDestinations.last().Is("2001:db8::33", 19090), qPrintable(peer.Diagnostics()));
        const auto answer = UdpViaTun(QHostAddress("203.0.113.53"), 53, DnsQuery(1), &peer);
        QVERIFY2(answer.endsWith(QByteArray::fromHex("cb007121")), "TUN DNS hijack did not reach the configured DNS peer");
        const auto answer6 = UdpViaTun(QHostAddress("2001:db8::53"), 53, DnsQuery(28), &peer);
        QVERIFY2(answer6.endsWith(QByteArray::fromHex("20010db8000000000000000000000033")), "TUN IPv6 DNS hijack did not reach the configured DNS peer");
    }

    // A real target Start failure must restore forwarding through the same
    // elevated TUN helper, not merely put the previous core name back in the UI.
    const auto beforeFailure = window->core_session;
    const auto httpPort = data->inbound_http_port;
    QTcpServer occupied;
    occupied.setProxy(QNetworkProxy::NoProxy);
    QVERIFY(occupied.listen(QHostAddress::LocalHost));
    data->inbound_http_port = occupied.serverPort();
    window->set_core_preference(profile->id, 2, true);
    QTRY_VERIFY_WITH_TIMEOUT(!window->core_switch_busy, 20000);
    data->inbound_http_port = httpPort;
    QCOMPARE(window->core_session, beforeFailure);
    QCOMPARE(profile->core_preference, 1);
    QCOMPARE(window->vpn_pid, helperPid);
    QVERIFY(data->spmode_vpn);
    QVERIFY2(HttpViaTun("203.0.113.33"), "TUN forwarding was not restored after target Start failed");
    QCOMPARE(UdpViaTun(QHostAddress("203.0.113.33"), 19090, "after-rollback", &peer), QByteArray("after-rollback"));

    window->last_crashed_core = -1;
    window->last_core_crash = {};
    const auto crashedInstance = window->core_session->instance;
    const auto crashedGeneration = window->core_session->generation;
    runOnUiThread([crashedInstance] { crashedInstance->process->kill(); }, DS_cores);
    QTRY_VERIFY_WITH_TIMEOUT(window->core_session && !window->core_switch_busy &&
                                window->core_session->generation > crashedGeneration && crashedInstance->ready && crashedInstance->alive, 15000);
    QCOMPARE(window->vpn_pid, helperPid);
    QVERIFY(data->spmode_vpn);
    QVERIFY2(HttpViaTun("203.0.113.33"), "TUN forwarding did not recover after the owned proxy core crashed");
    QCOMPARE(UdpViaTun(QHostAddress("2001:db8::33"), 19090, "after-crash", &peer), QByteArray("after-crash"));

    const auto beforeBlocked = peer.tcpRequests;
    QVERIFY(!HttpViaTun("203.0.113.34", 1500));
    QCOMPARE(peer.tcpRequests, beforeBlocked);

    // Reject an ambiguous FakeIP/IP-range combination before stopping the
    // working helper. Settings retain the user's draft, not a silent rewrite.
    const auto beforeRejectedSession = window->core_session;
    const auto beforeRejectedClient = window->vpn_client;
    const auto beforeRejectedFingerprint = window->vpn_config_fingerprint;
    QVERIFY(!beforeRejectedFingerprint.isEmpty());
    data->fake_dns = true;
    const auto rejectedSettings = data->ToJson();
    const auto rejectedRouting = data->routing->ToJson();
    qInfo("TUN acceptance: rejecting incompatible FakeIP and IP ranges while preserving forwarding");
    window->reload_tun_settings();
    QVERIFY(!window->vpn_changing);
    QVERIFY(!window->core_switch_busy);
    QVERIFY(data->spmode_vpn);
    QVERIFY(window->core_notice.contains("FakeIP"));
    QCOMPARE(window->vpn_pid, helperPid);
    QCOMPARE(window->vpn_client, beforeRejectedClient);
    QCOMPARE(window->vpn_config_fingerprint, beforeRejectedFingerprint);
    QCOMPARE(window->core_session, beforeRejectedSession);
    QCOMPARE(data->ToJson(), rejectedSettings);
    QCOMPARE(data->routing->ToJson(), rejectedRouting);
    QVERIFY2(HttpViaTun("203.0.113.33"), "TUN forwarding changed after incompatible FakeIP settings were rejected");
    QCOMPARE(UdpViaTun(QHostAddress("203.0.113.33"), 19090, "after-rejected-fakeip", &peer), QByteArray("after-rejected-fakeip"));
    const auto stillRealAnswer = UdpViaTun(QHostAddress("203.0.113.53"), 53, DnsQuery(1), &peer);
    QVERIFY2(stillRealAnswer.endsWith(QByteArray::fromHex("cb007121")), "Rejected settings changed the running TUN DNS configuration");

    // Process scope continues to match after FakeIP restores the domain.
    data->vpn_rule_cidr.clear();
    qInfo("TUN acceptance: applying FakeIP with process scope");
    window->reload_tun_settings();
    QTRY_VERIFY_WITH_TIMEOUT(!window->vpn_changing && data->spmode_vpn, 20000);
    QCOMPARE(window->vpn_pid, helperPid);
    const auto fakeAnswer = UdpViaTun(QHostAddress("203.0.113.53"), 53, DnsQuery(1), &peer);
    QVERIFY(fakeAnswer.size() >= 4);
    const auto tail = fakeAnswer.right(4);
    QVERIFY2(quint8(tail[0]) == 198 && (quint8(tail[1]) == 18 || quint8(tail[1]) == 19), "TUN FakeIP DNS answer is outside the configured pool");
    const auto fakeAddress = QString("%1.%2.%3.%4").arg(quint8(tail[0])).arg(quint8(tail[1])).arg(quint8(tail[2])).arg(quint8(tail[3]));
    const auto beforeFake = peer.tcpDestinations.size();
    QVERIFY2(HttpViaTun(fakeAddress), "FakeIP destination was not restored through the SOCKS bridge");
    QCOMPARE(peer.tcpDestinations.size(), beforeFake + 1);
    const auto restoredTarget = peer.tcpDestinations.last();
    QVERIFY2(restoredTarget.Is("acceptance.example.test", 80) || restoredTarget.Is("203.0.113.33", 80) ||
                 restoredTarget.Is("2001:db8::33", 80), qPrintable("FakeIP was not restored: " + restoredTarget.Description()));
    QVERIFY2(!restoredTarget.Is(fakeAddress, 80), "The raw FakeIP address reached the SOCKS peer");
    QVERIFY(window->StopVPNProcess());
    QVERIFY(!data->spmode_vpn);
    QCOMPARE(window->vpn_pid, qint64(0));
    qInfo("PASS: real TUN IPv4/IPv6 TCP/UDP/DNS, FakeIP, rejected-FakeIP settings preserve forwarding, block rule, stable helper across two cores, failed-Start rollback, owned-core crash recovery, normal cleanup");
#endif
}
