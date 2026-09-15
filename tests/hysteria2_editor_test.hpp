#pragma once

#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <functional>
#include <limits>

#include "tests/profile_editor_core_test.hpp"
#include "fmt/QUICBean.hpp"

// GUI-only regression cases. The suite supplies an isolated profile database;
// these synthetic profiles never start a core or change host network settings.
namespace Hysteria2EditorRegression {
    using ProfileEditorCoreRegression::Control;
    using SubscriptionMetadataRegression::GroupFixture;

    inline std::shared_ptr<NekoGui::ProxyEntity> AddNode(const GroupFixture &fixture, const QString &type = "hysteria2") {
        auto node = NekoGui::ProfileManager::NewProxyEntity(type);
        node->bean->name = "Synthetic QUIC editor fixture";
        node->bean->serverAddress = "127.0.0.1";
        node->bean->serverPort = 443;
        node->QUICBean()->password = "synthetic-password";
        if (!NekoGui::profileManager->AddProfile(node, fixture.group->id)) qFatal("cannot create QUIC editor fixture");
        return node;
    }

    // Dismiss only this editor's expected validation warning, with the same
    // parent that MessageBoxWarning will choose immediately before the action.
    // A scoped deadline fails an unexpected modal instead of closing other UI.
    class WarningMonitor {
        QPointer<QWidget> expectedParent;
        QTimer poll, deadline;
        bool saving = false;

    public:
        int previewWarnings = 0;
        int saveWarnings = 0;
        QString lastText;

        WarningMonitor() : expectedParent(GetMessageBoxParent()) {
            QObject::connect(&poll, &QTimer::timeout, &poll, [this] {
                auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!box || box->parentWidget() != expectedParent.data() || box->icon() != QMessageBox::Warning ||
                    box->windowTitle() != DialogEditProfile::tr("无法保存") || !box->text().contains("Hysteria2")) return;
                if (saving) ++saveWarnings;
                else ++previewWarnings;
                lastText = box->text();
                box->done(QMessageBox::Ok);
            });
            QObject::connect(&deadline, &QTimer::timeout, &deadline, [] {
                qFatal("Hysteria2 editor modal did not complete");
            });
            poll.start(5);
            deadline.setSingleShot(true);
            deadline.start(5000);
        }

        void accept(DialogEditProfile &editor) {
            expectedParent = GetMessageBoxParent();
            saving = true;
            editor.accept();
            saving = false;
        }
    };

    inline void AdvancedRoundTrip(MainWindow *window) {
        GroupFixture fixture;
        const auto originalRunning = NekoGui::dataStore->started_id;
        {
            DialogEditProfile editor("hysteria2", fixture.group->id, window);
            editor.show();
            auto toggle = Control<QToolButton>(&editor, "hy2AdvancedToggle");
            QVERIFY(!toggle->isHidden());
            QVERIFY(!toggle->isChecked());
            QVERIFY(Control<QScrollArea>(&editor, "hy2Advanced")->isHidden());
            QCOMPARE(Control<QLineEdit>(&editor, "uploadMbps")->text(), QString("0"));
            QCOMPARE(Control<QLineEdit>(&editor, "downloadMbps")->text(), QString("0"));
            editor.reject();
            QVERIFY(fixture.group->Profiles().isEmpty());
        }

        // Legacy inference, explicit default values, and configured new fields
        // must all survive a no-op save without collapsing their distinctions.
        for (int variant = 0; variant < 3; ++variant) {
            auto node = AddNode(fixture);
            auto bean = node->QUICBean();
            bean->uploadMbps = 37;
            bean->downloadMbps = 83;
            bean->hopInterval = 17;
            bean->hopPort = "443,2000-3000";
            bean->sni = "localhost";
            bean->alpn = "h3";
            bean->authPayload = "retained legacy authentication";
            bean->disableMtuDiscovery = true;
            bean->streamReceiveWindow = (qint64(1) << 40) + 17;
            bean->connectionReceiveWindow = (qint64(1) << 41) + 31;
            bean->obfsPassword = "synthetic-obfuscation";
            if (variant == 0) {
                bean->bbrProfile = "";
                bean->obfsType = "";
                bean->geckoMinPacketSize = 0;
                bean->geckoMaxPacketSize = 0;
            } else if (variant == 1) {
                bean->bbrProfile = "standard";
                bean->obfsType = "salamander";
                bean->password = QString(40000, QChar('p'));
            } else {
                bean->bbrProfile = "aggressive";
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                bean->streamReceiveWindow = std::numeric_limits<qint64>::max();
                bean->connectionReceiveWindow = (qint64(1) << 53) + 1;
#endif
                bean->obfsType = "gecko";
                bean->geckoMinPacketSize = 640;
                bean->geckoMaxPacketSize = 1280;
                bean->disableChromeParrot = true;
                bean->hopIntervalMin = "5.000000001s";
                bean->hopIntervalMax = "1m30s";
            }
            QVERIFY2(bean->Validate().isEmpty(), qPrintable(bean->Validate()));
            const auto original = node->bean->ToJson();
            {
                DialogEditProfile editor("", node->id, window);
                editor.show();
                QCoreApplication::processEvents();
                WarningMonitor warnings;
                auto toggle = Control<QToolButton>(&editor, "hy2AdvancedToggle");
                auto panel = Control<QScrollArea>(&editor, "hy2Advanced");
                auto bbr = Control<QComboBox>(&editor, "hy2BbrProfile");
                QVERIFY(!toggle->isChecked());
                QVERIFY(panel->isHidden());
                QCOMPARE(toggle->arrowType(), Qt::RightArrow);
                QCOMPARE(bbr->currentData().toString(), bean->bbrProfile);
                QCOMPARE(Control<QComboBox>(&editor, "hy2ObfsType")->currentData().toString(), bean->obfsType);
                QCOMPARE(Control<QLineEdit>(&editor, "hy2GeckoMinPacketSize")->text(), QString::number(bean->geckoMinPacketSize));
                QCOMPARE(Control<QLineEdit>(&editor, "hy2GeckoMaxPacketSize")->text(), QString::number(bean->geckoMaxPacketSize));
                QCOMPARE(Control<QCheckBox>(&editor, "hy2DisableChromeParrot")->isChecked(), bean->disableChromeParrot);
                QCOMPARE(Control<QLineEdit>(&editor, "hy2HopIntervalMin")->text(), bean->hopIntervalMin);
                QCOMPARE(Control<QLineEdit>(&editor, "hy2HopIntervalMax")->text(), bean->hopIntervalMax);
                QCOMPARE(Control<QLineEdit>(&editor, "password")->text(), bean->password);
                QCOMPARE(Control<QLineEdit>(&editor, "streamReceiveWindow")->text(), QString::number(bean->streamReceiveWindow));
                QCOMPARE(Control<QLineEdit>(&editor, "connectionReceiveWindow")->text(), QString::number(bean->connectionReceiveWindow));

                toggle->click();
                QVERIFY(!panel->isHidden());
                QCOMPARE(toggle->arrowType(), Qt::DownArrow);
                bbr->setCurrentIndex(bbr->findData("conservative"));
                QCOMPARE(Control<QLineEdit>(&editor, "uploadMbps")->text(), QString("37"));
                QCOMPARE(Control<QLineEdit>(&editor, "downloadMbps")->text(), QString("83"));
                bbr->setCurrentIndex(bbr->findData(bean->bbrProfile));
                toggle->click();
                QVERIFY(panel->isHidden());
                QCOMPARE(node->bean->ToJson(), original);
                QCOMPARE(warnings.previewWarnings, 0);
                warnings.accept(editor);
                QCOMPARE(warnings.saveWarnings, 0);
                QCOMPARE(editor.result(), int(QDialog::Accepted));
                QCOMPARE(node->bean->ToJson(), original);
            }
            auto loaded = NekoGui::ProfileManager::NewProxyEntity("hysteria2");
            loaded->fn = node->fn;
            QVERIFY(loaded->Load());
            QCOMPARE(loaded->bean->ToJson(), original);
            QCOMPARE(loaded->QUICBean()->bbrProfile, original.value("bbrProfile").toString());
            NekoGui_fmt::QUICBean shared(NekoGui_fmt::QUICBean::proxy_Hysteria2);
            QString shareError;
            QVERIFY2(shared.TryParseLink(node->bean->ToShareLink(), &shareError), qPrintable(shareError));
            QCOMPARE(shared.ToJson(), original);
            // Expanding the previous editor must not become a saved preference.
            DialogEditProfile reopened("", node->id, window);
            QVERIFY(!Control<QToolButton>(&reopened, "hy2AdvancedToggle")->isChecked());
            QVERIFY(Control<QScrollArea>(&reopened, "hy2Advanced")->isHidden());
            reopened.reject();
        }
        QCOMPARE(NekoGui::dataStore->started_id, originalRunning);
    }

    inline void CancelAndLegacyEditors(MainWindow *window) {
        GroupFixture fixture;
        auto node = AddNode(fixture);
        // Keep future/invalid loaded values visible and repairable, never
        // silently select a different combo option or clamp a packet size.
        node->QUICBean()->bbrProfile = "future-profile";
        node->QUICBean()->obfsType = "future-obfuscation";
        node->QUICBean()->geckoMinPacketSize = -7;
        node->QUICBean()->geckoMaxPacketSize = 4096;
        const auto original = node->bean->ToJson();
        const auto originalPreference = node->core_preference;
        {
            DialogEditProfile editor("", node->id, window);
            editor.show();
            QCoreApplication::processEvents();
            WarningMonitor warnings;
            QCOMPARE(Control<QComboBox>(&editor, "hy2BbrProfile")->currentData().toString(), QString("future-profile"));
            QCOMPARE(Control<QComboBox>(&editor, "hy2ObfsType")->currentData().toString(), QString("future-obfuscation"));
            QCOMPARE(Control<QLineEdit>(&editor, "hy2GeckoMinPacketSize")->text(), QString("-7"));
            QCOMPARE(Control<QLineEdit>(&editor, "hy2GeckoMaxPacketSize")->text(), QString("4096"));
            Control<QToolButton>(&editor, "hy2AdvancedToggle")->click();
            Control<QLineEdit>(&editor, "name")->setText("discarded HY2 draft");
            Control<QLineEdit>(&editor, "password")->setText("discarded password");
            Control<QLineEdit>(&editor, "hy2HopIntervalMin")->setText("4s");
            Control<QLineEdit>(&editor, "hy2GeckoMaxPacketSize")->setText("2147483648");
            Control<QCheckBox>(&editor, "hy2DisableChromeParrot")->click();
            QCOMPARE(warnings.previewWarnings, 0);
            QCOMPARE(node->bean->ToJson(), original);
            editor.reject();
        }
        QCOMPARE(node->bean->ToJson(), original);
        QCOMPARE(node->core_preference, originalPreference);

        // HY1 and TUIC retain their original controls and storage behavior.
        for (const auto &type : {QString("hysteria"), QString("tuic")}) {
            auto legacy = AddNode(fixture, type);
            auto bean = legacy->QUICBean();
            bean->sni = "localhost";
            bean->alpn = "h3";
            bean->allowInsecure = true;
            if (type == "hysteria") {
                bean->hyProtocol = NekoGui_fmt::QUICBean::hysteria_protocol_facktcp;
                bean->authPayloadType = NekoGui_fmt::QUICBean::hysteria_auth_string;
                bean->authPayload = "synthetic-authentication";
                bean->obfsPassword = "synthetic-obfuscation";
                bean->hopPort = "443,8443";
                bean->hopInterval = 19;
                bean->uploadMbps = 67;
                bean->downloadMbps = 89;
                bean->streamReceiveWindow = 4194304;
                bean->connectionReceiveWindow = 8388608;
            } else {
                bean->uuid = "11771398-235c-4ea3-98fe-7a941ea5ce0b";
                bean->congestionControl = "cubic";
                bean->udpRelayMode = "quic";
                bean->heartbeat = "17s";
                bean->zeroRttHandshake = true;
                bean->uos = true;
            }
            const auto before = legacy->bean->ToJson();
            DialogEditProfile editor("", legacy->id, window);
            editor.show();
            QVERIFY(Control<QToolButton>(&editor, "hy2AdvancedToggle")->isHidden());
            QVERIFY(Control<QScrollArea>(&editor, "hy2Advanced")->isHidden());
            editor.accept();
            QCOMPARE(editor.result(), int(QDialog::Accepted));
            QCOMPARE(legacy->bean->ToJson(), before);
        }
    }

    inline void InvalidAdvancedOptionsCannotSave(MainWindow *window) {
        GroupFixture fixture;
        struct InvalidCase {
            const char *description;
            std::function<void(DialogEditProfile &)> edit;
        };
        const QList<InvalidCase> cases{
            {"Gecko upper bound", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "hy2GeckoMaxPacketSize")->setText("2049");
             }},
            {"Gecko reversed range", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "hy2GeckoMinPacketSize")->setText("1281");
             }},
            {"integer overflow must not become default", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "hy2GeckoMinPacketSize")->setText("2147483648");
             }},
            {"bandwidth overflow must not select BBR", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "uploadMbps")->setText("2147483648");
             }},
            {"receive window overflow", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "streamReceiveWindow")->setText("9223372036854775808");
             }},
            {"minimum hopping duration", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "hy2HopIntervalMin")->setText("4.999999999s");
             }},
            {"reversed hopping duration", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "hy2HopIntervalMax")->setText("9s");
             }},
            {"hopping duration requires port range", [](DialogEditProfile &editor) {
                 Control<QLineEdit>(&editor, "hopPort")->clear();
             }},
            {"disabled obfuscation retains password", [](DialogEditProfile &editor) {
                 auto obfs = Control<QComboBox>(&editor, "hy2ObfsType");
                 obfs->setCurrentIndex(obfs->findData("none"));
             }},
            {"switching obfuscation retains packet sizes", [](DialogEditProfile &editor) {
                 auto obfs = Control<QComboBox>(&editor, "hy2ObfsType");
                 obfs->setCurrentIndex(obfs->findData("salamander"));
             }},
            {"native options cannot use an external core", [](DialogEditProfile &editor) {
                 Control<QCheckBox>(&editor, "forceExternal")->setChecked(true);
             }},
        };
        for (const auto &test : cases) {
            auto node = AddNode(fixture);
            auto bean = node->QUICBean();
            bean->obfsType = "gecko";
            bean->obfsPassword = "synthetic-obfuscation";
            bean->geckoMinPacketSize = 640;
            bean->geckoMaxPacketSize = 1280;
            bean->hopPort = "443,8443";
            bean->hopIntervalMin = "10s";
            bean->hopIntervalMax = "20s";
            QVERIFY2(bean->Validate().isEmpty(), qPrintable(bean->Validate()));
            const auto original = node->bean->ToJson();
            DialogEditProfile editor("", node->id, window);
            editor.show();
            QCoreApplication::processEvents();
            WarningMonitor warnings;
            Control<QToolButton>(&editor, "hy2AdvancedToggle")->click();
            test.edit(editor);
            QVERIFY2(warnings.previewWarnings == 0, test.description);
            QCOMPARE(node->bean->ToJson(), original);
            warnings.accept(editor);
            QVERIFY2(warnings.saveWarnings == 1, test.description);
            QVERIFY2(editor.result() != QDialog::Accepted, test.description);
            QVERIFY(editor.isVisible());
            QCOMPARE(node->bean->ToJson(), original);
            editor.reject();
        }

        // A user can repair a range in place and save its exact inclusive
        // boundaries; enabling BBR still never rewrites existing bandwidth.
        auto repaired = AddNode(fixture);
        repaired->QUICBean()->obfsType = "gecko";
        repaired->QUICBean()->obfsPassword = "synthetic-obfuscation";
        repaired->QUICBean()->geckoMaxPacketSize = 4096;
        repaired->QUICBean()->uploadMbps = 37;
        repaired->QUICBean()->downloadMbps = 83;
        DialogEditProfile editor("", repaired->id, window);
        editor.show();
        QCoreApplication::processEvents();
        WarningMonitor warnings;
        Control<QToolButton>(&editor, "hy2AdvancedToggle")->click();
        QCOMPARE(Control<QLineEdit>(&editor, "hy2GeckoMaxPacketSize")->text(), QString("4096"));
        Control<QLineEdit>(&editor, "hy2GeckoMinPacketSize")->setText("1");
        Control<QLineEdit>(&editor, "hy2GeckoMaxPacketSize")->setText("2048");
        Control<QLineEdit>(&editor, "hopPort")->setText("443,8443");
        Control<QLineEdit>(&editor, "hy2HopIntervalMin")->setText("5s");
        Control<QLineEdit>(&editor, "hy2HopIntervalMax")->setText("5s");
        auto bbr = Control<QComboBox>(&editor, "hy2BbrProfile");
        bbr->setCurrentIndex(bbr->findData("conservative"));
        Control<QCheckBox>(&editor, "hy2DisableChromeParrot")->setChecked(true);
        warnings.accept(editor);
        QCOMPARE(warnings.previewWarnings, 0);
        QCOMPARE(warnings.saveWarnings, 0);
        QCOMPARE(editor.result(), int(QDialog::Accepted));
        QCOMPARE(repaired->QUICBean()->geckoMinPacketSize, 1);
        QCOMPARE(repaired->QUICBean()->geckoMaxPacketSize, 2048);
        QCOMPARE(repaired->QUICBean()->hopIntervalMin, QString("5s"));
        QCOMPARE(repaired->QUICBean()->hopIntervalMax, QString("5s"));
        QCOMPARE(repaired->QUICBean()->bbrProfile, QString("conservative"));
        QVERIFY(repaired->QUICBean()->disableChromeParrot);
        QCOMPARE(repaired->QUICBean()->uploadMbps, 37);
        QCOMPARE(repaired->QUICBean()->downloadMbps, 83);
    }
} // namespace Hysteria2EditorRegression
