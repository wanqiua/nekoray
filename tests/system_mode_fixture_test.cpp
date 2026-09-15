#include "system_mode_fixture.hpp"
#include <QCoreApplication>
#include <QNetworkProxyFactory>

class RejectProxyFactory final : public QNetworkProxyFactory {
public:
    int queries = 0;
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override {
        ++queries;
        return {QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 1)};
    }
};

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    // This factory affects this standalone process only. Explicit NoProxy on
    // every fixture socket must keep even UDP ASSOCIATE off this invalid proxy.
    auto factory = new RejectProxyFactory;
    QNetworkProxyFactory::setApplicationProxyFactory(factory);
    const auto error = SystemModeAcceptance::SocksUdpFixtureSanity();
    if (!error.isEmpty()) { qCritical().noquote() << error; return 1; }
    if (factory->queries != 0) { qCritical() << "fixture consulted the global proxy factory" << factory->queries; return 1; }
    qInfo() << "PASS: fixture made zero global proxy-factory queries";
    return 0;
}
