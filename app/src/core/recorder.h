#pragma once
// The recorder: starts a project folder with one LibreTap session in it, runs the sources into it, and takes
// marks and syncs. Used by the GUI and by `LibreTapCLI record`; both answer the control socket (control.h) through
// handleCommand(), so an agent can drive either.
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <memory>
#include <vector>

#include "serialtap.h"
#include "session.h"

namespace lt {

class CameraFeed;
class Source;

struct RecordOptions {
    bool tap = true;               // record the serial tap
    TapConfig tapConfig;
    bool demo = false;             // the demo tap instead of real ports (demo.h)
    double stillEvery = 0.5;       // camera stills, seconds
    QString workspace;             // empty: workspaceDir()
    QString name;                  // added to the folder name
    QString note;                  // session.json "note"
};

class Recorder : public QObject {
    Q_OBJECT
public:
    explicit Recorder(QObject* parent = nullptr);
    ~Recorder() override;
    // The camera feed to take stills from while recording (not owned; may be null or closed: no camera).
    void setCamera(CameraFeed* feed) { camera_ = feed; }
    bool start(const RecordOptions& o, QString* error);
    void stop();
    bool isRecording() const { return writer_ != nullptr; }
    int mark(const QString& note);           // its number; 0 when not recording
    int sync();
    QJsonObject status() const;
    QString projectFolder() const { return project_; }
    QString sessionFolder() const { return writer_ ? writer_->folder() : lastSession_; }
    double elapsed() const;

    // A control request ({"cmd": "status" | "mark" | "sync" | "stop" | "start", …}) → its answer. "start" is
    // answered by startHandler when set (the GUI starts with its own connections), else refused.
    QJsonObject handleCommand(const QJsonObject& request);
    std::function<QJsonObject(const QJsonObject&)> startHandler;

signals:
    void started(const QString& projectFolder);
    void stopped(const QString& projectFolder);
    void recorded(const QJsonObject& event);  // every event written, for live views
    void problem(const QString& text);

private:
    void note(const QString& kind, const QString& text);
    std::unique_ptr<SessionWriter> writer_;
    std::vector<std::unique_ptr<Source>> sources_;
    CameraFeed* camera_ = nullptr;   // not owned
    QString project_, lastSession_;
    QElapsedTimer clock_;
    double t0_ = 0;
    int marks_ = 0, syncs_ = 0;
    qint64 lastElapsedMs_ = 0;
    QJsonObject lastStats_;
};

}  // namespace lt
