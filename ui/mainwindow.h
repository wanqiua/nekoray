#pragma once

#include <QMainWindow>
#include <atomic>

#include "main/NekoGui.hpp"

#ifndef MW_INTERFACE

#include <QTime>
#include <QTableWidgetItem>
#include <QKeyEvent>
#include <QSystemTrayIcon>
#include <QProcess>
#include <QTextDocument>
#include <QShortcut>
#include <QSemaphore>
#include <QMutex>

#include "GroupSort.hpp"

#include "db/ProxyEntity.hpp"
#include "main/GuiUtils.hpp"

#endif

namespace NekoGui_sys {
    class CoreProcess;
    class CoreManager;
}

namespace NekoGui {
    struct RunningCoreSession;
    struct CoreSwitchIntent;
    struct ProfileTestResult;
}
class QButtonGroup;
class QPushButton;
class QLabel;
class QToolButton;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
class QFile;
namespace NekoGui_rpc { class Client; }

QT_BEGIN_NAMESPACE
namespace Ui {
    class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    ~MainWindow() override;

    void refresh_proxy_list(const int &id = -1);

    void show_group(int gid);

    void refresh_groups();

    void refresh_status(const QString &traffic_update = "");

    void neko_start(int _id = -1);

    void neko_stop(bool crash = false, bool sem = false);

    void neko_set_spmode_system_proxy(bool enable, bool save = true);

    void neko_set_spmode_vpn(bool enable, bool save = true);

    void show_log_impl(const QString &log);

    void start_select_mode(QObject *context, const std::function<void(int)> &callback);

    void refresh_connection_list(const QJsonArray &arr);

    void RegisterHotkey(bool unregister);

    bool StopVPNProcess(bool unconditional = false);

    void set_core_preference(int profileId, int preference, bool applyToRunning = false);

    int active_core() const;

signals:

    void profile_selected(int id);

public slots:

    void on_commitDataRequest();

    void on_menu_exit_triggered();

#ifndef MW_INTERFACE

private slots:

    void show_group_context_menu(const QPoint &pos);

    void on_masterLogBrowser_customContextMenuRequested(const QPoint &pos);

    void on_menu_basic_settings_triggered();

    void on_menu_routing_settings_triggered();

    void on_menu_vpn_settings_triggered();

    void on_menu_hotkey_settings_triggered();

    void on_menu_add_from_input_triggered();

    void on_menu_add_from_clipboard_triggered();

    void on_menu_clone_triggered();

    void on_menu_move_triggered();

    void on_menu_delete_triggered();

    void on_menu_reset_traffic_triggered();

    void on_menu_profile_debug_info_triggered();

    void on_menu_copy_links_triggered();

    void on_menu_copy_links_nkr_triggered();

    void on_menu_export_config_triggered();

    void display_qr_link(bool nkrFormat = false);

    void on_menu_scan_qr_triggered();

    void on_menu_clear_test_result_triggered();

    void on_menu_manage_groups_triggered();

    void on_menu_select_all_triggered();

    void on_menu_delete_repeat_triggered();

    void on_menu_remove_unavailable_triggered();

    void on_menu_update_subscription_triggered();

    void on_menu_resolve_domain_triggered();

    void on_proxyListTable_itemDoubleClicked(QTableWidgetItem *item);

    void on_proxyListTable_customContextMenuRequested(const QPoint &pos);

    void on_tabWidget_currentChanged(int index);

private:
    friend class GuiRegressionTest;
    Ui::MainWindow *ui;
    QSystemTrayIcon *tray;
    QShortcut *shortcut_ctrl_f = new QShortcut(QKeySequence("Ctrl+F"), this);
    QShortcut *shortcut_esc = new QShortcut(QKeySequence("Esc"), this);
    //
    NekoGui_sys::CoreManager *core_manager = nullptr;
    qint64 vpn_pid = 0;
    void *vpn_handle = nullptr;
    QProcess *vpn_process = nullptr;
    bool vpn_changing = false;
    QString vpn_config_fingerprint;
    int tun_proxy_port = 0;
#ifndef NKR_NO_GRPC
    std::shared_ptr<NekoGui_rpc::Client> vpn_client;
#endif
    //
    bool qvLogAutoScoll = true;
    QTextDocument *qvLogDocument = new QTextDocument(this);
    //
    QString title_error;
    int icon_status = -1;
    std::shared_ptr<NekoGui::ProxyEntity> running;
    QString traffic_update_cache;
    QTime last_test_time;
    //
    int proxy_last_order = -1;
    bool select_mode = false;
    QMutex mu_starting;
    QMutex mu_stopping;
    QMutex mu_exit;
    QSemaphore sem_stopped;
    int exit_reason = 0;
    bool exit_completed = false;
    std::atomic<int> core_background_jobs{0};
    std::shared_ptr<std::atomic_bool> current_test_cancel;

    std::shared_ptr<NekoGui::RunningCoreSession> core_session;
    std::shared_ptr<NekoGui::CoreSwitchIntent> pending_core_intent;
    std::shared_ptr<NekoGui::CoreSwitchIntent> active_core_intent;
    bool core_switch_busy = false;
    std::atomic<quint64> core_intent_sequence{0};
    QHash<int, quint64> preference_revisions;
    QButtonGroup *core_buttons = nullptr;
    QLabel *core_selected_label = nullptr;
    QLabel *connectionEmptyLabel = nullptr;
    QByteArray expandedSplitterState;
    bool lowerPaneCollapsed = false;
    QStringList updaterArguments;
    QString core_notice;
    int last_crashed_core = -1;
    QTime last_core_crash;

    void setup_core_controls();
    void refresh_core_controls();
    void queue_core_start(const std::shared_ptr<NekoGui::ProxyEntity> &profile, int rollbackPreference = -1, bool prepareForTun = false);
    void process_core_intent();
    void core_notice_message(const QString &message);
    void refresh_test_validity(const std::shared_ptr<NekoGui::ProxyEntity> &profile);
    QString test_fingerprint(const std::shared_ptr<NekoGui::ProxyEntity> &profile, int core, int mode = 1,
                             const QJsonObject &parameters = {});
    NekoGui::ProfileTestResult current_test_result(const std::shared_ptr<NekoGui::ProxyEntity> &profile);
    NekoGui::ProfileTestResult removal_test_result(const std::shared_ptr<NekoGui::ProxyEntity> &profile);
    QString test_result_text(const std::shared_ptr<NekoGui::ProxyEntity> &profile);
    QString test_result_details(const std::shared_ptr<NekoGui::ProxyEntity> &profile);

    QList<std::shared_ptr<NekoGui::ProxyEntity>> get_now_selected_list();

    QList<std::shared_ptr<NekoGui::ProxyEntity>> get_selected_or_group();

    void dialog_message_impl(const QString &sender, const QString &info);

    void refresh_proxy_list_impl(const int &id = -1, GroupSortAction groupSortAction = {});

    void refresh_proxy_list_impl_refresh_data(const int &id = -1);

    void keyPressEvent(QKeyEvent *event) override;

    void closeEvent(QCloseEvent *event) override;

    //

    void HotkeyEvent(const QString &key);

    bool StartVPNProcess();
    void reload_tun_settings();
    void tun_bridge_prepared(bool succeeded, bool cancelled = false);

    // grpc and ...

    void setup_grpc();

    void speedtest_current_group(int mode, int coreOverride = -1, bool compare = false);

    void speedtest_current();

    void stop_core_daemon();

    void CheckUpdate();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

#endif // MW_INTERFACE
};

inline MainWindow *GetMainWindow() {
    return (MainWindow *) mainwindow;
}

void UI_InitMainWindow();
