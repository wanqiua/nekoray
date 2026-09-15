#include "edit_anytls.h"
#include "fmt/AnyTLSBean.hpp"
#include "fmt/Preset.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <limits>

EditAnyTLS::EditAnyTLS(QWidget *parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this);
    auto basic = new QFormLayout;
    auto line = [&](const char *name, QWidget *owner = nullptr) {
        auto field = new QLineEdit(owner ? owner : this);
        field->setObjectName(name);
        field->setMaxLength(std::numeric_limits<int>::max());
        return field;
    };
    password = line("anytlsPassword");
    password->setEchoMode(QLineEdit::Password);
    basic->addRow(tr("密码"), password);
    auto reveal = new QCheckBox(tr("显示密码"), this);
    reveal->setObjectName("anytlsShowPassword");
    connect(reveal, &QCheckBox::toggled, this, [this](bool checked) {
        password->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    });
    basic->addRow(QString{}, reveal);
    sni = line("anytlsSni");
    sni->setPlaceholderText(tr("留空使用服务器域名"));
    basic->addRow(tr("TLS 服务器名称"), sni);
    insecure = new QCheckBox(tr("跳过证书验证"), this);
    insecure->setObjectName("anytlsInsecure");
    basic->addRow(QString{}, insecure);
    layout->addLayout(basic);

    advancedToggle = new QToolButton(this);
    advancedToggle->setObjectName("anytlsAdvancedToggle");
    advancedToggle->setText(tr("高级设置"));
    advancedToggle->setCheckable(true);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setArrowType(Qt::RightArrow);
    layout->addWidget(advancedToggle);
    auto scroll = new QScrollArea(this);
    scroll->setObjectName("anytlsAdvanced");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setMaximumHeight(290);
    auto advanced = new QWidget(scroll);
    auto form = new QFormLayout(advanced);
    form->setContentsMargins(0, 0, 0, 0);
    alpn = line("anytlsAlpn", advanced);
    alpn->setPlaceholderText(tr("可选，以逗号分隔"));
    form->addRow("ALPN", alpn);
    fingerprint = new QComboBox(advanced);
    fingerprint->setObjectName("anytlsFingerprint");
    fingerprint->addItems(Preset::SingBox::UtlsFingerPrint);
    form->addRow(tr("uTLS 指纹"), fingerprint);
    certificate = new QPlainTextEdit(advanced);
    certificate->setObjectName("anytlsCertificate");
    certificate->setPlaceholderText(tr("可选，自定义 CA 或服务器证书（PEM）"));
    certificate->setMaximumHeight(85);
    form->addRow(tr("证书"), certificate);
    checkInterval = line("anytlsCheckInterval", advanced);
    idleTimeout = line("anytlsIdleTimeout", advanced);
    for (auto duration : {checkInterval, idleTimeout}) {
        duration->setPlaceholderText(tr("默认 30s；自定义值须大于 5s"));
        duration->setToolTip(tr("支持 10s、1m 等时长。留空使用默认值。"));
    }
    form->addRow(tr("空闲检查间隔"), checkInterval);
    form->addRow(tr("空闲会话超时"), idleTimeout);
    minIdleSession = line("anytlsMinIdleSession", advanced);
    minIdleSession->setValidator(new QIntValidator(0, std::numeric_limits<int>::max(), minIdleSession));
    minIdleSession->setPlaceholderText("0");
    form->addRow(tr("保留空闲会话数"), minIdleSession);
    metadata = line("anytlsMetadata", advanced);
    form->addRow(tr("客户端元数据"), metadata);
    scroll->setWidget(advanced);
    scroll->hide();
    layout->addWidget(scroll);
    connect(advancedToggle, &QToolButton::toggled, this, [this, scroll](bool checked) {
        advancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        scroll->setVisible(checked);
        QTimer::singleShot(0, this, [this] { window()->adjustSize(); });
    });
}

void EditAnyTLS::onStart(std::shared_ptr<NekoGui::ProxyEntity> value) {
    ent = std::move(value);
    auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(ent->bean.get());
    password->setText(bean->password);
    sni->setText(bean->sni);
    insecure->setChecked(bean->allowInsecure);
    alpn->setText(bean->alpn);
    if (fingerprint->findText(bean->utlsFingerprint) < 0) fingerprint->addItem(bean->utlsFingerprint);
    fingerprint->setCurrentText(bean->utlsFingerprint);
    originalCertificate = bean->caText;
    certificate->setPlainText(originalCertificate);
    initialCertificateDisplay = certificate->toPlainText();
    checkInterval->setText(bean->idleSessionCheckInterval);
    idleTimeout->setText(bean->idleSessionTimeout);
    minIdleSession->setText(QString::number(bean->minIdleSession));
    metadata->setText(bean->clientMetadata);
    const bool configured = !bean->alpn.isEmpty() || !bean->utlsFingerprint.isEmpty() || !bean->caText.isEmpty() ||
                            !bean->idleSessionCheckInterval.isEmpty() || !bean->idleSessionTimeout.isEmpty() ||
                            bean->minIdleSession != 0 || !bean->clientMetadata.isEmpty();
    advancedToggle->setText(configured ? tr("高级设置（已配置）") : tr("高级设置"));
}

bool EditAnyTLS::onEnd() {
    if (!ent) return false;
    auto bean = dynamic_cast<NekoGui_fmt::AnyTLSBean *>(ent->bean.get());
    bean->password = password->text();
    bean->sni = sni->text().trimmed();
    bean->allowInsecure = insecure->isChecked();
    bean->alpn = alpn->text().trimmed();
    bean->utlsFingerprint = fingerprint->currentText();
    const auto certificateDisplay = certificate->toPlainText();
    bean->caText = certificateDisplay == initialCertificateDisplay ? originalCertificate : certificateDisplay;
    bean->idleSessionCheckInterval = checkInterval->text().trimmed();
    bean->idleSessionTimeout = idleTimeout->text().trimmed();
    bool numberValid = true;
    bean->minIdleSession = minIdleSession->text().isEmpty() ? 0 : minIdleSession->text().toInt(&numberValid);
    if (!numberValid) bean->minIdleSession = -1;
    bean->clientMetadata = metadata->text();
    return true;
}
