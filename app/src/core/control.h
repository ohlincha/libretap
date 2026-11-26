#pragma once
// The control socket: a running LibreTap (the GUI or `LibreTapCLI record`) listens on a local socket (a Unix
// socket / Windows named pipe) named libretap-<user>, or $LIBRETAP_CONTROL. One JSON object per line each way:
//   → {"cmd":"mark","note":"integration 1 s"}      ← {"ok":true,"mark":3,"note":"…","t":1796031300.1}
// Commands: status, start, mark, sync, stop (Recorder::handleCommand). `LibreTapCLI status|mark|sync|stop|start`
// are the clients; anything that can write a line to a local socket can be one.
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QLocalServer;

namespace lt {

QString controlName();

class ControlServer : public QObject {
    Q_OBJECT
public:
    explicit ControlServer(std::function<QJsonObject(const QJsonObject&)> handler, QObject* parent = nullptr);
    // False when another LibreTap already listens (error says so); a stale socket file is removed first.
    bool listen(QString* error);

private:
    std::function<QJsonObject(const QJsonObject&)> handler_;
    QLocalServer* server_;
};

// Sends one request to the running LibreTap and returns its answer; false (error set) when none runs.
bool controlRequest(const QJsonObject& request, QJsonObject* answer, QString* error, int timeoutMs = 5000);

}  // namespace lt
