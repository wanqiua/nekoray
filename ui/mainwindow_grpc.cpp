#include "./ui_mainwindow.h"
#include "mainwindow.h"
#include "CoreRuntime.hpp"

#include "db/Database.hpp"
#include "db/ConfigBuilder.hpp"
#include "db/traffic/TrafficLooper.hpp"
#include "rpc/gRPC.h"
#include "ui/widget/MessageBoxTimer.h"
#include "SpeedtestBatch.hpp"

#include <QTimer>
#include <QThread>
#include <QInputDialog>
#include <QPushButton>
#include <QDesktopServices>
#include <QMessageBox>
#include <QDialogButtonBox>

// ext core

std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> CreateExtCFromExtR(const std::list<std::shared_ptr<NekoGui_fmt::ExternalBuildResult>> &extRs, bool start) {
    // plz run and start in same thread
    std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> l;
    for (const auto &extR: extRs) {
        std::shared_ptr<NekoGui_sys::ExternalProcess> extC(new NekoGui_sys::ExternalProcess());
        extC->tag = extR->tag;
        extC->program = extR->program;
        extC->arguments = extR->arguments;
        extC->env = extR->env;
        l.emplace_back(extC);
        //
        if (start) extC->Start();
    }
    return l;
}

// grpc

#ifndef NKR_NO_GRPC
using namespace NekoGui_rpc;
#endif

void MainWindow::setup_grpc() {
    core_manager = new NekoGui_sys::CoreManager(this);
#ifndef NKR_NO_GRPC
    const auto initial = core_manager->Get(NekoGui::PreferredCore());
    defaultClient = initial->client.get(); // Legacy update API only; sessions/tests use explicit clients.
    NekoGui::dataStore->core_port = initial->port;
    NekoGui::dataStore->core_token = initial->token;
    core_manager->onExited = [this](int core, quint64 exitedGeneration) {
        if (NekoGui::dataStore->prepare_exit || core_switch_busy || !core_session ||
            core_session->instance->core != core || core_session->generation != exitedGeneration) return;
        const auto saved = core_session;
        const auto now = QTime::currentTime();
        if (last_crashed_core == core && !last_core_crash.isNull() && last_core_crash.msecsTo(now) >= 0 && last_core_crash.msecsTo(now) < 10000) {
            core_notice_message(tr("The core repeatedly exited. Disconnected; retry when ready."));
            neko_stop(true);
            return;
        }
        last_crashed_core = core;
        last_core_crash = now;
        core_notice_message(tr("%1 exited; restoring the saved connection.").arg(NekoGui_sys::CoreManager::Name(core)));
        auto intent = std::make_shared<NekoGui::CoreSwitchIntent>();
        intent->sequence = ++core_intent_sequence;
        intent->profile = saved->profile;
        intent->recovery = saved;
        intent->crash = true;
        pending_core_intent = intent;
        process_core_intent();
    };
    runOnNewThread([] { NekoGui_traffic::trafficLooper->Loop(); });
    const int remember = NekoGui::dataStore->remember_enable ? NekoGui::dataStore->remember_id : -1;
    ++core_background_jobs;
    runOnNewThread([this, initial, remember] {
        QString error;
        const bool ready = core_manager->EnsureReady(initial->core, error);
        runOnUiThread([this, ready, error, remember] {
            --core_background_jobs;
            if (!ready) core_notice_message(error);
            if (ready && remember >= 0 && !NekoGui::dataStore->prepare_exit) neko_start(remember);
            refresh_proxy_list();
            refresh_core_controls();
        }, this);
    });
#endif
}

// 测速

// Accessed on the GUI thread; workers retain their own shared reference.
inline std::shared_ptr<SpeedtestBatch> speedtest_batch;

void MainWindow::speedtest_current_group(int mode, int coreOverride, bool compare) {
    if (mode == 114514) {
        if (current_test_cancel) current_test_cancel->store(true);
        if (speedtest_batch) {
            speedtest_batch->cancel();
            core_notice_message(tr("Test cancelled. Previous results are kept."));
        }
        return;
    }
#ifndef NKR_NO_GRPC
    if (NekoGui::dataStore->prepare_exit) return;
    if (speedtest_batch) {
        core_notice_message(tr("The previous test is finishing. You can cancel it from the test menu."));
        return;
    }
    const auto profiles = get_selected_or_group();
    if (profiles.isEmpty() || NekoGui::profileManager->CurrentGroup()->archive) return;
    QStringList flags;
    if (mode == libcore::FullTest) {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Test Options"));
        auto layout = new QVBoxLayout(&dialog);
        QList<QCheckBox *> options;
        for (const auto &label : {tr("Latency"), tr("UDP latency"), tr("Download speed"), tr("In and Out IP")}) {
            auto option = new QCheckBox(label, &dialog);
            layout->addWidget(option);
            options.append(option);
        }
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        for (int n = 0; n < options.size(); ++n) if (options[n]->isChecked()) flags.append(QString::number(n + 1));
        if (flags.isEmpty()) return;
    }
    if (NekoGui::dataStore->prepare_exit || speedtest_batch) return;
    struct Work {
        std::shared_ptr<NekoGui::ProxyEntity> profile;
        int forcedCore = -1;
    };
    QVector<Work> work;
    int skipped = 0;
    for (const auto &profile : profiles) {
        const auto resolution = NekoGui::ResolveCore(profile, NekoGui::PreferredCore());
        if (compare) {
            if (!resolution.xraySupported || !resolution.singBoxSupported) { ++skipped; continue; }
            work.append({profile, NekoGui::CoreType::V2RAY});
            work.append({profile, NekoGui::CoreType::SING_BOX});
        } else if (coreOverride >= 0) {
            if (!resolution.Supports(coreOverride)) { ++skipped; continue; }
            work.append({profile, coreOverride});
        } else work.append({profile, -1});
    }
    if (skipped) core_notice_message(tr("%1 node(s) do not support the requested comparison.").arg(skipped));
    if (work.isEmpty()) return;
    auto batch = std::make_shared<SpeedtestBatch>(int(work.size()), NekoGui::dataStore->test_concurrent);
    speedtest_batch = batch;
    const auto latencyURL = NekoGui::dataStore->test_latency_url;
    const auto downloadURL = mode == libcore::FullTest ? NekoGui::dataStore->test_download_url : QString();
    const int timeout = mode == libcore::TcpPing ? 3000 : qBound(1000, NekoGui::dataStore->test_latency_timeout, 60000);
    const int downloadTimeout = mode == libcore::FullTest ? NekoGui::dataStore->test_download_timeout : 0;
    const QPointer<MainWindow> guard(this);

    ++core_background_jobs;
    runOnNewThread([this, guard, work, mode, flags, batch, latencyURL, downloadURL, timeout, downloadTimeout] {
        try {
            // This one queue is the total budget across both core families.
            batch->run([&] {
                std::array<std::unique_ptr<TestClient>, 2> clients;
                int index;
                while (batch->next(index)) {
                    const auto task = work.at(index);
                    const auto original = task.profile;
                    const int id = original->id;
                    int core = -1;
                    quint64 preferenceRevision = 0, processGeneration = 0;
                    QString displayName, sourceFingerprint;
                    QByteArray beanSnapshot;
                    bool valid = false;
                    std::shared_ptr<NekoGui::BuildConfigResult> built, alternateBuilt;
                    libcore::TestReq alternateRequest;
                    QString alternateFingerprint;
                    int alternateCore = -1;
                    libcore::TestReq request;
                    request.set_mode(static_cast<libcore::TestMode>(mode));
                    request.set_timeout(timeout);
                    request.set_url(latencyURL.toStdString());
                    request.set_full_latency(flags.contains("1"));
                    request.set_full_udp_latency(flags.contains("2"));
                    request.set_full_speed(flags.contains("3"));
                    request.set_full_in_out(flags.contains("4"));
                    request.set_full_speed_url(downloadURL.toStdString());
                    request.set_full_speed_timeout(downloadTimeout);
                    NekoGui::ProfileTestResult record;
                    record.mode = mode == libcore::TcpPing ? "tcp" : mode == libcore::FullTest ? "full" : "url";
                    if (mode == libcore::FullTest) record.parameters = {{"latency", flags.contains("1")}, {"udp_latency", flags.contains("2")},
                                                                      {"speed", flags.contains("3")}, {"in_out", flags.contains("4")}};

                    QSemaphore prepared;
                    runOnUiThread([&] {
                        auto profile = NekoGui::profileManager->GetProfile(id);
                        if (guard && !batch->cancelled() && profile == original) {
                            valid = true;
                            displayName = profile->bean->DisplayTypeAndName();
                            beanSnapshot = profile->bean->ToJsonBytes();
                            preferenceRevision = preference_revisions.value(id);
                            const auto resolution = NekoGui::ResolveCore(profile, NekoGui::PreferredCore());
                            core = task.forcedCore >= 0 ? task.forcedCore : resolution.targetCore;
                            if (core < 0 || !resolution.Supports(core)) {
                                record.status = NekoGui::TestResultStatus::Unsupported;
                                record.error = core < 0 ? resolution.reason : core == 0 ? resolution.xrayReason : resolution.singBoxReason;
                            } else {
                                auto context = NekoGui::CaptureBuildConfigContext(core, true);
                                context.tunProxyPort = tun_proxy_port;
                                sourceFingerprint = NekoGui::CoreTestInputDigest(profile, context, mode, latencyURL, timeout, downloadURL, downloadTimeout, record.parameters);
                                if (mode == libcore::TcpPing) {
                                    request.set_address(profile->bean->DisplayAddress().toStdString());
                                    record.generated_config_digest = NekoGui::CoreConfigDigest({{"address", profile->bean->DisplayAddress()}});
                                } else {
                                    built = NekoGui::BuildConfig(profile, true, false, context);
                                    request.set_in_address(profile->bean->serverAddress.toStdString());
                                    record.generated_config_digest = QString::fromLatin1(built->Fingerprint());
                                    if (!built->error.isEmpty()) {
                                        record.status = NekoGui::TestResultStatus::Unsupported;
                                        record.error = built->error;
                                    } else {
                                        request.mutable_config()->set_core_config(QJsonObject2QString(built->coreConfig, true).toStdString());
                                        if (task.forcedCore < 0 && resolution.Supports(1 - core)) {
                                            auto alternateContext = NekoGui::CaptureBuildConfigContext(1 - core, true);
                                            alternateContext.tunProxyPort = tun_proxy_port;
                                            alternateBuilt = NekoGui::BuildConfig(profile, true, false, alternateContext);
                                            if (alternateBuilt->error.isEmpty()) {
                                                alternateCore = 1 - core;
                                                alternateRequest = request;
                                                alternateRequest.mutable_config()->set_core_config(QJsonObject2QString(alternateBuilt->coreConfig, true).toStdString());
                                                alternateFingerprint = NekoGui::CoreTestInputDigest(profile, alternateContext, mode, latencyURL, timeout, downloadURL, downloadTimeout, record.parameters);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        prepared.release();
                    }, this);
                    prepared.acquire();
                    if (!valid) continue;

                    std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> external;
                    const auto cleanup = [&] {
                        if (!external.empty()) {
                            QSemaphore stopped;
                            runOnUiThread([&] {
                                for (const auto &process : external) process->Kill();
                                external.clear();
                                stopped.release();
                            }, DS_cores);
                            stopped.acquire();
                        }
                        if (built) RemoveCoreHelperFiles(built->extRs);
                    };
                    for (int attempt = 0; attempt < 2; ++attempt) {
                        record.core = core;
                        record.config_fingerprint = sourceFingerprint;
                        bool localUnsupported = false;
                        if (record.error.isEmpty() && !batch->cancelled()) {
                            QString readyError;
                            if (!core_manager->EnsureReady(core, readyError)) {
                                record.status = NekoGui::TestResultStatus::CoreError;
                                record.error = readyError;
                            } else {
                                record.build_id = core_manager->Identity(core);
                                processGeneration = core_manager->Generation(core);
                                if (built) {
                                    record.error = NekoGui::CoreBuildFeaturesError(built->coreConfig, core, core_manager->Features(core));
                                    if (!record.error.isEmpty()) {
                                        record.status = NekoGui::TestResultStatus::Unsupported;
                                        localUnsupported = true;
                                    }
                                }
                                if (!clients[core]) clients[core] = core_manager->Get(core)->client->CreateTestClient();
                                if (record.error.isEmpty() && !batch->cancelled() && built && !built->extRs.empty()) {
                                    if (!MaterializeCoreHelperFiles(built->extRs, record.error)) {
                                        record.status = NekoGui::TestResultStatus::CoreError;
                                    } else {
                                        QSemaphore started;
                                        runOnUiThread([&] {
                                            if (!batch->cancelled()) external = CreateExtCFromExtR(built->extRs, false);
                                            for (const auto &process : external) {
                                                if (batch->cancelled()) break;
                                                process->managed = false;
                                                auto raw = process.get();
                                                QObject::connect(raw, &QProcess::readyReadStandardOutput, raw, [raw] { raw->readAllStandardOutput(); });
                                                QObject::connect(raw, &QProcess::readyReadStandardError, raw, [raw] { raw->readAllStandardError(); });
                                                process->Start();
                                                if (!process->waitForStarted(3000)) {
                                                    record.status = NekoGui::TestResultStatus::CoreError;
                                                    record.error = tr("External test helper could not start.");
                                                    break;
                                                }
                                            }
                                            started.release();
                                        }, DS_cores);
                                        started.acquire();
                                    }
                                }
                                if (record.error.isEmpty() && !batch->cancelled()) {
                                    bool rpcOK = false;
                                    const auto response = clients[core]->Test(&rpcOK, request, batch->cancellation());
                                    record.latency_ms = response.ms();
                                    record.error = QString::fromStdString(response.error());
                                    record.report = QString::fromStdString(response.full_report());
                                    if (!rpcOK) record.status = batch->cancelled() ? NekoGui::TestResultStatus::Cancelled : NekoGui::TestResultStatus::CoreError;
                                    else if (response.error().empty()) record.status = NekoGui::TestResultStatus::Success;
                                    else switch (response.failure_category()) {
                                        case libcore::FailureRemote: record.status = NekoGui::TestResultStatus::ConnectionFailure; break;
                                        case libcore::FailureUnsupported:
                                            localUnsupported = true;
                                            record.status = NekoGui::TestResultStatus::Unsupported;
                                            break;
                                        case libcore::FailureConfig: record.status = NekoGui::TestResultStatus::Unsupported; break;
                                        case libcore::FailureCancelled: record.status = NekoGui::TestResultStatus::Cancelled; break;
                                        default: record.status = NekoGui::TestResultStatus::CoreError; break;
                                    }
                                }
                            }
                        }
                        if (record.build_id.isEmpty() && core >= 0) record.build_id = core_manager->Identity(core);
                        cleanup();
                        // A local missing feature may select the other compatible
                        // adapter. Remote failures never retry on a different core.
                        if (attempt != 0 || !localUnsupported || alternateCore < 0 || batch->cancelled()) break;
                        core = alternateCore;
                        built = alternateBuilt;
                        request = alternateRequest;
                        sourceFingerprint = alternateFingerprint;
                        record.error.clear();
                        record.report.clear();
                        record.build_id.clear();
                        record.status = NekoGui::TestResultStatus::Untested;
                        record.generated_config_digest = QString::fromLatin1(built->Fingerprint());
                    }
                    QSemaphore saved;
                    runOnUiThread([&] {
                        auto profile = NekoGui::profileManager->GetProfile(id);
                        if (guard && profile == original) {
                            const auto nowResolution = NekoGui::ResolveCore(profile, NekoGui::PreferredCore());
                            const bool stale = speedtest_batch != batch || profile->bean->ToJsonBytes() != beanSnapshot || preference_revisions.value(id) != preferenceRevision ||
                                               (core >= 0 && test_fingerprint(profile, core, mode, record.parameters) != sourceFingerprint) ||
                                               (processGeneration && core_manager->Generation(core) != processGeneration) ||
                                               (task.forcedCore < 0 && nowResolution.targetCore != core);
                            if (batch->cancelled()) record.status = NekoGui::TestResultStatus::Cancelled;
                            else if (stale) record.status = NekoGui::TestResultStatus::Stale;
                            profile->RecordTestResult(record);
                            if (!stale && !batch->cancelled() && !record.error.isEmpty())
                                MW_show_log(tr("[%1] test error: %2").arg(displayName, record.error));
                            refresh_test_validity(profile);
                            profile->Save();
                            refresh_proxy_list(id);
                        }
                        saved.release();
                    }, this);
                    saved.acquire();
                    if (batch->cancelled()) break;
                }
            });
        } catch (const std::exception &error) {
            MW_show_log(QString("Batch test error: %1").arg(error.what()));
        } catch (...) { MW_show_log("Batch test error"); }
        runOnUiThread([this, guard, batch] {
            --core_background_jobs;
            if (speedtest_batch == batch) speedtest_batch.reset();
            if (guard) refresh_core_controls();
        }, this);
    });
#endif
}

void MainWindow::speedtest_current() {
#ifndef NKR_NO_GRPC
    const auto session = core_session;
    if (NekoGui::dataStore->prepare_exit) return;
    if (!session || core_switch_busy || !session->instance->ready || !session->instance->alive || !session->externalHealthy->load()) {
        core_notice_message(tr("Connect a node before testing the current connection."));
        return;
    }
    last_test_time = QTime::currentTime();
    ui->label_running->setText(tr("Testing current connection…"));
    const auto url = NekoGui::dataStore->test_latency_url;
    const int timeout = qBound(1000, NekoGui::dataStore->test_latency_timeout, 60000);
    const auto identity = core_manager->Identity(session->instance->core);
    if (current_test_cancel) current_test_cancel->store(true);
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    current_test_cancel = cancelled;
    const QPointer<MainWindow> guard(this);
    ++core_background_jobs;
    runOnNewThread([this, guard, session, url, timeout, identity, cancelled] {
        libcore::TestReq request;
        request.set_mode(libcore::UrlTest);
        request.set_timeout(timeout);
        request.set_url(url.toStdString()); // No config: use this daemon's active saved session.
        bool ok = false;
        auto client = session->instance->client->CreateTestClient();
        const auto response = client->Test(&ok, request, cancelled.get());
        runOnUiThread([this, guard, session, response, ok, identity, url, timeout, cancelled] {
            if (!guard) return;
            --core_background_jobs;
            if (current_test_cancel == cancelled) current_test_cancel.reset();
            NekoGui::ProfileTestResult record;
            record.core = session->instance->core;
            record.scope = "current";
            record.build_id = identity;
            record.config_fingerprint = NekoGui::CoreConfigDigest({{"session", session->fingerprint}, {"url", url}, {"timeout", timeout}});
            record.generated_config_digest = QString::fromLatin1(session->built->Fingerprint());
            record.latency_ms = response.ms();
            record.error = QString::fromStdString(response.error());
            record.status = !ok ? NekoGui::TestResultStatus::CoreError
                            : response.error().empty() ? NekoGui::TestResultStatus::Success
                            : response.failure_category() == libcore::FailureRemote ? NekoGui::TestResultStatus::ConnectionFailure
                            : (response.failure_category() == libcore::FailureUnsupported || response.failure_category() == libcore::FailureConfig)
                                  ? NekoGui::TestResultStatus::Unsupported
                            : NekoGui::TestResultStatus::CoreError;
            if (core_session != session || session->generation != core_manager->Generation(record.core)) record.status = NekoGui::TestResultStatus::Stale;
            if (cancelled->load()) record.status = NekoGui::TestResultStatus::Cancelled;
            if (NekoGui::profileManager->GetProfile(session->profile->id) == session->profile) {
                session->profile->RecordTestResult(record);
                session->profile->Save();
            }
            if (record.status == NekoGui::TestResultStatus::Stale || record.status == NekoGui::TestResultStatus::Cancelled || NekoGui::dataStore->prepare_exit) return;
            last_test_time = QTime::currentTime();
            ui->label_running->setText(tr("Current connection: %1 · %2")
                                          .arg(record.status == NekoGui::TestResultStatus::Success ? QString("%1 ms").arg(record.latency_ms)
                                                   : record.status == NekoGui::TestResultStatus::Unsupported ? tr("不适用") : tr("Test failed"),
                                               NekoGui_sys::CoreManager::Name(record.core)));
            if (!record.error.isEmpty()) MW_show_log(record.error);
        }, this);
    });
#endif
}

void MainWindow::stop_core_daemon() {
    // Called from an exit worker while Qt still processes completion callbacks.
    // Do not destroy clients/processes until all captured test jobs have drained.
    while (core_background_jobs.load() > 0) QThread::msleep(10);
    if (core_manager) core_manager->Shutdown();
}

void MainWindow::CheckUpdate() {
    // on new thread...
#ifndef NKR_NO_GRPC
    bool ok;
    libcore::UpdateReq request;
    request.set_action(libcore::UpdateAction::Check);
    request.set_check_pre_release(NekoGui::dataStore->check_include_pre);
    auto response = NekoGui_rpc::defaultClient->Update(&ok, request);
    if (!ok) return;

    auto err = response.error();
    if (!err.empty()) {
        runOnUiThread([=] {
            MessageBoxWarning(QObject::tr("Update"), err.c_str());
        });
        return;
    }

    if (response.release_download_url() == nullptr) {
        runOnUiThread([=] {
            MessageBoxInfo(QObject::tr("Update"), QObject::tr("No update"));
        });
        return;
    }

    runOnUiThread([=] {
        auto allow_updater = !NekoGui::dataStore->flag_use_appdata;
        auto note_pre_release = response.is_pre_release() ? " (Pre-release)" : "";
        QMessageBox box(QMessageBox::Question, QObject::tr("Update") + note_pre_release,
                        QObject::tr("Update found: %1\nRelease note:\n%2").arg(response.assets_name().c_str(), response.release_note().c_str()));
        //
        QAbstractButton *btn1 = nullptr;
        if (allow_updater) {
            btn1 = box.addButton(QObject::tr("Update"), QMessageBox::AcceptRole);
        }
        QAbstractButton *btn2 = box.addButton(QObject::tr("Open in browser"), QMessageBox::AcceptRole);
        box.addButton(QObject::tr("Close"), QMessageBox::RejectRole);
        box.exec();
        //
        if (btn1 == box.clickedButton() && allow_updater) {
            // Download Update
            runOnNewThread([=] {
                bool ok2;
                libcore::UpdateReq request2;
                request2.set_action(libcore::UpdateAction::Download);
                auto response2 = NekoGui_rpc::defaultClient->Update(&ok2, request2);
                runOnUiThread([=] {
                    if (response2.error().empty()) {
                        auto q = QMessageBox::question(nullptr, QObject::tr("Update"),
                                                       QObject::tr("Update is ready, restart to install?"));
                        if (q == QMessageBox::StandardButton::Yes) {
                            this->exit_reason = 1;
                            on_menu_exit_triggered();
                        }
                    } else {
                        MessageBoxWarning(QObject::tr("Update"), response2.error().c_str());
                    }
                });
            });
        } else if (btn2 == box.clickedButton()) {
            QDesktopServices::openUrl(QUrl(response.release_url().c_str()));
        }
    });
#endif
}
