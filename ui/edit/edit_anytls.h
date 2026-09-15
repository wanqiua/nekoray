#pragma once

#include <QWidget>
#include "profile_editor.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QToolButton;

class EditAnyTLS : public QWidget, public ProfileEditor {
    Q_OBJECT
public:
    explicit EditAnyTLS(QWidget *parent = nullptr);
    void onStart(std::shared_ptr<NekoGui::ProxyEntity> ent) override;
    bool onEnd() override;

private:
    std::shared_ptr<NekoGui::ProxyEntity> ent;
    QLineEdit *password, *sni, *alpn, *checkInterval, *idleTimeout, *minIdleSession, *metadata;
    QCheckBox *insecure;
    QComboBox *fingerprint;
    QPlainTextEdit *certificate;
    QToolButton *advancedToggle;
    QString originalCertificate, initialCertificateDisplay;
};
