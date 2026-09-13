#include "dialog_edit_profile.h"
#include "ui_dialog_edit_profile.h"

#include "ui/edit/edit_socks_http.h"
#include "ui/edit/edit_shadowsocks.h"
#include "ui/edit/edit_chain.h"
#include "ui/edit/edit_vmess.h"
#include "ui/edit/edit_trojan_vless.h"
#include "ui/edit/edit_naive.h"
#include "ui/edit/edit_quic.h"
#include "ui/edit/edit_custom.h"

#include "fmt/includes.h"
#include "fmt/Preset.hpp"

#include "3rdparty/qv2ray/v2/ui/widgets/editors/w_JsonEditor.hpp"
#include "main/GuiUtils.hpp"
#include "db/CoreCapabilities.hpp"
#include "db/ProfileFilter.hpp"
#include "ui/mainwindow_interface.h"

#include <QInputDialog>
#include <QButtonGroup>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QTimer>

#define ADJUST_SIZE QTimer::singleShot(0, this, [this] { adjustSize(); adjustPosition(mainwindow); });
#define LOAD_TYPE(a) ui->type->addItem(NekoGui::ProfileManager::NewProxyEntity(a)->bean->DisplayType(), a);

DialogEditProfile::DialogEditProfile(const QString &_type, int profileOrGroupId, QWidget *parent)
    : QDialog(parent), ui(new Ui::DialogEditProfile) {
    // setup UI
    ui->setupUi(this);
    ui->dialog_layout->setAlignment(ui->left, Qt::AlignTop);

    auto coreBox = new QGroupBox(tr("节点内核"), this);
    coreBox->setObjectName("edit_core_box");
    auto coreLayout = new QVBoxLayout(coreBox);
    auto coreRow = new QHBoxLayout;
    coreChoices = new QButtonGroup(this);
    const QStringList labels{tr("自动"), QStringLiteral("Xray"), QStringLiteral("sing-box")};
    const QStringList names{"edit_core_auto", "edit_core_xray", "edit_core_singbox"};
    for (int preference = 0; preference < labels.size(); ++preference) {
        auto button = new QRadioButton(labels[preference], coreBox);
        button->setObjectName(names[preference]);
        coreChoices->addButton(button, preference);
        coreRow->addWidget(button);
        connect(button, &QRadioButton::clicked, this, [this, preference] {
            pendingCorePreference = preference;
            refresh_core_choice();
        });
    }
    coreChoices->button(0)->setChecked(true);
    coreLayout->addLayout(coreRow);
    coreReason = new QLabel(coreBox);
    coreReason->setObjectName("edit_core_reason");
    coreReason->setWordWrap(true);
    coreLayout->addWidget(coreReason);
    ui->left->insertWidget(1, coreBox);

    // network changed
    network_title_base = ui->network_box->title();
    connect(ui->network, &QComboBox::currentTextChanged, this, [=](const QString &txt) {
        ui->network_box->setTitle(network_title_base.arg(txt));
        // 传输设置
        ui->path_l->setText(txt == "kcp" ? QStringLiteral("Seed") : txt == "quic" ? QStringLiteral("Key") : tr("Path"));
        ui->host_l->setText(txt == "quic" ? tr("Security") : tr("Host"));
        if (txt == "tcp" || txt == "quic" || txt == "kcp") {
            ui->header_type->setVisible(true);
            ui->header_type_l->setVisible(true);
            ui->path->setVisible(true);
            ui->path_l->setVisible(true);
            ui->host->setVisible(txt != "kcp");
            ui->host_l->setVisible(txt != "kcp");
        } else if (txt == "grpc") {
            ui->header_type->setVisible(false);
            ui->header_type_l->setVisible(false);
            ui->path->setVisible(true);
            ui->path_l->setVisible(true);
            ui->host->setVisible(false);
            ui->host_l->setVisible(false);
        } else if (txt == "ws" || txt == "http" || txt == "httpupgrade") {
            ui->header_type->setVisible(false);
            ui->header_type_l->setVisible(false);
            ui->path->setVisible(true);
            ui->path_l->setVisible(true);
            ui->host->setVisible(true);
            ui->host_l->setVisible(true);
        } else {
            ui->header_type->setVisible(false);
            ui->header_type_l->setVisible(false);
            ui->path->setVisible(false);
            ui->path_l->setVisible(false);
            ui->host->setVisible(false);
            ui->host_l->setVisible(false);
        }
        // 传输设置 ED
        if (txt == "ws") {
            ui->ws_early_data_length->setVisible(true);
            ui->ws_early_data_length_l->setVisible(true);
            ui->ws_early_data_name->setVisible(true);
            ui->ws_early_data_name_l->setVisible(true);
        } else {
            ui->ws_early_data_length->setVisible(false);
            ui->ws_early_data_length_l->setVisible(false);
            ui->ws_early_data_name->setVisible(false);
            ui->ws_early_data_name_l->setVisible(false);
        }
        for (const auto &fingerprint: Preset::Xray::UtlsFingerPrint + Preset::SingBox::UtlsFingerPrint) {
            if (ui->utlsFingerprint->findText(fingerprint) < 0) ui->utlsFingerprint->addItem(fingerprint);
        }
        // 传输设置 是否可见
        int networkBoxVisible = 0;
        for (auto label: ui->network_box->findChildren<QLabel *>()) {
            if (!label->isHidden()) networkBoxVisible++;
        }
        ui->network_box->setVisible(networkBoxVisible);
        refresh_core_choice();
        ADJUST_SIZE
    });
    ui->network->removeItem(0);

    ui->network->addItems({"httpupgrade", "kcp"});
    ui->header_type->addItems({"none", "srtp", "utp", "wechat-video", "dtls", "wireguard"});

    // security changed
    connect(ui->security, &QComboBox::currentTextChanged, this, [=](const QString &txt) {
        if (txt == "tls") {
            ui->security_box->setVisible(true);
            ui->tls_camouflage_box->setVisible(true);
            ui->reality_spx->show();
            ui->reality_spx_l->show();
        } else {
            ui->security_box->setVisible(false);
            ui->tls_camouflage_box->setVisible(false);
        }
        refresh_core_choice();
        ADJUST_SIZE
    });
    emit ui->security->currentTextChanged(ui->security->currentText());

    // 确定模式和 ent
    newEnt = _type != "";
    if (newEnt) {
        this->groupId = profileOrGroupId < 0 ? NekoGui::dataStore->current_group : profileOrGroupId;
        this->type = _type;

        // load type to combo box
        LOAD_TYPE("socks")
        LOAD_TYPE("http")
        LOAD_TYPE("shadowsocks")
        LOAD_TYPE("trojan")
        LOAD_TYPE("vmess")
        LOAD_TYPE("vless")
        LOAD_TYPE("naive")
        LOAD_TYPE("hysteria")
        LOAD_TYPE("hysteria2")
        LOAD_TYPE("tuic")
        ui->type->addItem(tr("Custom (%1 outbound)").arg(software_core_name), "internal");
        ui->type->addItem(tr("Custom (%1 config)").arg(software_core_name), "internal-full");
        ui->type->addItem(tr("Custom (Extra Core)"), "custom");
        LOAD_TYPE("chain")

        // type changed
        connect(ui->type, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [=](int index) {
            typeSelected(ui->type->itemData(index).toString());
        });

        ui->apply_to_group->hide();
        const QSignalBlocker blocked(ui->type);
        ui->type->setCurrentIndex(ui->type->findData(_type));
    } else {
        originalEnt = NekoGui::profileManager->GetProfile(profileOrGroupId);
        if (!originalEnt) return;
        this->type = originalEnt->type;
        this->ent = NekoGui::ProfileManager::NewProxyEntity(type);
        this->ent->FromJson(originalEnt->ToJson());
        pendingCorePreference = ent->core_preference;
        coreChoices->button(pendingCorePreference)->setChecked(true);
        ui->type->setVisible(false);
        ui->type_l->setVisible(false);
    }

    typeSelected(this->type);

    // Preview changes to the common controls without committing the draft.
    for (auto control: ui->groupBox->findChildren<QLineEdit *>()) {
        connect(control, &QLineEdit::textChanged, this, [this] { refresh_core_choice(); });
    }
    for (auto control: ui->right_all_w->findChildren<QLineEdit *>()) {
        connect(control, &QLineEdit::textChanged, this, [this] { refresh_core_choice(); });
    }
    for (auto control: ui->right_all_w->findChildren<QComboBox *>()) {
        connect(control, &QComboBox::currentTextChanged, this, [this] { refresh_core_choice(); });
    }
    connect(ui->insecure, &QCheckBox::toggled, this, [this] { refresh_core_choice(); });
}

DialogEditProfile::~DialogEditProfile() {
    delete ui;
}

void DialogEditProfile::typeSelected(const QString &newType) {
    loadingEditor = true;
    QString customType;
    type = newType;
    bool validType = true;

    if (type == "socks" || type == "http") {
        auto _innerWidget = new EditSocksHttp(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
    } else if (type == "shadowsocks") {
        auto _innerWidget = new EditShadowSocks(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
    } else if (type == "chain") {
        auto _innerWidget = new EditChain(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
    } else if (type == "vmess") {
        auto _innerWidget = new EditVMess(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
    } else if (type == "trojan" || type == "vless") {
        auto _innerWidget = new EditTrojanVLESS(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
    } else if (type == "naive") {
        auto _innerWidget = new EditNaive(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
    } else if (type == "hysteria" || type == "hysteria2" || type == "tuic") {
        auto _innerWidget = new EditQUIC(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
    } else if (type == "custom" || type == "internal" || type == "internal-full") {
        auto _innerWidget = new EditCustom(this);
        innerWidget = _innerWidget;
        innerEditor = _innerWidget;
        customType = newEnt ? type : ent->CustomBean()->core;
        if (customType != "custom") _innerWidget->preset_core = customType;
        type = "custom";
    } else {
        validType = false;
    }

    if (!validType) {
        loadingEditor = false;
        MessageBoxWarning(newType, "Wrong type");
        return;
    }

    if (newEnt) {
        this->ent = NekoGui::ProfileManager::NewProxyEntity(type);
        this->ent->gid = groupId;
    }

    // hide some widget
    auto showAddressPort = type != "chain" && customType != "internal" && customType != "internal-full";
    ui->address->setVisible(showAddressPort);
    ui->address_l->setVisible(showAddressPort);
    ui->port->setVisible(showAddressPort);
    ui->port_l->setVisible(showAddressPort);

    // 右边 stream
    auto stream = GetStreamSettings(ent->bean.get());
    if (stream != nullptr) {
        auto preserveCombo = [](QComboBox *combo, const QString &value) {
            if (combo->findText(value) < 0) combo->addItem(value);
        };
        preserveCombo(ui->network, stream->network);
        preserveCombo(ui->security, stream->security);
        preserveCombo(ui->packet_encoding, stream->packet_encoding);
        preserveCombo(ui->utlsFingerprint, stream->utlsFingerprint);
        ui->right_all_w->setVisible(true);
        ui->network->setCurrentText(stream->network);
        ui->security->setCurrentText(stream->security);
        ui->packet_encoding->setCurrentText(stream->packet_encoding);
        ui->path->setText(stream->path);
        ui->host->setText(stream->host);
        ui->sni->setText(stream->sni);
        ui->alpn->setText(stream->alpn);
        if (newEnt) {
            ui->utlsFingerprint->setCurrentText(NekoGui::dataStore->utlsFingerprint);
        } else {
            ui->utlsFingerprint->setCurrentText(stream->utlsFingerprint);
        }
        ui->insecure->setChecked(stream->allow_insecure);
        ui->header_type->setCurrentText(stream->header_type);
        ui->ws_early_data_name->setText(stream->ws_early_data_name);
        ui->ws_early_data_length->setText(Int2String(stream->ws_early_data_length));
        ui->reality_pbk->setText(stream->reality_pbk);
        ui->reality_sid->setText(stream->reality_sid);
        ui->reality_spx->setText(stream->reality_spx);
        ui->multiplex->setCurrentIndex(stream->multiplex_status);
        CACHE.certificate = stream->certificate;
    } else {
        ui->right_all_w->setVisible(false);
    }

    // left: custom
    CACHE.custom_config = ent->bean->custom_config;
    CACHE.custom_outbound = ent->bean->custom_outbound;
    bool show_custom_config = true;
    bool show_custom_outbound = true;
    if (type == "chain") {
        show_custom_outbound = false;
    } else if (type == "custom") {
        if (customType == "internal") {
            show_custom_outbound = false;
        } else if (customType == "internal-full") {
            show_custom_outbound = false;
            show_custom_config = false;
        }
    }
    ui->custom_box->setVisible(show_custom_outbound);
    ui->custom_global_box->setVisible(show_custom_config);

    // 左边 bean
    auto old = ui->bean->layout()->itemAt(0)->widget();
    ui->bean->layout()->removeWidget(old);
    innerWidget->layout()->setContentsMargins(0, 0, 0, 0);
    ui->bean->layout()->addWidget(innerWidget);
    ui->bean->setTitle(ent->bean->DisplayType());
    delete old;

    // 左边 bean inner editor
    innerEditor->get_edit_dialog = [&]() { return (QWidget *) this; };
    innerEditor->get_edit_text_name = [&]() { return ui->name->text(); };
    innerEditor->get_edit_text_serverAddress = [&]() { return ui->address->text(); };
    innerEditor->get_edit_text_serverPort = [&]() { return ui->port->text(); };
    innerEditor->editor_cache_updated = [=] { editor_cache_updated_impl(); };
    innerEditor->onStart(ent);

    // The editor presents the union of both adapters. ResolveCore explains
    // which combinations can be used; the old global core no longer hides a
    // field or silently substitutes a value when opening an imported node.
    if (type == "shadowsocks") {
        auto methods = innerWidget->findChild<QComboBox *>("method");
        if (methods) {
            for (const auto &method: Preset::Xray::ShadowsocksMethods + Preset::SingBox::ShadowsocksMethods) {
                if (methods->findText(method) < 0) methods->addItem(method);
            }
            if (methods->findText(ent->ShadowSocksBean()->method) < 0) methods->addItem(ent->ShadowSocksBean()->method);
            methods->setCurrentText(ent->ShadowSocksBean()->method);
        }
    }
    if (type == "vless" || type == "trojan") {
        auto flows = innerWidget->findChild<QComboBox *>("flow");
        if (flows) {
            for (const auto &flow: Preset::Xray::Flows + Preset::SingBox::Flows) {
                if (flows->findText(flow) < 0) flows->addItem(flow);
            }
            if (flows->findText(ent->TrojanVLESSBean()->flow) < 0) flows->addItem(ent->TrojanVLESSBean()->flow);
            flows->setCurrentText(ent->TrojanVLESSBean()->flow);
        }
    }
    if (type == "hysteria" || type == "hysteria2") {
        if (auto forceExternal = innerWidget->findChild<QCheckBox *>("forceExternal")) forceExternal->show();
    }
    if (type == "tuic") {
        if (auto uos = innerWidget->findChild<QCheckBox *>("uos")) uos->show();
    }
    for (auto control: innerWidget->findChildren<QLineEdit *>()) {
        connect(control, &QLineEdit::textChanged, this, [this] { refresh_core_choice(); });
    }
    for (auto control: innerWidget->findChildren<QComboBox *>()) {
        connect(control, &QComboBox::currentTextChanged, this, [this] { refresh_core_choice(); });
    }
    for (auto control: innerWidget->findChildren<QCheckBox *>()) {
        connect(control, &QCheckBox::toggled, this, [this] { refresh_core_choice(); });
    }
    for (auto control: innerWidget->findChildren<QPlainTextEdit *>()) {
        connect(control, &QPlainTextEdit::textChanged, this, [this] { refresh_core_choice(); });
    }

    // 左边 common
    ui->name->setText(ent->bean->name);
    ui->address->setText(ent->bean->serverAddress);
    ui->port->setText(Int2String(ent->bean->serverPort));
    ui->port->setValidator(QRegExpValidator_Number);

    // 星号
    ADD_ASTERISK(this)

    const bool packetEncoding = type == "vmess" || type == "vless";
    ui->packet_encoding->setVisible(packetEncoding);
    ui->packet_encoding_l->setVisible(packetEncoding);
    ui->network->setVisible(stream != nullptr);
    ui->network_l->setVisible(stream != nullptr);
    ui->security->setVisible(stream != nullptr);
    ui->security_l->setVisible(stream != nullptr);
    const bool multiplex = type == "vmess" || type == "vless" || type == "trojan" || type == "shadowsocks";
    ui->multiplex->setVisible(multiplex);
    ui->multiplex_l->setVisible(multiplex);
    ui->stream_box->setVisible(stream != nullptr);

    // 载入 type 之后，有些类型没有右边的设置
    auto rightNoBox = (ui->stream_box->isHidden() && ui->network_box->isHidden() && ui->security_box->isHidden());
    if (rightNoBox && !ui->right_all_w->isHidden()) {
        ui->right_all_w->setVisible(false);
    }

    editor_cache_updated_impl();
    loadingEditor = false;
    refresh_core_choice();
    ADJUST_SIZE

    // 第一次显示
    if (isHidden()) {
        QTimer::singleShot(0, this, [this] { show(); });
    }
}

bool DialogEditProfile::onEnd() {
    // bean
    if (!innerEditor->onEnd()) {
        return false;
    }

    // 左边
    ent->bean->name = ui->name->text();
    ent->bean->serverAddress = ui->address->text().remove(' ');
    ent->bean->serverPort = ui->port->text().toInt();

    save_stream_fields(ent->bean.get());

    // cached custom
    ent->bean->custom_outbound = CACHE.custom_outbound;
    ent->bean->custom_config = CACHE.custom_config;

    return true;
}

void DialogEditProfile::save_stream_fields(NekoGui_fmt::AbstractBean *bean) const {
    auto stream = GetStreamSettings(bean);
    if (stream != nullptr) {
        stream->network = ui->network->currentText();
        stream->security = ui->security->currentText();
        stream->packet_encoding = ui->packet_encoding->currentText();
        stream->path = ui->path->text();
        stream->host = ui->host->text();
        stream->sni = ui->sni->text();
        stream->alpn = ui->alpn->text();
        stream->utlsFingerprint = ui->utlsFingerprint->currentText();
        stream->allow_insecure = ui->insecure->isChecked();
        stream->header_type = ui->header_type->currentText();
        stream->ws_early_data_name = ui->ws_early_data_name->text();
        stream->ws_early_data_length = ui->ws_early_data_length->text().toInt();
        stream->reality_pbk = ui->reality_pbk->text();
        stream->reality_sid = ui->reality_sid->text();
        stream->reality_spx = ui->reality_spx->text();
        stream->multiplex_status = ui->multiplex->currentIndex();
        stream->certificate = CACHE.certificate;
    }

}

void DialogEditProfile::refresh_core_choice() {
    if (loadingEditor || !ent || !innerEditor || !coreChoices) return;
    // These editors only collect field values in onEnd. The custom/chain
    // editors also validate with dialogs, so preview their relevant controls
    // directly and reserve validation for Accept.
    if (type != "custom" && type != "chain") innerEditor->onEnd();
    ent->bean->name = ui->name->text();
    ent->bean->serverAddress = ui->address->text().remove(' ');
    ent->bean->serverPort = ui->port->text().toInt();
    save_stream_fields(ent->bean.get());
    ent->bean->custom_config = CACHE.custom_config;
    ent->bean->custom_outbound = CACHE.custom_outbound;
    ent->core_preference = pendingCorePreference;
    if (type == "custom") {
        if (auto core = innerWidget->findChild<QComboBox *>("core")) ent->CustomBean()->core = core->currentText();
        if (auto config = innerWidget->findChild<QPlainTextEdit *>("config_simple")) ent->CustomBean()->config_simple = config->toPlainText();
    } else if (type == "chain") {
        if (auto list = innerWidget->findChild<QListWidget *>("listWidget")) {
            ent->ChainBean()->list.clear();
            for (int index = 0; index < list->count(); ++index) ent->ChainBean()->list.append(list->item(index)->data(114514).toInt());
        }
    }
    const auto resolution = NekoGui::ResolveCore(ent, NekoGui::PreferredCore());
    coreChoices->button(1)->setEnabled(resolution.xraySupported);
    coreChoices->button(2)->setEnabled(resolution.singBoxSupported);
    coreChoices->button(1)->setToolTip(resolution.xrayReason);
    coreChoices->button(2)->setToolTip(resolution.singBoxReason);
    coreReason->setText(resolution.reason + "\n" + tr("保存后下次连接生效，当前连接保持。"));
}

bool DialogEditProfile::commit_draft() {
    if (newEnt) {
        if (!NekoGui::profileManager->GetGroup(groupId)) {
            MessageBoxWarning(tr("无法保存"), tr("目标分组已被删除，请重新选择分组。"));
            return false;
        }
        ent->core_preference = pendingCorePreference;
        return NekoGui::profileManager->AddProfile(ent, groupId);
    }
    if (!originalEnt || NekoGui::profileManager->GetProfile(originalEnt->id) != originalEnt) {
        MessageBoxWarning(tr("无法保存"), tr("节点已被删除或被订阅替换，请重新打开编辑。"));
        return false;
    }
    const bool configChanged = NekoGui::ProfileFilter::SubscriptionIdentity(originalEnt) !=
                               NekoGui::ProfileFilter::SubscriptionIdentity(ent);
    // Keep current local metadata/history (which may have changed during a
    // batch test), and copy only the edited protocol into a separate bean.
    auto replacement = NekoGui::ProfileManager::NewProxyEntity(type);
    replacement->bean->FromJson(ent->bean->ToJson());
    originalEnt->bean = replacement->bean;
    originalEnt->_get("bean")->ptr = dynamic_cast<JsonStore *>(originalEnt->bean.get());
    if (configChanged) {
        originalEnt->latency = 0;
        originalEnt->full_test_report.clear();
    }
    originalEnt->Save();
    return true;
}

void DialogEditProfile::accept() {
    // save to ent
    if (!onEnd()) {
        return;
    }

    // finish
    QStringList msg = {"accept"};

    const bool configChanged = !newEnt && originalEnt &&
        NekoGui::ProfileFilter::SubscriptionIdentity(originalEnt) != NekoGui::ProfileFilter::SubscriptionIdentity(ent);
    if (!commit_draft()) return;
    auto saved = newEnt ? ent : originalEnt;
    if (auto window = GetMainWindow()) window->set_core_preference(saved->id, pendingCorePreference, false);
    else {
        saved->core_preference = pendingCorePreference;
        saved->Save();
    }
    if (configChanged && NekoGui::dataStore->started_id == saved->id) msg << "restart";

    MW_dialog_message(Dialog_DialogEditProfile, msg.join(","));
    QDialog::accept();
}

// cached editor (dialog)

void DialogEditProfile::editor_cache_updated_impl() {
    if (CACHE.certificate.isEmpty()) {
        ui->certificate_edit->setText(tr("Not set"));
    } else {
        ui->certificate_edit->setText(tr("Already set"));
    }
    if (CACHE.custom_outbound.isEmpty()) {
        ui->custom_outbound_edit->setText(tr("Not set"));
    } else {
        ui->custom_outbound_edit->setText(tr("Already set"));
    }
    if (CACHE.custom_config.isEmpty()) {
        ui->custom_config_edit->setText(tr("Not set"));
    } else {
        ui->custom_config_edit->setText(tr("Already set"));
    }

    // CACHE macro
    for (auto a: innerEditor->get_editor_cached()) {
        if (a.second.isEmpty()) {
            a.first->setText(tr("Not set"));
        } else {
            a.first->setText(tr("Already set"));
        }
    }
    refresh_core_choice();
}

void DialogEditProfile::on_custom_outbound_edit_clicked() {
    C_EDIT_JSON_ALLOW_EMPTY(custom_outbound)
    editor_cache_updated_impl();
}

void DialogEditProfile::on_custom_config_edit_clicked() {
    C_EDIT_JSON_ALLOW_EMPTY(custom_config)
    editor_cache_updated_impl();
}

void DialogEditProfile::on_certificate_edit_clicked() {
    bool ok;
    auto txt = QInputDialog::getMultiLineText(this, tr("Certificate"), "", CACHE.certificate, &ok);
    if (ok) {
        CACHE.certificate = txt;
        editor_cache_updated_impl();
    }
}

void DialogEditProfile::on_apply_to_group_clicked() {
    if (apply_to_group_ui.empty()) {
        apply_to_group_ui[ui->multiplex] = new FloatCheckBox(ui->multiplex, this);
        apply_to_group_ui[ui->sni] = new FloatCheckBox(ui->sni, this);
        apply_to_group_ui[ui->alpn] = new FloatCheckBox(ui->alpn, this);
        apply_to_group_ui[ui->host] = new FloatCheckBox(ui->host, this);
        apply_to_group_ui[ui->path] = new FloatCheckBox(ui->path, this);
        apply_to_group_ui[ui->utlsFingerprint] = new FloatCheckBox(ui->utlsFingerprint, this);
        apply_to_group_ui[ui->insecure] = new FloatCheckBox(ui->insecure, this);
        apply_to_group_ui[ui->certificate_edit] = new FloatCheckBox(ui->certificate_edit, this);
        apply_to_group_ui[ui->custom_config_edit] = new FloatCheckBox(ui->custom_config_edit, this);
        apply_to_group_ui[ui->custom_outbound_edit] = new FloatCheckBox(ui->custom_outbound_edit, this);
        ui->apply_to_group->setText(tr("Confirm"));
    } else {
        auto group = NekoGui::profileManager->GetGroup(ent->gid);
        if (group == nullptr) {
            MessageBoxWarning("failed", "unknown group");
            return;
        }
        // save this
        if (!onEnd() || !commit_draft()) {
            MessageBoxWarning("failed", "failed to save");
            return;
        }
        // copy keys
        for (const auto &pair: apply_to_group_ui) {
            if (pair.second->isChecked()) {
                do_apply_to_group(group, pair.first);
            }
            delete pair.second;
        }
        apply_to_group_ui.clear();
        ui->apply_to_group->setText(tr("Apply settings to this group"));
    }
}

void DialogEditProfile::do_apply_to_group(const std::shared_ptr<NekoGui::Group> &group, QWidget *key) {
    auto stream = GetStreamSettings(ent->bean.get());

    auto copyStream = [=](void *p) {
        for (const auto &profile: group->Profiles()) {
            auto newStream = GetStreamSettings(profile->bean.get());
            if (newStream == nullptr) continue;
            if (stream == newStream) continue;
            newStream->_setValue(stream->_name(p), p);
            // qDebug() << newStream->ToJsonBytes();
            profile->Save();
        }
    };

    auto copyBean = [=](void *p) {
        for (const auto &profile: group->Profiles()) {
            if (profile == ent) continue;
            profile->bean->_setValue(ent->bean->_name(p), p);
            // qDebug() << profile->bean->ToJsonBytes();
            profile->Save();
        }
    };

    if (key == ui->multiplex) {
        copyStream(&stream->multiplex_status);
    } else if (key == ui->sni) {
        copyStream(&stream->sni);
    } else if (key == ui->alpn) {
        copyStream(&stream->alpn);
    } else if (key == ui->host) {
        copyStream(&stream->host);
    } else if (key == ui->path) {
        copyStream(&stream->path);
    } else if (key == ui->utlsFingerprint) {
        copyStream(&stream->utlsFingerprint);
    } else if (key == ui->insecure) {
        copyStream(&stream->allow_insecure);
    } else if (key == ui->certificate_edit) {
        copyStream(&stream->certificate);
    } else if (key == ui->custom_config_edit) {
        copyBean(&ent->bean->custom_config);
    } else if (key == ui->custom_outbound_edit) {
        copyBean(&ent->bean->custom_outbound);
    }
}
