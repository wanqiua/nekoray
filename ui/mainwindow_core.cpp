#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "CoreRuntime.hpp"
#include "db/traffic/TrafficLooper.hpp"

#include <QButtonGroup>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

using namespace NekoGui;
using NekoGui_sys::CoreManager;

namespace {
void StopSessionExternal(const std::shared_ptr<RunningCoreSession> &session) {
    if (!session) return;
    if (session->external.empty()) {
        if (session->built) RemoveCoreHelperFiles(session->built->extRs);
        return;
    }
    QSemaphore stopped;
    runOnUiThread([session, &stopped] {
        for (const auto &process : session->external) {
            process->setProperty("sessionStopping", true);
            process->Kill();
        }
        session->external.clear();
        RemoveCoreHelperFiles(session->built->extRs);
        stopped.release();
    }, DS_cores);
    stopped.acquire();
}

bool StartSessionExternal(const std::shared_ptr<RunningCoreSession> &session, QString &error) {
    if (!session || session->built->extRs.empty()) return true;
    session->externalHealthy = std::make_shared<std::atomic_bool>(true);
    if (!MaterializeCoreHelperFiles(session->built->extRs, error)) {
        RemoveCoreHelperFiles(session->built->extRs);
        return false;
    }
    QSemaphore started;
    bool ok = true;
    runOnUiThread([session, &error, &ok, &started] {
        for (const auto &result : session->built->extRs) {
            auto process = std::make_shared<NekoGui_sys::ExternalProcess>();
            process->managed = false;
            process->tag = result->tag;
            process->program = result->program;
            process->arguments = result->arguments;
            process->env = result->env;
            auto raw = process.get();
            auto health = session->externalHealthy;
            auto notify = session->externalExited;
            QObject::connect(raw, &QProcess::readyReadStandardOutput, raw, [raw] { raw->readAllStandardOutput(); });
            QObject::connect(raw, &QProcess::readyReadStandardError, raw, [raw] { raw->readAllStandardError(); });
            QObject::connect(raw, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), raw,
                             [raw, health, notify](int, QProcess::ExitStatus) {
                if (raw->property("sessionStopping").toBool()) return;
                *health = false;
                if (notify) notify();
            });
            process->Start();
            session->external.push_back(process);
            if (!process->waitForStarted(3000)) {
                error = QObject::tr("Cannot start an external protocol helper: %1").arg(process->errorString());
                ok = false;
                break;
            }
        }
        started.release();
    }, DS_cores);
    started.acquire();
    return ok;
}

void SetTrafficSession(const std::shared_ptr<RunningCoreSession> &session) {
    auto looper = NekoGui_traffic::trafficLooper;
    QMutexLocker lock(&looper->loop_mutex);
    looper->loop_enabled = false;
    ++looper->generation;
    looper->items.clear();
    looper->proxy = nullptr;
#ifndef NKR_NO_GRPC
    looper->client.reset();
    if (session) {
        for (const auto &binding : session->built->trafficBindings) {
            binding.data->id = binding.id;
            binding.data->tag = binding.tag;
        }
        looper->client = session->instance->client;
        looper->items = session->built->outboundStats;
        looper->proxy = session->built->outboundStat.get();
        looper->loop_enabled = true;
    }
#endif
}
} // namespace

int MainWindow::active_core() const {
    return core_session ? core_session->instance->core : PreferredCore();
}

void MainWindow::core_notice_message(const QString &message) {
    core_notice = message;
    if (!message.isEmpty()) MW_show_log(message);
    refresh_core_controls();
}

void MainWindow::setup_core_controls() {
    auto panel = new QWidget(this);
    panel->setObjectName("coreSelectionPanel");
    auto layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    auto row = new QHBoxLayout;
    core_selected_label = new QLabel(tr("Select a node"), panel);
    core_selected_label->setObjectName("coreSelectedNode");
    row->addWidget(core_selected_label, 1);
    core_buttons = new QButtonGroup(this);
    const QStringList names{tr("Auto"), "Xray", "sing-box"};
    const QStringList objects{"core_auto", "core_xray", "core_singbox"};
    for (int preference = 0; preference < 3; ++preference) {
        auto button = new QPushButton(names[preference], panel);
        button->setObjectName(objects[preference]);
        button->setCheckable(true);
        button->setToolTip(tr("For the running node, this switches the core. Existing connections may reconnect."));
        core_buttons->addButton(button, preference);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, preference] {
            const auto selected = get_now_selected_list();
            if (selected.size() == 1) set_core_preference(selected.first()->id, preference, true);
        });
    }
    layout->addLayout(row);
    core_choice_reason = new QLabel(panel);
    core_choice_reason->setObjectName("coreChoiceReason");
    core_choice_reason->setWordWrap(true);
    core_choice_reason->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(core_choice_reason);
    ui->verticalLayout_3->insertWidget(1, panel);
    ui->proxyListTable->setColumnCount(6);
    ui->proxyListTable->setHorizontalHeaderItem(5, new QTableWidgetItem(tr("Core")));
    connect(ui->proxyListTable, &QTableWidget::itemSelectionChanged, this, [this] { refresh_core_controls(); });

    auto menu = new QMenu(tr("Core for next connection"), ui->menu_server);
    menu->setObjectName("corePreferenceMenu");
    for (int preference = 0; preference < 3; ++preference) {
        auto action = menu->addAction(names[preference]);
        action->setObjectName(QString("core_bulk_%1").arg(preference));
        connect(action, &QAction::triggered, this, [this, preference] {
            int saved = 0, skipped = 0;
            for (const auto &profile : get_now_selected_list()) {
                const auto resolution = ResolveCore(profile, PreferredCore());
                if (preference && !resolution.Supports(preference - 1)) { ++skipped; continue; }
                set_core_preference(profile->id, preference, false);
                ++saved;
            }
            core_notice_message(tr("Saved preferences for %1 node(s); %2 unsupported. Current connection kept.")
                                    .arg(saved).arg(skipped));
        });
    }
    ui->menu_server->addMenu(menu);
    auto compare = ui->menu_server->addAction(tr("Compare both cores"));
    compare->setObjectName("test_both_cores");
    connect(compare, &QAction::triggered, this, [this] { speedtest_current_group(1, -1, true); });
    auto other = ui->menu_server->addAction(tr("Test the other core"));
    other->setObjectName("test_other_core");
    connect(other, &QAction::triggered, this, [this] {
        const auto profiles = get_now_selected_list();
        if (profiles.size() != 1) return;
        const auto resolution = ResolveCore(profiles.first(), PreferredCore());
        if (resolution.targetCore >= 0 && resolution.Supports(1 - resolution.targetCore))
            speedtest_current_group(1, 1 - resolution.targetCore);
    });
    auto details = ui->menu_server->addAction(tr("Test history"));
    details->setObjectName("test_history");
    connect(details, &QAction::triggered, this, [this] {
        const auto profiles = get_now_selected_list();
        if (profiles.size() != 1) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Test history"));
        auto content = new QVBoxLayout(&dialog);
        auto text = new QTextEdit(&dialog);
        text->setReadOnly(true);
        text->setPlainText(test_result_details(profiles.first()));
        content->addWidget(text);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        content->addWidget(buttons);
        dialog.resize(560, 360);
        dialog.exec();
    });
}

void MainWindow::refresh_core_controls() {
    if (!core_buttons) return;
    const auto selected = get_now_selected_list();
    const bool single = selected.size() == 1;
    for (auto button : core_buttons->buttons()) {
        button->setEnabled(single);
        QSignalBlocker blocker(button);
        button->setChecked(false);
    }
    if (!single) {
        core_selected_label->setText(selected.isEmpty() ? tr("Select a node") : tr("%1 nodes selected").arg(selected.size()));
        core_choice_reason->setText(core_notice.isEmpty() ? tr("Use the Core menu to save preferences for multiple nodes.") : core_notice);
        return;
    }
    const auto profile = selected.first();
    const auto resolution = ResolveCore(profile, PreferredCore());
    core_selected_label->setText(profile->bean->DisplayName().left(64));
    core_selected_label->setToolTip(profile->bean->DisplayName());
    if (auto selectedButton = core_buttons->button(qBound(0, profile->core_preference, 2))) selectedButton->setChecked(true);
    core_buttons->button(1)->setEnabled(resolution.xraySupported);
    core_buttons->button(2)->setEnabled(resolution.singBoxSupported);
    core_buttons->button(1)->setToolTip(resolution.xraySupported ? tr("Use Xray. Existing connections may reconnect.") : resolution.xrayReason);
    core_buttons->button(2)->setToolTip(resolution.singBoxSupported ? tr("Use sing-box. Existing connections may reconnect.") : resolution.singBoxReason);
    QString reason = resolution.reason;
    if (resolution.targetCore >= 0)
        reason = tr("Next connection: %1").arg(CoreManager::Name(resolution.targetCore)) + (reason.isEmpty() ? "" : " · " + reason);
    if (core_session && core_session->profile == profile)
        reason = tr("Running: %1").arg(CoreManager::Name(core_session->instance->core)) + " · " + reason;
    if (resolution.preferenceFallback) reason += " · " + tr("Saved preference temporarily unavailable");
    if (core_switch_busy) reason = tr("Switching…") + " · " + reason;
    if (!core_notice.isEmpty()) reason = core_notice + "\n" + reason;
    core_choice_reason->setText(reason);
}

void MainWindow::set_core_preference(int profileId, int preference, bool applyToRunning) {
    auto profile = profileManager->GetProfile(profileId);
    if (!profile || preference < 0 || preference > 2) return;
    const auto resolution = ResolveCore(profile, PreferredCore());
    if (applyToRunning && preference != 0 && !resolution.Supports(preference - 1)) {
        core_notice_message(preference == 1 ? resolution.xrayReason : resolution.singBoxReason);
        return;
    }
    const auto previous = profile->core_preference;
    if (previous == preference) {
        if (applyToRunning && ((core_session && core_session->profile == profile) ||
                              (active_core_intent && active_core_intent->profile == profile))) queue_core_start(profile);
        else refresh_core_controls();
        return;
    }
    profile->core_preference = preference;
    ++preference_revisions[profileId];
    profile->Save();
    core_notice.clear();
    refresh_proxy_list(profileId);
    if (applyToRunning && ((core_session && core_session->profile == profile) ||
                          (active_core_intent && active_core_intent->profile == profile))) {
        queue_core_start(profile, previous);
    } else {
        core_notice_message(tr("Preference saved; applies to the next connection."));
    }
    refresh_core_controls();
}

QString MainWindow::test_fingerprint(const std::shared_ptr<ProxyEntity> &profile, int core, int mode, const QJsonObject &parameters) {
    auto context = CaptureBuildConfigContext(core, true);
    context.tunProxyPort = tun_proxy_port;
    return CoreTestInputDigest(profile, context, mode, dataStore->test_latency_url,
                               mode == 0 ? 3000 : qBound(1000, dataStore->test_latency_timeout, 60000),
                               mode == 2 ? dataStore->test_download_url : QString(),
                               mode == 2 ? dataStore->test_download_timeout : 0, parameters);
}

ProfileTestResult MainWindow::current_test_result(const std::shared_ptr<ProxyEntity> &profile) {
    ProfileTestResult empty;
    if (!profile || !core_manager) return empty;
    const auto resolution = ResolveCore(profile, PreferredCore());
    if (resolution.targetCore < 0) { empty.status = TestResultStatus::Unsupported; empty.error = resolution.reason; return empty; }
    const auto core = resolution.targetCore;
    const auto identity = core_manager->Identity(core);
    auto result = profile->LastTestResult(core);
    if (result.core != core || result.scope != "next" || result.mode != "url" || result.timestamp_ms <= 0 ||
        result.config_fingerprint != test_fingerprint(profile, core) || result.build_id != identity) return empty;
    if ((result.status == TestResultStatus::Success || result.status == TestResultStatus::ConnectionFailure) &&
        !profile->HasValidTestResult(core, identity, result.config_fingerprint)) return empty;
    if (result.status == TestResultStatus::Legacy || result.status == TestResultStatus::Stale || result.status == TestResultStatus::Cancelled) return empty;
    return result;
}

void MainWindow::refresh_test_validity(const std::shared_ptr<ProxyEntity> &profile) {
    if (!profile) return;
    const auto result = current_test_result(profile);
    profile->latency = result.status == TestResultStatus::Success ? qMax(1, result.latency_ms)
                       : result.status == TestResultStatus::ConnectionFailure ? -1 : 0;
}

QString MainWindow::test_result_text(const std::shared_ptr<ProxyEntity> &profile) {
    const auto result = current_test_result(profile);
    QString text;
    switch (result.status) {
        case TestResultStatus::Success: text = QString("%1 ms").arg(qMax(1, result.latency_ms)); break;
        case TestResultStatus::ConnectionFailure: text = tr("Connection failed"); break;
        case TestResultStatus::Unsupported: text = tr("Unsupported"); break;
        case TestResultStatus::CoreError: text = tr("Core error"); break;
        default: {
            if (profile && core_manager) {
                const auto resolution = ResolveCore(profile, PreferredCore());
                ProfileTestResult other;
                if (resolution.targetCore >= 0) {
                    for (const auto &mode : {QString("tcp"), QString("full")}) {
                        const auto candidate = profile->LastTestResult(resolution.targetCore, "next", mode);
                        const int testMode = mode == "tcp" ? 0 : 2;
                        if (candidate.Matches(resolution.targetCore, core_manager->Identity(resolution.targetCore),
                                              test_fingerprint(profile, resolution.targetCore, testMode, candidate.parameters), "next", mode) &&
                            candidate.timestamp_ms >= other.timestamp_ms) other = candidate;
                    }
                }
                if (other.timestamp_ms) {
                    const auto outcome = other.status == TestResultStatus::Success
                        ? (other.mode == "tcp" ? tr("TCP: %1 ms").arg(other.latency_ms) : tr("Full test complete"))
                        : (other.mode == "tcp" ? tr("TCP connection failed") : tr("Full test failed"));
                    return tr("URL untested") + " · " + outcome + " · " + CoreManager::Name(other.core);
                }
            }
            return tr("Untested");
        }
    }
    return text + (result.core >= 0 ? " · " + CoreManager::Name(result.core) : QString());
}

QString MainWindow::test_result_details(const std::shared_ptr<ProxyEntity> &profile) {
    QStringList lines{profile->bean->DisplayName(), tr("Current results use the next connection's settings.")};
    QList<ProfileTestResult> records;
    QSet<QByteArray> seen;
    const auto include = [&](const QJsonObject &value) {
        const auto key = QJsonDocument(value).toJson(QJsonDocument::Compact);
        if (!seen.contains(key)) {
            seen.insert(key);
            records.append(ProfileTestResult::FromJson(value));
        }
    };
    const auto latest = profile->test_results.value("latest").toObject();
    for (const auto &value : latest) include(value.toObject());
    for (const auto &value : profile->test_results.value("history").toArray()) include(value.toObject());
    std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) { return a.timestamp_ms > b.timestamp_ms; });
    for (const auto &result : records) {
        if (!result.timestamp_ms && result.status != TestResultStatus::Legacy) continue;
        const auto core = result.core >= 0 ? CoreManager::Name(result.core) : tr("Unknown core (legacy)");
        QString status;
        switch (result.status) {
            case TestResultStatus::Success: status = tr("Success"); break;
            case TestResultStatus::ConnectionFailure: status = tr("Connection failed"); break;
            case TestResultStatus::Unsupported: status = tr("Unsupported"); break;
            case TestResultStatus::CoreError: status = tr("Core error"); break;
            case TestResultStatus::Cancelled: status = tr("Cancelled"); break;
            case TestResultStatus::Stale: status = tr("Outdated result"); break;
            case TestResultStatus::Legacy: status = tr("Legacy result"); break;
            default: status = tr("Untested"); break;
        }
        const auto mode = result.mode == "tcp" ? tr("TCP test") : result.mode == "full" ? tr("Full test") : tr("URL test");
        QString line = QString("%1 · %2 · %3 · %4\n%5")
                           .arg(result.timestamp_ms ? QDateTime::fromMSecsSinceEpoch(result.timestamp_ms).toString(Qt::ISODate) : tr("Unknown date"),
                                core, result.scope == "current" ? tr("Current connection") : tr("Next connection"), mode, status);
        if (result.status == TestResultStatus::Success && result.mode != "full") line += QString(" · %1 ms").arg(result.latency_ms);
        if (!result.error.isEmpty()) line += "\n" + result.error;
        if (!result.report.isEmpty()) line += "\n" + result.report;
        lines.append(line);
    }
    return lines.join("\n\n");
}

void MainWindow::queue_core_start(const std::shared_ptr<ProxyEntity> &profile, int rollbackPreference, bool prepareForTun) {
    if (!profile || dataStore->prepare_exit) return;
    auto intent = std::make_shared<CoreSwitchIntent>();
    intent->sequence = ++core_intent_sequence;
    intent->profile = profile;
    intent->rollbackPreference = rollbackPreference;
    intent->prepareForTun = prepareForTun;
    intent->preferenceRevision = preference_revisions.value(profile->id);
    pending_core_intent = intent; // Coalesce pending clicks to the latest intent.
    process_core_intent();
}

void MainWindow::neko_start(int id) {
    if (dataStore->prepare_exit) return;
    const auto selected = get_now_selected_list();
    auto profile = id < 0 && !selected.isEmpty() ? selected.first() : profileManager->GetProfile(id);
    if (!profile) return;
    if (select_mode) {
        emit profile_selected(profile->id);
        select_mode = false;
        refresh_status();
        return;
    }
    auto group = profileManager->GetGroup(profile->gid);
    if (!group || group->archive) return;
    queue_core_start(profile);
}

void MainWindow::neko_stop(bool crash, bool sem) {
    auto intent = std::make_shared<CoreSwitchIntent>();
    intent->sequence = ++core_intent_sequence;
    intent->crash = crash;
    intent->signalStopped = sem || (pending_core_intent && pending_core_intent->signalStopped);
    pending_core_intent = intent;
    process_core_intent();
}

void MainWindow::process_core_intent() {
    if (core_switch_busy || vpn_changing || !pending_core_intent) return;
    const auto intent = pending_core_intent;
    pending_core_intent.reset();
    const auto previous = core_session;
    std::shared_ptr<RunningCoreSession> target, alternative;
    QString prepareError;
    const auto buildSession = [&](int core) {
        auto session = std::make_shared<RunningCoreSession>();
        session->profile = intent->profile;
        session->context = CaptureBuildConfigContext(core, true);
        session->context.tunProxyPort = tun_proxy_port;
        if (intent->prepareForTun) session->context.settings->spmode_vpn = true;
        session->built = BuildConfig(intent->profile, false, false, session->context);
        session->instance = core_manager->Get(core);
        session->fingerprint = QString::fromLatin1(session->built->Fingerprint()) + ":" + QString::fromLatin1(session->context.Fingerprint());
        session->displayName = intent->profile->bean->DisplayTypeAndName();
        QSet<int> visited;
        session->profileInputDigest = CoreConfigDigest(CoreProfileInputs(intent->profile, visited));
#ifndef NKR_NO_GRPC
        session->request.set_core_config(QJsonObject2QString(session->built->coreConfig, true).toStdString());
        session->request.set_enable_nekoray_connections(session->context.settings->connection_statistics);
        if (session->context.settings->traffic_loop_interval > 0) {
            session->request.add_stats_outbounds("proxy");
            session->request.add_stats_outbounds("bypass");
        }
#endif
        return session;
    };
    if (intent->profile) {
        if (profileManager->GetProfile(intent->profile->id) != intent->profile) {
            if (intent->prepareForTun) tun_bridge_prepared(false, true);
            process_core_intent();
            return;
        }
        if (intent->recovery) {
            target = std::make_shared<RunningCoreSession>(*intent->recovery);
            target->external.clear();
        } else {
            const auto resolution = ResolveCore(intent->profile, PreferredCore());
            if (resolution.targetCore < 0) prepareError = resolution.reason;
            else {
                target = buildSession(resolution.targetCore);
                prepareError = target->built->error;
                // Prepare the other configuration from the same GUI snapshot.
                // It is used only if local capability discovery rejects the first.
                if (prepareError.isEmpty() && resolution.Supports(1 - resolution.targetCore)) {
                    alternative = buildSession(1 - resolution.targetCore);
                    if (!alternative->built->error.isEmpty()) alternative.reset();
                }
            }
        }
    }
    if (!prepareError.isEmpty()) {
        if (intent->rollbackPreference >= 0 && preference_revisions.value(intent->profile->id) == intent->preferenceRevision) {
            intent->profile->core_preference = intent->rollbackPreference;
            intent->profile->Save();
            refresh_proxy_list(intent->profile->id);
        }
        core_notice_message(prepareError);
        if (intent->prepareForTun) tun_bridge_prepared(false);
        process_core_intent();
        return;
    }
    if (!target && !previous) {
        if (intent->signalStopped) sem_stopped.release();
        return;
    }
    const auto healthy = [](const std::shared_ptr<RunningCoreSession> &session) {
        return session && session->externalHealthy->load() && session->instance->alive && session->instance->ready &&
               session->generation == session->instance->generation.load();
    };
    if (target && previous && !intent->recovery && previous->profile == target->profile && previous->instance == target->instance &&
        previous->fingerprint == target->fingerprint && healthy(previous)) {
        core_notice.clear();
        refresh_core_controls();
        if (intent->prepareForTun) tun_bridge_prepared(true);
        return;
    }
    for (const auto &session : {target, alternative}) {
        if (!session) continue;
        const QPointer<MainWindow> guarded(this);
        const std::weak_ptr<RunningCoreSession> weakSession(session);
        session->externalExited = [guarded, weakSession] {
            if (!guarded) return;
            runOnUiThread([guarded, weakSession] {
                auto saved = weakSession.lock();
                if (!guarded || !saved || guarded->core_session != saved || guarded->core_switch_busy) return;
                guarded->core_notice_message(QObject::tr("The external protocol helper exited. Disconnected."));
                guarded->neko_stop();
            }, guarded);
        };
    }
    core_switch_busy = true;
    if (current_test_cancel) current_test_cancel->store(true);
    active_core_intent = intent;
    core_notice.clear();
    refresh_core_controls();
    const QPointer<MainWindow> guard(this);
    runOnNewThread([this, guard, intent, previous, target, alternative, healthy]() mutable {
        QString error;
        bool succeeded = false, touchedPrevious = false, cancelled = false, targetAttempted = false;
        auto resultingSession = previous;
#ifndef NKR_NO_GRPC
        const auto stop = [this](const std::shared_ptr<RunningCoreSession> &session, QString &failure) {
            if (!session) return true;
            bool closed = true;
            if (session->instance->alive) {
                bool ok = false;
                failure = session->instance->client->Stop(&ok);
                if (!ok || !failure.isEmpty()) {
                    if (failure.isEmpty()) failure = QObject::tr("The previous core could not confirm stopping.");
                    QString reconcileError;
                    closed = core_manager->StopFaulted(session->instance->core, reconcileError);
                    if (!closed) {
                        session->externalHealthy->store(false);
                        failure += "\n" + reconcileError;
                    }
                }
            }
            StopSessionExternal(session);
            return closed;
        };
        const auto preflight = [this](const std::shared_ptr<RunningCoreSession> &session, QString &failure, bool &unsupported) {
            unsupported = false;
            if (!core_manager->EnsureReady(session->instance->core, failure)) return false;
            session->generation = session->instance->generation.load();
            failure = CoreBuildFeaturesError(session->built->coreConfig, session->instance->core,
                                              core_manager->Features(session->instance->core));
            if (!failure.isEmpty()) { unsupported = true; return false; }
            bool ok = false;
            const auto check = session->instance->client->Check(&ok, session->request);
            if (!ok) failure = QObject::tr("Configuration preflight could not be completed.");
            else if (!check.error().empty()) {
                failure = QString::fromStdString(check.error());
                unsupported = check.failure_category() == libcore::FailureUnsupported;
            }
            if (failure.isEmpty() && (!session->instance->alive || !session->instance->ready ||
                session->generation != session->instance->generation.load()))
                failure = QObject::tr("The core exited during configuration preflight.");
            return failure.isEmpty();
        };
        const auto start = [](const std::shared_ptr<RunningCoreSession> &session, QString &failure) {
            const auto generation = session->instance->generation.load();
            if (!session->instance->alive || !session->instance->ready || generation != session->generation) {
                failure = QObject::tr("The core exited before startup.");
                return false;
            }
            if (!StartSessionExternal(session, failure)) return false;
            bool ok = false;
            failure = session->instance->client->Start(&ok, session->request);
            if (!ok && failure.isEmpty()) failure = QObject::tr("Core start could not be confirmed.");
            if (!ok || !failure.isEmpty()) return false;
            if (!session->externalHealthy->load() || !session->instance->alive ||
                !session->instance->ready || session->instance->generation.load() != generation) {
                failure = QObject::tr("The core or external protocol helper exited during startup.");
                return false;
            }
            return true;
        };
        if (target) {
            bool unsupported = false;
            if (!preflight(target, error, unsupported) && unsupported && alternative) {
                const auto originalError = error;
                error.clear();
                if (preflight(alternative, error, unsupported)) {
                    target = alternative;
                    target->selectionNotice = tr("%1 Using %2 for this configuration; the saved preference is kept.")
                                                  .arg(originalError, CoreManager::Name(target->instance->core));
                } else error = originalError + "\n" + error;
            }
        }
        bool previousMayRestore = bool(previous);
        const auto validateTarget = [&] {
            bool profileStillValid = true;
            if (!target) return true;
            QSemaphore verified;
            runOnUiThread([&] {
                if (!guard) { profileStillValid = false; verified.release(); return; }
                QSet<int> visited;
                auto currentContext = CaptureBuildConfigContext(target->instance->core, true);
                currentContext.tunProxyPort = tun_proxy_port;
                if (intent->prepareForTun) currentContext.settings->spmode_vpn = true;
                profileStillValid = !dataStore->prepare_exit && profileManager->GetProfile(target->profile->id) == target->profile &&
                                    (intent->recovery || (target->profileInputDigest == CoreConfigDigest(CoreProfileInputs(target->profile, visited)) &&
                                                         target->context.Fingerprint() == currentContext.Fingerprint()));
                previousMayRestore = previous && profileManager->GetProfile(previous->profile->id) == previous->profile;
                verified.release();
            }, this);
            verified.acquire();
            return profileStillValid;
        };
        // A superseded preparation must not release the old listener.
        cancelled = target && (!validateTarget() || intent->sequence != core_intent_sequence.load());
        bool previousClosed = !previous;
        if (error.isEmpty() && !cancelled) {
            SetTrafficSession(nullptr);
            if (previous) {
                touchedPrevious = true;
                previousClosed = stop(previous, error);
                if (previousClosed) resultingSession.reset();
            }
            if (error.isEmpty()) {
                if (target) {
                    targetAttempted = true;
                    succeeded = start(target, error);
                    if (succeeded) resultingSession = target;
                } else succeeded = true;
            }
        }
        if (succeeded && target && (!validateTarget() || !healthy(target))) {
            succeeded = false;
            cancelled = true;
            error = tr("The node or settings changed during startup, or the core exited. The pending start was cancelled.");
        }
        // Stop every partially started target before restoring old listeners.
        bool targetClosed = true;
        if (targetAttempted && !succeeded) {
            QString cleanupError;
            targetClosed = stop(target, cleanupError);
            if (!cleanupError.isEmpty()) error += "\n" + cleanupError;
            if (targetClosed && resultingSession == target) resultingSession.reset();
            if (!targetClosed) resultingSession = target; // Retain ownership for retry.
        }
        const bool previousDied = previous && !touchedPrevious && !healthy(previous);
        if (!succeeded && target && previousMayRestore && previous && (touchedPrevious || previousDied) && targetClosed &&
            (previousClosed || previousDied) && (!cancelled || targetAttempted) && !dataStore->prepare_exit) {
            QString restoreError;
            bool unsupported = false;
            if (previousDied) stop(previous, restoreError);
            restoreError.clear();
            if (preflight(previous, restoreError, unsupported) && start(previous, restoreError)) {
                resultingSession = previous;
                error += "\n" + tr("The previous connection configuration has been restored.");
            } else {
                QString cleanupError;
                if (stop(previous, cleanupError)) resultingSession.reset();
                else resultingSession = previous;
                error += "\n" + tr("Disconnected: the previous connection could not be restored.") + " " + restoreError;
                if (!cleanupError.isEmpty()) error += "\n" + cleanupError;
            }
        }
        if (resultingSession && !healthy(resultingSession)) {
            QString cleanupError;
            if (stop(resultingSession, cleanupError)) resultingSession.reset();
            if (!cleanupError.isEmpty()) error += "\n" + cleanupError;
            succeeded = false;
            if (error.isEmpty()) error = tr("The core exited during the switch. Disconnected.");
        }
        if (touchedPrevious || resultingSession != previous) SetTrafficSession(healthy(resultingSession) ? resultingSession : nullptr);
#else
        error = tr("This build has no core RPC support.");
#endif
        runOnUiThread([this, guard, intent, previous, target, resultingSession, error, succeeded, cancelled, healthy]() mutable {
            if (!guard) return;
            const auto previousId = previous ? previous->profile->id : -1;
            const bool superseded = intent->sequence != core_intent_sequence.load();
            const bool exited = resultingSession && !healthy(resultingSession);
            const bool removed = resultingSession && profileManager->GetProfile(resultingSession->profile->id) != resultingSession->profile;
            bool outdated = false;
            if (resultingSession == target && target && !intent->recovery) {
                auto context = CaptureBuildConfigContext(target->instance->core, true);
                context.tunProxyPort = tun_proxy_port;
                if (intent->prepareForTun) context.settings->spmode_vpn = true;
                QSet<int> visited;
                outdated = target->profileInputDigest != CoreConfigDigest(CoreProfileInputs(target->profile, visited)) ||
                           target->context.Fingerprint() != context.Fingerprint();
            }
            if (outdated) {
                succeeded = false;
                cancelled = true;
                error = tr("The node or settings changed during startup, or the core exited. The pending start was cancelled.");
                SetTrafficSession(nullptr);
            }
            if (exited) {
                error += "\n" + tr("The core exited during the switch. Disconnected.");
                SetTrafficSession(nullptr);
                succeeded = false;
            }
            // Even an uncertain session stays owned until its Stop completes.
            core_session = resultingSession;
            running = resultingSession && !removed && !exited && !outdated ? resultingSession->profile : nullptr;
            dataStore->UpdateStartedId(running ? running->id : -1919);
            dataStore->need_keep_vpn_off = running && resultingSession->built->needKeepVpnOff;
            dataStore->ignoreConnTag = running ? resultingSession->built->ignoreConnTag : QStringList{};
            if (!succeeded && !cancelled && intent->profile && intent->rollbackPreference >= 0 &&
                intent->sequence == core_intent_sequence.load() &&
                preference_revisions.value(intent->profile->id) == intent->preferenceRevision &&
                profileManager->GetProfile(intent->profile->id) == intent->profile) {
                intent->profile->core_preference = intent->rollbackPreference;
                intent->profile->Save();
            }
            core_switch_busy = false;
            active_core_intent.reset();
            if (!superseded && !error.isEmpty()) core_notice_message(error.trimmed());
            else if (!superseded && succeeded && resultingSession) {
                core_notice = resultingSession->selectionNotice;
                MW_show_log(tr("Connected using %1.").arg(CoreManager::Name(resultingSession->instance->core)));
            }
            if (previousId >= 0) refresh_proxy_list(previousId);
            if (intent->profile) refresh_proxy_list(intent->profile->id);
            refresh_status();
            refresh_core_controls();
            if (intent->signalStopped) sem_stopped.release();
            if (intent->prepareForTun) tun_bridge_prepared(!superseded && succeeded && resultingSession == target, cancelled || superseded);
            if ((removed || exited || outdated) && !pending_core_intent) {
                if (previous && !dataStore->prepare_exit && profileManager->GetProfile(previous->profile->id) == previous->profile &&
                    intent->profile && !intent->recovery) {
                    auto recovery = std::make_shared<CoreSwitchIntent>();
                    recovery->sequence = ++core_intent_sequence;
                    recovery->profile = previous->profile;
                    recovery->recovery = previous;
                    pending_core_intent = recovery;
                } else neko_stop();
            }
            process_core_intent();
        }, this);
    });
}
