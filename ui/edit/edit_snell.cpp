#include "edit_snell.h"
#include "fmt/SnellBean.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <limits>

EditSnell::EditSnell(QWidget *parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this);
    auto basic = new QFormLayout;
    version = new QComboBox(this);
    version->setObjectName("snellVersion");
    version->addItem(tr("版本 4（兼容 v5 服务端）"), 4);
    version->addItem(tr("版本 6"), 6);
    basic->addRow(tr("协议版本"), version);
    psk = new QLineEdit(this);
    psk->setObjectName("snellPsk");
    psk->setMaxLength(std::numeric_limits<int>::max());
    psk->setEchoMode(QLineEdit::Password);
    basic->addRow(tr("密钥（PSK）"), psk);
    auto reveal = new QCheckBox(tr("显示密钥"), this);
    reveal->setObjectName("snellShowKeys");
    basic->addRow(QString{}, reveal);
    layout->addLayout(basic);
    advancedToggle = new QToolButton(this);
    advancedToggle->setObjectName("snellAdvancedToggle");
    advancedToggle->setText(tr("高级设置"));
    advancedToggle->setCheckable(true);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setArrowType(Qt::RightArrow);
    layout->addWidget(advancedToggle);
    auto advanced = new QWidget(this);
    advanced->setObjectName("snellAdvanced");
    advancedForm = new QFormLayout(advanced);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    network = new QComboBox(advanced);
    network->setObjectName("snellNetwork");
    network->addItem(tr("TCP 和 UDP"), "");
    network->addItem(tr("仅 TCP"), "tcp");
    network->addItem(tr("仅 UDP（不适用 URL 测试）"), "udp");
    advancedForm->addRow(tr("代理流量"), network);
    userkey = new QLineEdit(advanced);
    userkey->setObjectName("snellUserkey");
    userkey->setMaxLength(std::numeric_limits<int>::max());
    userkey->setEchoMode(QLineEdit::Password);
    advancedForm->addRow(tr("用户密钥"), userkey);
    reuse = new QCheckBox(tr("复用连接"), advanced);
    reuse->setObjectName("snellReuse");
    advancedForm->addRow(QString{}, reuse);
    obfsMode = new QComboBox(advanced);
    obfsMode->setObjectName("snellObfsMode");
    obfsMode->addItem(tr("无"), "");
    obfsMode->addItem("HTTP", "http");
    obfsMode->addItem("TLS", "tls");
    advancedForm->addRow(tr("混淆模式"), obfsMode);
    obfsHost = new QLineEdit(advanced);
    obfsHost->setObjectName("snellObfsHost");
    advancedForm->addRow(tr("混淆域名"), obfsHost);
    mode = new QComboBox(advanced);
    mode->setObjectName("snellMode");
    mode->addItem(tr("默认"), "");
    mode->addItem("unshaped", "unshaped");
    mode->addItem("unsafe-raw", "unsafe-raw");
    advancedForm->addRow(tr("流量整形"), mode);
    layout->addWidget(advanced);
    advanced->hide();
    connect(advancedToggle, &QToolButton::toggled, this, [this, advanced](bool checked) {
        advancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        advanced->setVisible(checked);
        QTimer::singleShot(0, this, [this] { window()->adjustSize(); });
    });
    connect(reveal, &QCheckBox::toggled, this, [this](bool checked) {
        psk->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
        userkey->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(version, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateVersionFields(); });
    connect(obfsMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateVersionFields(); });
    updateVersionFields();
}

void EditSnell::updateVersionFields() {
    const bool v4 = version->currentData().toInt() == 4;
    for (auto field : {static_cast<QWidget *>(obfsMode), static_cast<QWidget *>(obfsHost)}) {
        field->setVisible(v4);
        advancedForm->labelForField(field)->setVisible(v4);
    }
    mode->setVisible(!v4);
    advancedForm->labelForField(mode)->setVisible(!v4);
    const auto obfs = obfsMode->currentData().toString();
    obfsHost->setEnabled(v4 && (obfs == "http" || obfs == "tls"));
    psk->setPlaceholderText(v4 ? QString{} : tr("12–255 个 UTF-8 字节"));
}

void EditSnell::onStart(std::shared_ptr<NekoGui::ProxyEntity> value) {
    ent = std::move(value);
    auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(ent->bean.get());
    auto select = [](QComboBox *combo, const QVariant &value) {
        if (combo->findData(value) < 0) combo->addItem(value.toString(), value);
        combo->setCurrentIndex(combo->findData(value));
    };
    const QSignalBlocker blockVersion(version), blockObfs(obfsMode);
    select(version, bean->snellVersion);
    psk->setText(bean->psk);
    userkey->setText(bean->userkey);
    reuse->setChecked(bean->reuse);
    select(network, bean->network);
    select(obfsMode, bean->obfsMode);
    obfsHost->setText(bean->obfsHost);
    select(mode, bean->mode);
    updateVersionFields();
    const bool configured = !bean->userkey.isEmpty() || bean->reuse || !bean->network.isEmpty() ||
                            !bean->obfsMode.isEmpty() || !bean->obfsHost.isEmpty() || !bean->mode.isEmpty();
    advancedToggle->setText(configured ? tr("高级设置（已配置）") : tr("高级设置"));
}

bool EditSnell::onEnd() {
    if (!ent) return false;
    auto bean = dynamic_cast<NekoGui_fmt::SnellBean *>(ent->bean.get());
    bean->snellVersion = version->currentData().toInt();
    bean->psk = psk->text();
    bean->userkey = userkey->text();
    bean->reuse = reuse->isChecked();
    bean->network = network->currentData().toString();
    // Each version retains its draft controls when switching back, but only
    // that version's settings are committed to the profile.
    const bool v4 = bean->snellVersion == 4;
    bean->obfsMode = v4 ? obfsMode->currentData().toString() : QString{};
    bean->obfsHost = v4 && (bean->obfsMode == "http" || bean->obfsMode == "tls") ? obfsHost->text().trimmed() : QString{};
    bean->mode = v4 ? QString{} : mode->currentData().toString();
    return true;
}
