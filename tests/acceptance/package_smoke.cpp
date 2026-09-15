#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTimer>
#include <windows.h>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc != 3) return 2;
    qInfo() << "Platform:" << QApplication::platformName();
    qInfo() << "Library paths:" << QCoreApplication::libraryPaths();
    if (QApplication::platformName() != "windows") return 3;
    if (!QSslSocket::setActiveBackend("openssl") || !QSslSocket::supportsSsl()) return 4;
    qInfo() << "TLS:" << QSslSocket::sslLibraryVersionString();
    const auto certs = QSslCertificate::fromPath(QString::fromLocal8Bit(argv[1]));
    QFile keyFile(QString::fromLocal8Bit(argv[2]));
    if (certs.isEmpty() || !keyFile.open(QIODevice::ReadOnly)) return 5;
    QSslKey key(keyFile.readAll(), QSsl::Rsa);
    if (key.isNull()) return 6;
    QSslConfiguration serverConfig;
    serverConfig.setLocalCertificateChain(certs);
    serverConfig.setPrivateKey(key);
    serverConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    QSslServer server;
    server.setSslConfiguration(serverConfig);
    QObject::connect(&server, &QSslServer::pendingConnectionAvailable, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    if (!server.listen(QHostAddress::LocalHost, 0)) return 7;
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);
    QSslConfiguration clientConfig = QSslConfiguration::defaultConfiguration();
    clientConfig.addCaCertificates(certs);
    QNetworkRequest request(QUrl(QString("https://localhost:%1/").arg(server.serverPort())));
    request.setSslConfiguration(clientConfig);
    auto *reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, [&] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 204) {
            qCritical() << reply->errorString() << status;
            app.exit(8);
            return;
        }
        for (const auto *name : {L"Qt6Core.dll", L"Qt6Gui.dll", L"Qt6Widgets.dll", L"Qt6Network.dll",
                                L"qwindows.dll", L"qopensslbackend.dll", L"libssl-3-x64.dll",
                                L"libcrypto-3-x64.dll", L"msvcp140.dll", L"vcruntime140.dll"}) {
            wchar_t path[32768] = {};
            const auto module = GetModuleHandleW(name);
            if (!module || !GetModuleFileNameW(module, path, 32768)) { app.exit(9); return; }
            const QString full = QString::fromWCharArray(path).replace('\\', '/');
            if (!full.startsWith(QCoreApplication::applicationDirPath() + '/', Qt::CaseInsensitive)) {
                qCritical() << "Runtime outside package:" << full;
                app.exit(10);
                return;
            }
            qInfo() << "Local runtime:" << QString::fromWCharArray(name);
        }
        qInfo() << "HTTPS certificate verification and 204 response passed.";
        app.exit(0);
    });
    QTimer::singleShot(10000, &app, [&] { qCritical() << "TLS smoke timeout"; app.exit(11); });
    return app.exec();
}
