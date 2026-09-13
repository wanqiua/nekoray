#include "dialog_edit_group.h"
#include "ui_dialog_edit_group.h"

#include "db/Database.hpp"
#include "ui/mainwindow_interface.h"

#include <QClipboard>
#include <QTimer>

DialogEditGroup::DialogEditGroup(const std::shared_ptr<NekoGui::Group> &ent, QWidget *parent) : QDialog(parent), ui(new Ui::DialogEditGroup) {
    ui->setupUi(this);
    this->ent = ent;

    connect(ui->type_subscription, &QRadioButton::toggled, this, [this](bool subscription) {
        ui->cat_sub->setVisible(subscription);
        QTimer::singleShot(0, this, [this] { adjustSize(); adjustPosition(mainwindow); });
    });

    ui->name->setText(ent->name);
    ui->archive->setChecked(ent->archive);
    ui->skip_auto_update->setChecked(ent->skip_auto_update);
    ui->url->setText(ent->url);
    ui->type_basic->setChecked(ent->url.isEmpty());
    ui->type_subscription->setChecked(!ent->url.isEmpty());
    ui->cat_sub->setVisible(!ent->url.isEmpty());
    ui->manually_column_width->setChecked(ent->manually_column_width);
    ui->cat_share->setVisible(false);

    if (ent->id >= 0) { // already a group
        ui->type_basic->setDisabled(true);
        ui->type_subscription->setDisabled(true);
        if (!ent->Profiles().isEmpty()) {
            ui->cat_share->setVisible(true);
        }
    } else { // new group
        ui->front_proxy->hide();
        ui->front_proxy_l->hide();
        ui->front_proxy_clear->hide();
    }

    CACHE.front_proxy = ent->front_proxy_id;
    refresh_front_proxy();
    connect(ui->front_proxy_clear, &QPushButton::clicked, this, [=] {
        CACHE.front_proxy = -1;
        refresh_front_proxy();
    });

    connect(ui->copy_links, &QPushButton::clicked, this, [=] {
        QStringList links;
        for (const auto &[_, profile]: NekoGui::profileManager->profiles) {
            if (profile->gid != ent->id) continue;
            links += profile->bean->ToShareLink();
        }
        QApplication::clipboard()->setText(links.join("\n"));
        MessageBoxInfo(software_name, tr("Copied"));
    });
    connect(ui->copy_links_nkr, &QPushButton::clicked, this, [=] {
        QStringList links;
        for (const auto &[_, profile]: NekoGui::profileManager->profiles) {
            if (profile->gid != ent->id) continue;
            links += profile->bean->ToNekorayShareLink(profile->type);
        }
        QApplication::clipboard()->setText(links.join("\n"));
        MessageBoxInfo(software_name, tr("Copied"));
    });

    QTimer::singleShot(0, this, [this] { adjustSize(); adjustPosition(mainwindow); });
}

DialogEditGroup::~DialogEditGroup() {
    delete ui;
}

void DialogEditGroup::accept() {
    const auto url = ui->url->text().trimmed();
    if (ui->type_subscription->isChecked() && url.isEmpty()) {
        MessageBoxWarning(tr("Warning"), tr("Please input URL"));
        return;
    }
    ent->name = ui->name->text();
    ent->url = ui->type_subscription->isChecked() ? url : QString();
    ent->archive = ui->archive->isChecked();
    ent->skip_auto_update = ui->skip_auto_update->isChecked();
    ent->manually_column_width = ui->manually_column_width->isChecked();
    ent->front_proxy_id = CACHE.front_proxy;
    QDialog::accept();
}

void DialogEditGroup::refresh_front_proxy() {
    auto fEnt = NekoGui::profileManager->GetProfile(CACHE.front_proxy);
    ui->front_proxy->setText(fEnt == nullptr ? tr("None") : fEnt->bean->DisplayTypeAndName());
}

void DialogEditGroup::on_front_proxy_clicked() {
    auto parentWindow = qobject_cast<QWidget *>(parent());
    if (parentWindow != nullptr && parentWindow != GetMainWindow()) parentWindow->hide();
    this->hide();
    GetMainWindow()->start_select_mode(this, [=](int id) {
        CACHE.front_proxy = id;
        refresh_front_proxy();
        if (parentWindow != nullptr) parentWindow->show();
        show();
    });
}
