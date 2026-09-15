#pragma once

#include <QWidget>
#include "profile_editor.h"

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QToolButton;

class EditSnell : public QWidget, public ProfileEditor {
    Q_OBJECT
public:
    explicit EditSnell(QWidget *parent = nullptr);
    void onStart(std::shared_ptr<NekoGui::ProxyEntity> ent) override;
    bool onEnd() override;

private:
    void updateVersionFields();
    std::shared_ptr<NekoGui::ProxyEntity> ent;
    QComboBox *version, *network, *obfsMode, *mode;
    QLineEdit *psk, *userkey, *obfsHost;
    QCheckBox *reuse;
    QFormLayout *advancedForm;
    QToolButton *advancedToggle;
};
