#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

class UrlImportDialog : public QDialog {
public:
    enum Choice { Cancel = QDialog::Rejected, AddToGroup = 1, NewGroup = 2, Link = 3 };

    explicit UrlImportDialog(const QString &url, QWidget *parent = nullptr) : QDialog(parent) {
        setObjectName("urlImportDialog");
        setWindowTitle(QObject::tr("url detected"));
        setMinimumWidth(400);
        auto layout = new QVBoxLayout(this);
        auto label = new QLabel(QObject::tr("%1\nHow to update?").arg(url), this);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);

        auto addChoice = [this, layout](const QString &text, const char *name, Choice choice) {
            auto button = new QPushButton(text, this);
            button->setObjectName(name);
            button->setAutoDefault(false);
            connect(button, &QPushButton::clicked, this, [this, choice] { done(choice); });
            layout->addWidget(button);
        };
        addChoice(QObject::tr("As Subscription (add to this group)"), "importSubscriptionCurrent", AddToGroup);
        addChoice(QObject::tr("As Subscription (create new group)"), "importSubscriptionNew", NewGroup);
        addChoice(QObject::tr("As link"), "importLink", Link);

        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }
};
