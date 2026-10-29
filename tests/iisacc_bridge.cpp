#include <iiCalendar/IisaccAccount.h>
#include <iiAccountManager/AccountManager.h>
#include <iiFileProviderNative.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <iostream>
#include <string_view>

namespace {
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
class Provider final : public iiCalendar::ExternalCalendarProvider {
public:
    std::string id() const override { return "fixture"; }
    std::vector<iiCalendar::RemoteCalendar> calendars() override { return {{"calendar", "Work"}}; }
    std::vector<iiCalendar::Event> events(const iiCalendar::RemoteCalendar &, iiCalendar::TimeRange) override { return {}; }
};
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("iiCalendar.AccountBridgeTest");
    try {
        const std::string_view fixturePath = IISACC_ACCOUNT_FIXTURE;
        const std::u8string_view utf8Path(reinterpret_cast<const char8_t *>(fixturePath.data()), fixturePath.size());
        const auto fixture = iiFileProvider::native::File::read(std::filesystem::path(utf8Path), 1024 * 1024);
        const auto profile = QJsonDocument::fromJson(QByteArray::fromStdString(fixture)).object();
        check(!profile.isEmpty(), "account fixture contains a JSON profile");
        QTcpServer server; check(server.listen(QHostAddress::LocalHost), "local fixture server");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, profile] {
                    auto data = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", data);
                    if (socket->property("handled").toBool()) return;
                    const auto headers = data.indexOf("\r\n\r\n"); if (headers < 0) return;
                    qsizetype length = 0;
                    for (const auto &line : data.left(headers).split('\n'))
                        if (line.trimmed().toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    if (data.size() < headers + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto request = QJsonDocument::fromJson(data.mid(headers + 4)).object();
                    const auto device = request.value("variables").toObject().value("input").toObject().value("device").toObject();
                    const auto now = QDateTime::currentDateTimeUtc();
                    const QJsonObject session{{"id", QString(32, 'a')}, {"client", "app"}, {"current", true}, {"device", device},
                        {"createdAt", now.addSecs(-60).toString(Qt::ISODateWithMs)}, {"lastSeenAt", now.toString(Qt::ISODateWithMs)},
                        {"expiresAt", now.addSecs(600).toString(Qt::ISODateWithMs)}};
                    const auto body = QJsonDocument(QJsonObject{{"data", QJsonObject{{"appSession", QJsonObject{{"account", profile}, {"session", session}}}}}}).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        const QUrl origin("http://127.0.0.1:" + QString::number(server.serverPort()));
        iisacc::accounts::AccountManager source(origin);
        iiCalendar::CalendarAccountOptions options; options.iisaccOrigin = origin.toString().toStdString();
        iiCalendar::CalendarAccountManager calendar(options);
        const auto synchronize = [&] { iiCalendar::synchronizeIisaccAccount(calendar, source); };
        QObject::connect(&source, &iisacc::accounts::AccountManager::authenticatedChanged, &source, synchronize);
        QObject::connect(&source, &iisacc::accounts::AccountManager::loginSessionChanged, &source, synchronize);
        QObject::connect(&source, &iisacc::accounts::AccountManager::sessionEnding, &source, [&] { calendar.setIisaccSession(std::nullopt); });
        check(source.readAccount(profile.toVariantMap()), "display account fixture accepted"); synchronize();
        check(!calendar.authenticated(), "display-only account must not authenticate Calendar");
        check(source.loginWithPassword("fixture@example.com", "isolated-test-password"), "login dispatched");
        QElapsedTimer elapsed; elapsed.start();
        while (!source.isAuthenticated() && elapsed.elapsed() < 5000) { app.processEvents(); QThread::msleep(1); }
        check(source.isAuthenticated(), "real account SDK accepted its fixture app session"); synchronize();
        check(calendar.authenticated(), "calendar follows verified account SDK state");
        check(calendar.iisaccSession()->subject == profile.value("sub").toString().toStdString(), "primary identity preserved");
        check(calendar.iisaccSession()->expiresAt.seconds() == source.authenticatedUntil().toSecsSinceEpoch(), "effective SDK expiry preserved");
        const auto connected = calendar.connect({"external", "fixture", "Work"}, std::make_shared<Provider>());
        check(connected.ownerId == calendar.iisaccSession()->subject && calendar.calendars().size() == 1, "external account bound to iisacc");
        source.clear(); check(!calendar.authenticated() && calendar.accounts().empty(), "account clear retires Calendar connections");
        std::cout << "Verified iiAccountManager login, native binding, effective expiry and logout integration passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
