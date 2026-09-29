#pragma once

#include <QtTest>
#include <QComboBox>
#include <QLineEdit>
#include <QRadioButton>

#include "tests/subscription_metadata_test.hpp"
#include "ui/edit/dialog_edit_profile.h"
#include "ui/mainwindow.h"
#include "fmt/includes.h"

namespace ProfileEditorCoreRegression {
    template<typename T>
    T *Control(QObject *parent, const char *name) {
        auto control = parent->findChild<T *>(name);
        if (!control) qFatal("missing profile editor control: %s", name);
        return control;
    }

    inline void EditorPreferenceIsDraftOnly(MainWindow *window) {
        using namespace SubscriptionMetadataRegression;
        GroupFixture fixture;
        auto node = fixture.add(1080, "original editor name");
        const auto originalBean = node->bean->ToJson();
        const auto originalRunning = NekoGui::dataStore->started_id;
        const int choice = NekoGui::coreType == NekoGui::CoreType::SING_BOX ? 2 : 1;
        const char *buttonName = choice == 1 ? "edit_core_xray" : "edit_core_singbox";
        {
            DialogEditProfile editor("", node->id, window);
            editor.show();
            auto button = Control<QRadioButton>(&editor, buttonName);
            QVERIFY(button->isEnabled());
            button->click();
            Control<QLineEdit>(&editor, "name")->setText("discarded draft");
            QCOMPARE(node->core_preference, 0);
            QCOMPARE(node->bean->ToJson(), originalBean);
            editor.reject();
        }
        QCOMPARE(node->core_preference, 0);
        QCOMPARE(node->bean->ToJson(), originalBean);
        {
            DialogEditProfile editor("", node->id, window);
            editor.show();
            Control<QRadioButton>(&editor, buttonName)->click();
            Control<QLineEdit>(&editor, "name")->setText("saved editor name");
            // A concurrent completed test must not be overwritten by the
            // metadata snapshot that the editor originally loaded.
            node->RecordTestResult(Result(NekoGui::TestResultStatus::Success));
            const auto history = node->test_results;
            editor.accept();
            QCOMPARE(editor.result(), int(QDialog::Accepted));
            QCOMPARE(node->core_preference, choice);
            QCOMPARE(node->bean->name, QString("saved editor name"));
            QCOMPARE(node->test_results, history);
            QCOMPARE(NekoGui::dataStore->started_id, originalRunning);
            QVERIFY(NekoGui::profileManager->GetProfile(node->id) == node);
        }
    }

    inline void NewNodeKeepsPreferenceAcrossTypeChanges(MainWindow *window) {
        SubscriptionMetadataRegression::GroupFixture fixture;
        const int choice = NekoGui::coreType == NekoGui::CoreType::SING_BOX ? 2 : 1;
        const char *buttonName = choice == 1 ? "edit_core_xray" : "edit_core_singbox";
        DialogEditProfile editor("socks", fixture.group->id, window);
        editor.show();
        auto button = Control<QRadioButton>(&editor, buttonName);
        QVERIFY(button->isEnabled());
        button->click();
        auto type = Control<QComboBox>(&editor, "type");
        type->setCurrentIndex(type->findData("vless"));
        QVERIFY(button->isChecked());
        type->setCurrentIndex(type->findData("socks"));
        QVERIFY(button->isChecked());
        QVERIFY(fixture.group->Profiles().isEmpty());
        editor.accept();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
        QCOMPARE(fixture.group->Profiles().size(), 1);
        QCOMPARE(fixture.group->Profiles().first()->core_preference, choice);
        QCOMPARE(fixture.group->Profiles().first()->gid, fixture.group->id);
    }

    inline void EditorExposesBothCoreOptions(MainWindow *window) {
        SubscriptionMetadataRegression::GroupFixture fixture;
        {
            DialogEditProfile editor("vless", fixture.group->id, window);
            editor.show();
            auto network = Control<QComboBox>(&editor, "network");
            QVERIFY(network->findText("httpupgrade") >= 0);
            QVERIFY(network->findText("kcp") >= 0);
            network->setCurrentText("ws");
            QVERIFY(!Control<QLineEdit>(&editor, "ws_early_data_length")->isHidden());
            QVERIFY(!Control<QComboBox>(&editor, "packet_encoding")->isHidden());
            Control<QComboBox>(&editor, "security")->setCurrentText("tls");
            QVERIFY(!Control<QLineEdit>(&editor, "reality_spx")->isHidden());
            network->setCurrentText("httpupgrade");
            QVERIFY(Control<QRadioButton>(&editor, "edit_core_xray")->isEnabled());
            editor.reject();
        }
        auto node = NekoGui::ProfileManager::NewProxyEntity("shadowsocks");
        node->ShadowSocksBean()->method = "bf-cfb";
        node->ShadowSocksBean()->password = "synthetic-password";
        QVERIFY(NekoGui::profileManager->AddProfile(node, fixture.group->id));
        {
            DialogEditProfile editor("", node->id, window);
            editor.show();
            QCOMPARE(Control<QComboBox>(&editor, "method")->currentText(), QString("bf-cfb"));
            QVERIFY(!Control<QRadioButton>(&editor, "edit_core_singbox")->isEnabled());
            editor.reject();
        }
        QCOMPARE(node->ShadowSocksBean()->method, QString("bf-cfb"));
    }
} // namespace ProfileEditorCoreRegression
