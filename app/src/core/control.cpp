#include "control.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>

namespace lt {

QString controlName() {
    const QString env = qEnvironmentVariable("LIBRETAP_CONTROL");
    if (!env.isEmpty()) return env;
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) user = qEnvironmentVariable("USERNAME");
    user.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    return QStringLiteral("libretap-") + (user.isEmpty() ? QStringLiteral("user") : user);
}

ControlServer::ControlServer(std::function<QJsonObject(const QJsonObject&)> handler, QObject* parent)
    : QObject(parent), handler_(std::move(handler)), server_(new QLocalServer(this)) {
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* s = server_->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, s, [this, s] {
                while (s->canReadLine()) {
                    const QByteArray line = s->readLine().trimmed();
                    if (line.isEmpty()) continue;
                    QJsonParseError pe;
                    const QJsonObject req = QJsonDocument::fromJson(line, &pe).object();
                    const QJsonObject ans = pe.error == QJsonParseError::NoError
                                                ? handler_(req)
                                                : QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("not JSON: ") + pe.errorString()}};
                    s->write(QJsonDocument(ans).toJson(QJsonDocument::Compact) + '\n');
                    s->flush();
                }
            });
        }
    });
}

bool ControlServer::listen(QString* error) {
    const QString name = controlName();
    if (server_->listen(name)) return true;
    // someone listening, or a socket file left by a crash?
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(500)) {
        if (error) *error = QStringLiteral("another LibreTap is already running (control socket %1); agents will talk to that one").arg(name);
        return false;
    }
    QLocalServer::removeServer(name);
    if (server_->listen(name)) return true;
    if (error) *error = QStringLiteral("cannot listen on %1: %2").arg(name, server_->errorString());
    return false;
}

bool controlRequest(const QJsonObject& request, QJsonObject* answer, QString* error, int timeoutMs) {
    QLocalSocket s;
    s.connectToServer(controlName());
    if (!s.waitForConnected(1000)) {
        if (error) *error = QStringLiteral("no LibreTap is running (start the program, or LibreTapCLI record)");
        return false;
    }
    s.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    s.flush();
    QElapsedTimer clock;
    clock.start();
    while (!s.canReadLine()) {
        if (clock.elapsed() > timeoutMs || !s.waitForReadyRead(int(std::max<qint64>(1, timeoutMs - clock.elapsed())))) {
            if (s.canReadLine()) break;
            if (error) *error = QStringLiteral("LibreTap did not answer");
            return false;
        }
    }
    QJsonParseError pe;
    *answer = QJsonDocument::fromJson(s.readLine(), &pe).object();
    if (pe.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("garbled answer from LibreTap");
        return false;
    }
    return true;
}

}  // namespace lt
