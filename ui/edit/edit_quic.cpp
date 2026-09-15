#include "edit_quic.h"
#include "ui_edit_quic.h"

#include "fmt/QUICBean.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <limits>

EditQUIC::EditQUIC(QWidget *parent) : QWidget(parent), ui(new Ui::EditQUIC) {
    ui->setupUi(this);
    connect(ui->uuidgen, &QPushButton::clicked, this, [=] { ui->uuid->setText(QUuid::createUuid().toString().remove("{").remove("}")); });

    hy2AdvancedToggle = new QToolButton(this);
    hy2AdvancedToggle->setObjectName("hy2AdvancedToggle");
    hy2AdvancedToggle->setText(tr("高级设置"));
    hy2AdvancedToggle->setCheckable(true);
    hy2AdvancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    hy2AdvancedToggle->setArrowType(Qt::RightArrow);
    hy2AdvancedToggle->hide();
    ui->verticalLayout->addWidget(hy2AdvancedToggle);

    hy2Advanced = new QScrollArea(this);
    hy2Advanced->setObjectName("hy2Advanced");
    hy2Advanced->setFrameShape(QFrame::NoFrame);
    hy2Advanced->setWidgetResizable(true);
    hy2Advanced->setMaximumHeight(300);
    auto advanced = new QWidget(hy2Advanced);
    auto form = new QFormLayout(advanced);
    form->setContentsMargins(0, 0, 0, 0);
    auto line = [advanced](const char *name) {
        auto field = new QLineEdit(advanced);
        field->setObjectName(name);
        field->setMaxLength(std::numeric_limits<int>::max());
        return field;
    };

    hy2BbrProfile = new QComboBox(advanced);
    hy2BbrProfile->setObjectName("hy2BbrProfile");
    hy2BbrProfile->addItem(tr("默认（standard）"), QString{});
    hy2BbrProfile->addItem(tr("标准（standard）"), "standard");
    hy2BbrProfile->addItem(tr("保守（conservative）"), "conservative");
    hy2BbrProfile->addItem(tr("激进（aggressive）"), "aggressive");
    form->addRow(tr("BBR 配置"), hy2BbrProfile);
    auto bandwidthHint = new QLabel(tr("上、下行带宽为 0 时使用 BBR；选择配置不会修改带宽。"), advanced);
    bandwidthHint->setObjectName("hy2BbrHint");
    bandwidthHint->setWordWrap(true);
    form->addRow(bandwidthHint);

    hy2ObfsType = new QComboBox(advanced);
    hy2ObfsType->setObjectName("hy2ObfsType");
    hy2ObfsType->addItem(tr("默认（按混淆密码判断）"), QString{});
    hy2ObfsType->addItem(tr("关闭"), "none");
    hy2ObfsType->addItem("Salamander", "salamander");
    hy2ObfsType->addItem("Gecko", "gecko");
    hy2ObfsType->setToolTip(tr("默认模式下，有混淆密码时使用 Salamander。选择关闭时请清空上方混淆密码。"));
    form->addRow(tr("混淆类型"), hy2ObfsType);

    hy2GeckoMinPacketSize = line("hy2GeckoMinPacketSize");
    hy2GeckoMaxPacketSize = line("hy2GeckoMaxPacketSize");
    hy2GeckoMinPacketSize->setPlaceholderText("512");
    hy2GeckoMaxPacketSize->setPlaceholderText("1200");
    auto packetSizes = new QHBoxLayout;
    for (auto field : {hy2GeckoMinPacketSize, hy2GeckoMaxPacketSize}) {
        field->setValidator(new QRegularExpressionValidator(QRegularExpression("^[0-9]+$"), field));
        field->setToolTip(tr("仅用于 Gecko，范围 1–2048 字节，最大值不得小于最小值。0 或留空使用默认值 512/1200；其他混淆类型应保留默认范围。"));
        if (field == hy2GeckoMaxPacketSize) packetSizes->addWidget(new QLabel(tr("至"), advanced));
        packetSizes->addWidget(field);
    }
    form->addRow(tr("Gecko 包长（字节）"), packetSizes);

    hy2DisableChromeParrot = new QCheckBox(tr("关闭 Chrome QUIC 指纹模拟"), advanced);
    hy2DisableChromeParrot->setObjectName("hy2DisableChromeParrot");
    hy2DisableChromeParrot->setToolTip(tr("默认启用 Chrome QUIC 指纹模拟；连接使用 Ed25519 证书的服务端时可关闭。"));
    form->addRow(QString{}, hy2DisableChromeParrot);

    hy2HopIntervalMin = line("hy2HopIntervalMin");
    hy2HopIntervalMin->setPlaceholderText(tr("留空使用上方间隔（秒）"));
    hy2HopIntervalMin->setToolTip(tr("需先填写跳端口。支持 10s、1m30s 等时长，最小间隔不得小于 5 秒；0 使用默认 30 秒。"));
    form->addRow(tr("最小跳端口间隔"), hy2HopIntervalMin);
    hy2HopIntervalMax = line("hy2HopIntervalMax");
    hy2HopIntervalMax->setPlaceholderText(tr("留空使用固定间隔"));
    hy2HopIntervalMax->setToolTip(tr("设置后在最小、最大间隔之间随机跳端口；最大值不得小于最小值，且最小值不能为 0。"));
    form->addRow(tr("最大跳端口间隔"), hy2HopIntervalMax);

    hy2Advanced->setWidget(advanced);
    hy2Advanced->hide();
    ui->verticalLayout->addWidget(hy2Advanced);
    connect(hy2AdvancedToggle, &QToolButton::toggled, this, [this](bool checked) {
        hy2AdvancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        hy2Advanced->setVisible(checked);
        QTimer::singleShot(0, this, [this] { window()->adjustSize(); });
    });
    for (auto combo : {hy2BbrProfile, hy2ObfsType}) {
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateHysteria2AdvancedState(); });
    }
    for (auto field : {hy2GeckoMinPacketSize, hy2GeckoMaxPacketSize, hy2HopIntervalMin, hy2HopIntervalMax}) {
        connect(field, &QLineEdit::textChanged, this, [this] { updateHysteria2AdvancedState(); });
    }
    connect(hy2DisableChromeParrot, &QCheckBox::toggled, this, [this] { updateHysteria2AdvancedState(); });
}

EditQUIC::~EditQUIC() {
    delete ui;
}

void EditQUIC::updateHysteria2AdvancedState() {
    const auto bbr = hy2BbrProfile->currentData().toString();
    const auto customSize = [](const QLineEdit *field, const QString &defaultValue) {
        const auto value = field->text();
        return !value.isEmpty() && value != "0" && value != defaultValue;
    };
    const bool configured = (!bbr.isEmpty() && bbr != "standard") || !hy2ObfsType->currentData().toString().isEmpty() ||
                            customSize(hy2GeckoMinPacketSize, "512") || customSize(hy2GeckoMaxPacketSize, "1200") ||
                            hy2DisableChromeParrot->isChecked() || !hy2HopIntervalMin->text().isEmpty() ||
                            !hy2HopIntervalMax->text().isEmpty();
    hy2AdvancedToggle->setText(configured ? tr("高级设置（已配置）") : tr("高级设置"));
}

void EditQUIC::onStart(std::shared_ptr<NekoGui::ProxyEntity> _ent) {
    this->ent = _ent;
    auto bean = this->ent->QUICBean();

    const bool hysteria2 = bean->proxy_type == NekoGui_fmt::QUICBean::proxy_Hysteria2;
    if (hysteria2) {
        // An imported string must not be truncated just by opening the editor.
        for (auto field : findChildren<QLineEdit *>()) field->setMaxLength(std::numeric_limits<int>::max());
    }

    P_LOAD_STRING(hopPort);
    P_LOAD_INT(hopInterval);
    P_LOAD_INT(uploadMbps);
    P_LOAD_INT(downloadMbps);
    P_LOAD_BOOL(disableMtuDiscovery)
    P_LOAD_STRING(obfsPassword);
    P_LOAD_STRING(authPayload);
    P_LOAD_INT(streamReceiveWindow);
    P_LOAD_INT(connectionReceiveWindow);

    P_LOAD_BOOL(forceExternal);
    P_LOAD_COMBO_INT(hyProtocol);
    P_LOAD_COMBO_INT(authPayloadType);
    P_LOAD_STRING(uuid);
    P_LOAD_STRING(password);

    P_LOAD_COMBO_STRING(congestionControl);
    P_LOAD_COMBO_STRING(udpRelayMode);
    P_LOAD_BOOL(zeroRttHandshake);
    P_LOAD_STRING(heartbeat);
    P_LOAD_BOOL(uos);

    // TLS
    P_LOAD_STRING(sni);
    P_LOAD_STRING(alpn);
    P_C_LOAD_STRING(caText);
    P_LOAD_BOOL(allowInsecure);
    P_LOAD_BOOL(disableSni);

    hy2AdvancedToggle->setChecked(false);
    hy2AdvancedToggle->setVisible(hysteria2);
    hy2Advanced->hide();
    if (hysteria2) {
        // Keep explicit defaults and unknown imported values distinct. The
        // profile dialog validates on Save, while preview only collects data.
        auto select = [](QComboBox *combo, const QString &value) {
            if (combo->findData(value) < 0) combo->addItem(value, value);
            combo->setCurrentIndex(combo->findData(value));
        };
        select(hy2BbrProfile, bean->bbrProfile);
        select(hy2ObfsType, bean->obfsType);
        hy2GeckoMinPacketSize->setText(QString::number(bean->geckoMinPacketSize));
        hy2GeckoMaxPacketSize->setText(QString::number(bean->geckoMaxPacketSize));
        hy2DisableChromeParrot->setChecked(bean->disableChromeParrot);
        hy2HopIntervalMin->setText(bean->hopIntervalMin);
        hy2HopIntervalMax->setText(bean->hopIntervalMax);
        updateHysteria2AdvancedState();
        ui->uploadMbps->setToolTip(tr("0 表示使用 BBR；单位为 Mbps。"));
        ui->downloadMbps->setToolTip(ui->uploadMbps->toolTip());
        ui->hopPort->setPlaceholderText(tr("例：443,2000-3000"));
        ui->hopPort->setToolTip(tr("可用逗号分隔多个端口或范围，例如 443,2000-3000；范围也可写为 2000:3000。"));
    }

    if (bean->proxy_type == NekoGui_fmt::QUICBean::proxy_Hysteria || bean->proxy_type == NekoGui_fmt::QUICBean::proxy_Hysteria2) {
        ui->uuid->hide();
        ui->uuid_l->hide();
        ui->uuidgen->hide();
        ui->congestionControl->hide();
        ui->congestionControl_l->hide();
        ui->udpRelayMode->hide();
        ui->udpRelayMode_l->hide();
        ui->zeroRttHandshake->hide();
        ui->heartbeat->hide();
        ui->heartbeat_l->hide();
        ui->uos->hide();
        if (!IS_NEKO_BOX) ui->forceExternal->hide();

        if (bean->proxy_type == NekoGui_fmt::QUICBean::proxy_Hysteria) { // hy1
            ui->password->hide();
            ui->password_l->hide();
        } else { // hy2
            ui->hyProtocol->hide();
            ui->hyProtocol_l->hide();
            ui->hyProtocol->hide();
            ui->hyProtocol_l->hide();
            ui->authPayload->hide();
            ui->authPayload_l->hide();
            ui->authPayloadType->hide();
            ui->authPayloadType_l->hide();
            ui->alpn->hide();
            ui->alpn_l->hide();
            ui->TLS->removeItem(ui->alpn_sp);
            if (IS_NEKO_BOX) {
                ui->disableMtuDiscovery->hide();
                ui->connectionReceiveWindow->hide();
                ui->connectionReceiveWindow_l->hide();
                ui->streamReceiveWindow->hide();
                ui->streamReceiveWindow_l->hide();
            }
        }
    } else if (bean->proxy_type == NekoGui_fmt::QUICBean::proxy_TUIC) {
        ui->hopPort->hide();
        ui->hopPort_l->hide();
        ui->hopInterval->hide();
        ui->hopInterval_l->hide();
        ui->uploadMbps->hide();
        ui->uploadMbps_l->hide();
        ui->downloadMbps->hide();
        ui->downloadMbps_l->hide();
        ui->hyProtocol->hide();
        ui->hyProtocol_l->hide();
        ui->disableMtuDiscovery->hide();
        ui->obfsPassword->hide();
        ui->obfsPassword_l->hide();
        ui->authPayload->hide();
        ui->authPayload_l->hide();
        ui->authPayloadType->hide();
        ui->authPayloadType_l->hide();
        ui->streamReceiveWindow->hide();
        ui->streamReceiveWindow_l->hide();
        ui->connectionReceiveWindow->hide();
        ui->connectionReceiveWindow_l->hide();
        if (!IS_NEKO_BOX) {
            ui->uos->hide();
        }
    }
}

bool EditQUIC::onEnd() {
    if (!ent) return false;
    auto bean = this->ent->QUICBean();

    P_SAVE_BOOL(forceExternal);

    // Hysteria
    P_SAVE_STRING(hopPort);
    if (bean->proxy_type == NekoGui_fmt::QUICBean::proxy_Hysteria2) {
        const auto integer = [](const QLineEdit *field) {
            bool valid = true;
            const int value = field->text().isEmpty() ? 0 : field->text().toInt(&valid);
            return valid ? value : -1;
        };
        const auto windowSize = [](const QLineEdit *field) {
            bool valid = true;
            const qint64 value = field->text().isEmpty() ? 0 : field->text().toLongLong(&valid);
            return valid ? value : qint64(-1);
        };
        bean->hopInterval = integer(ui->hopInterval);
        bean->uploadMbps = integer(ui->uploadMbps);
        bean->downloadMbps = integer(ui->downloadMbps);
        // Receive windows are stored as 64-bit values, including when hidden.
        bean->streamReceiveWindow = windowSize(ui->streamReceiveWindow);
        bean->connectionReceiveWindow = windowSize(ui->connectionReceiveWindow);
        bean->bbrProfile = hy2BbrProfile->currentData().toString();
        bean->obfsType = hy2ObfsType->currentData().toString();
        bean->geckoMinPacketSize = integer(hy2GeckoMinPacketSize);
        bean->geckoMaxPacketSize = integer(hy2GeckoMaxPacketSize);
        bean->disableChromeParrot = hy2DisableChromeParrot->isChecked();
        bean->hopIntervalMin = hy2HopIntervalMin->text();
        bean->hopIntervalMax = hy2HopIntervalMax->text();
    } else {
        P_SAVE_INT(hopInterval);
        P_SAVE_INT(uploadMbps);
        P_SAVE_INT(downloadMbps);
        P_SAVE_INT(streamReceiveWindow);
        P_SAVE_INT(connectionReceiveWindow);
    }
    P_SAVE_COMBO_INT(hyProtocol);
    P_SAVE_BOOL(disableMtuDiscovery)
    P_SAVE_STRING(obfsPassword);
    P_SAVE_COMBO_INT(authPayloadType);
    P_SAVE_STRING(authPayload);

    // TUIC
    P_SAVE_STRING(uuid);
    P_SAVE_STRING(password);
    P_SAVE_COMBO_STRING(congestionControl);
    P_SAVE_COMBO_STRING(udpRelayMode);
    P_SAVE_BOOL(zeroRttHandshake);
    P_SAVE_STRING(heartbeat);
    P_SAVE_BOOL(uos);

    // TLS
    P_SAVE_STRING(sni);
    P_SAVE_STRING(alpn);
    P_SAVE_BOOL(allowInsecure);
    P_C_SAVE_STRING(caText);
    P_SAVE_BOOL(disableSni);
    return true;
}

QList<QPair<QPushButton *, QString>> EditQUIC::get_editor_cached() {
    return {
        {ui->certificate, CACHE.caText},
    };
}

void EditQUIC::on_certificate_clicked() {
    bool ok;
    auto txt = QInputDialog::getMultiLineText(this, tr("Certificate"), "", CACHE.caText, &ok);
    if (ok) {
        CACHE.caText = txt;
        editor_cache_updated();
    }
}
