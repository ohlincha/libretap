#include "recorder.h"

#include <QDir>
#include <QJsonArray>

#include "camerasource.h"
#include "demo.h"
#include "project.h"
#include "util.h"

namespace lt {

Recorder::Recorder(QObject* parent) : QObject(parent) {}

Recorder::~Recorder() { stop(); }

bool Recorder::start(const RecordOptions& o, QString* error) {
    if (writer_) {
        if (error) *error = QStringLiteral("already recording");
        return false;
    }
    const double now = wallNow();
    QString base = stampName(now);
    if (!o.name.trimmed().isEmpty()) base += QLatin1Char('-') + o.name.trimmed().replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    const QString ws = o.workspace.isEmpty() ? workspaceDir() : o.workspace;
    QString folder = QDir(ws).filePath(base);
    for (int k = 2; QFileInfo::exists(folder); ++k) folder = QDir(ws).filePath(base + QStringLiteral("-%1").arg(k));

    std::vector<std::unique_ptr<Source>> sources;
    if (o.demo) sources.push_back(std::make_unique<DemoTapSource>());
    else if (o.tap) sources.push_back(std::make_unique<SerialTapSource>(o.tapConfig));
    if (o.usb) sources.push_back(std::make_unique<PicoSource>(o.pico));
    if (o.ethernet) sources.push_back(std::make_unique<EthernetSource>(o.eth));
    if (camera_ && camera_->isOpen()) sources.push_back(std::make_unique<CameraSource>(camera_, o.stillEvery));
    if (sources.empty()) {
        if (error) *error = QStringLiteral("nothing to record: no serial tap, USB sniffer, network or camera");
        return false;
    }

    Project project;
    if (!Project::create(folder, &project, error)) return false;
    auto w = std::make_unique<SessionWriter>();
    QJsonObject meta{{QStringLiteral("note"), o.note}};
    if (o.demo) meta.insert(QStringLiteral("demo"), true);
    const QString sessionFolder = QDir(folder).filePath(stampName(now) + QStringLiteral("-libretap"));
    if (!w->create(sessionFolder, meta, error)) return false;
    QJsonArray desc;
    for (auto& s : sources) {
        QString err;
        if (!s->start(w.get(), &err)) {
            for (auto& started : sources) started->stop();
            w->event(wallNow(), QStringLiteral("error"), {{QStringLiteral("text"), err}});
            w->finish({{QStringLiteral("failed"), err}});
            if (error) *error = err;
            QDir(folder).removeRecursively();
            return false;
        }
        desc.append(s->describe());
        connect(s.get(), &Source::recorded, this, &Recorder::recorded);
        connect(s.get(), &Source::problem, this, [this](const QString& text) { note(QStringLiteral("error"), text); });
    }
    w->setSources(desc);
    writer_ = std::move(w);
    sources_ = std::move(sources);
    project_ = folder;
    lastSession_ = writer_->folder();
    project.addSession(lastSession_, nullptr);
    project.save(nullptr);
    marks_ = syncs_ = 0;
    t0_ = now;
    clock_.start();
    emit recorded(writer_->event(wallNow(), QStringLiteral("status"), {{QStringLiteral("text"), QStringLiteral("recording started")}}));
    emit started(project_);
    return true;
}

void Recorder::stop() {
    if (!writer_) return;
    for (auto& s : sources_) s->stop();
    QJsonObject stats;
    for (auto& s : sources_) {
        const QJsonObject st = s->stats();
        for (auto it = st.begin(); it != st.end(); ++it) stats.insert(it.key(), it.value());
    }
    stats.insert(QStringLiteral("marks"), marks_);
    stats.insert(QStringLiteral("syncs"), syncs_);
    lastStats_ = stats;
    lastElapsedMs_ = clock_.elapsed();
    emit recorded(writer_->event(wallNow(), QStringLiteral("status"), {{QStringLiteral("text"), QStringLiteral("recording stopped")}}));
    writer_->finish({{QStringLiteral("stats"), stats}});
    // the project file again: the session's first event time is now known
    Project p;
    if (Project::load(project_, &p, nullptr) && !p.sessions.empty()) {
        p.sessions[0].t0 = p.sessions[0].session.start();
        p.save(nullptr);
    }
    sources_.clear();
    writer_.reset();
    emit stopped(project_);
}

void Recorder::note(const QString& kind, const QString& text) {
    if (writer_) emit recorded(writer_->event(wallNow(), kind, {{QStringLiteral("text"), text}}));
    emit problem(text);
}

int Recorder::mark(const QString& text) {
    if (!writer_) return 0;
    const double t = wallNow();
    for (auto& s : sources_) s->flush();   // the bytes before the mark come before it in the file
    emit recorded(writer_->event(t, QStringLiteral("mark"), {{QStringLiteral("n"), ++marks_}, {QStringLiteral("note"), text}}));
    return marks_;
}

int Recorder::sync() {
    if (!writer_) return 0;
    const double t = wallNow();
    for (auto& s : sources_) s->flush();
    emit recorded(writer_->event(t, QStringLiteral("sync"), {{QStringLiteral("n"), ++syncs_}}));
    return syncs_;
}

double Recorder::elapsed() const { return writer_ ? clock_.elapsed() / 1000.0 : lastElapsedMs_ / 1000.0; }

QJsonObject Recorder::status() const {
    QJsonObject o{{QStringLiteral("running"), true},
                  {QStringLiteral("recording"), isRecording()},
                  {QStringLiteral("project"), project_},
                  {QStringLiteral("session"), sessionFolder()},
                  {QStringLiteral("elapsed_s"), elapsed()},
                  {QStringLiteral("marks"), marks_},
                  {QStringLiteral("syncs"), syncs_}};
    QJsonObject stats = lastStats_;
    QJsonArray src;
    for (const auto& s : sources_) {
        const QJsonObject st = s->stats();
        for (auto it = st.begin(); it != st.end(); ++it) stats.insert(it.key(), it.value());
        src.append(s->describe());
    }
    o.insert(QStringLiteral("stats"), stats);
    if (!src.isEmpty()) o.insert(QStringLiteral("sources"), src);
    return o;
}

QJsonObject Recorder::handleCommand(const QJsonObject& req) {
    const QString cmd = req.value(QStringLiteral("cmd")).toString();
    auto fail = [](const QString& text) { return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), text}}; };
    auto ok = [](QJsonObject o) {
        o.insert(QStringLiteral("ok"), true);
        return o;
    };
    if (cmd == QLatin1String("status")) return ok(status());
    if (cmd == QLatin1String("mark")) {
        if (!isRecording()) return fail(QStringLiteral("not recording"));
        const QString text = req.value(QStringLiteral("note")).toString();
        return ok({{QStringLiteral("mark"), mark(text)}, {QStringLiteral("note"), text}, {QStringLiteral("t"), wallNow()}});
    }
    if (cmd == QLatin1String("sync")) {
        if (!isRecording()) return fail(QStringLiteral("not recording"));
        return ok({{QStringLiteral("sync"), sync()}, {QStringLiteral("t"), wallNow()}});
    }
    if (cmd == QLatin1String("stop")) {
        if (!isRecording()) return fail(QStringLiteral("not recording"));
        stop();
        return ok(status());
    }
    if (cmd == QLatin1String("start")) {
        if (isRecording()) return fail(QStringLiteral("already recording"));
        if (!startHandler) return fail(QStringLiteral("this recorder cannot be started remotely"));
        return startHandler(req);
    }
    return fail(QStringLiteral("unknown command '%1' (status, start, mark, sync, stop)").arg(cmd));
}

}  // namespace lt
