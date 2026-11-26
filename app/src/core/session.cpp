#include "session.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSysInfo>

#include <algorithm>

#include "util.h"

namespace lt {

SessionWriter::~SessionWriter() { events_.close(); }

bool SessionWriter::create(const QString& folder, const QJsonObject& meta, QString* error) {
    folder_ = QDir(folder).absolutePath();
    QDir d;
    if (!d.mkpath(folder_) || !d.mkpath(folder_ + QStringLiteral("/frames")) || !d.mkpath(folder_ + QStringLiteral("/raw"))) {
        if (error) *error = QStringLiteral("cannot create the folder %1").arg(folder_);
        return false;
    }
    started_ = wallNow();
    meta_ = QJsonObject{
        {QStringLiteral("format"), QStringLiteral("libretap-session")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("recorder"), recorderName()},
        {QStringLiteral("host"), QSysInfo::machineHostName()},
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("started"), isoLocal(started_)},
        {QStringLiteral("t_started"), started_},
        {QStringLiteral("clock"), QStringLiteral("Recorder's own wall clock; event t = seconds since 1970-01-01 UTC by that clock")},
        {QStringLiteral("sources"), QJsonArray()},
        {QStringLiteral("note"), QString()},
    };
    for (auto it = meta.begin(); it != meta.end(); ++it) meta_.insert(it.key(), it.value());
    events_.setFileName(folder_ + QStringLiteral("/events.jsonl"));
    if (!events_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(events_.fileName());
        return false;
    }
    writeMeta();
    return true;
}

void SessionWriter::setSources(const QJsonArray& sources) {
    QMutexLocker lock(&mutex_);
    meta_.insert(QStringLiteral("sources"), sources);
    writeMeta();
}

void SessionWriter::writeMeta() {
    QSaveFile f(folder_ + QStringLiteral("/session.json"));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(meta_).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

QJsonObject SessionWriter::event(double t, const QString& kind, const QJsonObject& fields) {
    QMutexLocker lock(&mutex_);
    events_.write(eventLine(t, kind, fields));
    events_.flush();   // a crash or a pulled stick loses at most the line being written
    QJsonObject o = fields;
    o.insert(QStringLiteral("t"), t);
    o.insert(QStringLiteral("kind"), kind);
    return o;
}

void SessionWriter::finish(const QJsonObject& extra) {
    QMutexLocker lock(&mutex_);
    events_.close();
    const double t = wallNow();
    meta_.insert(QStringLiteral("ended"), isoLocal(t));
    meta_.insert(QStringLiteral("t_ended"), t);
    for (auto it = extra.begin(); it != extra.end(); ++it) meta_.insert(it.key(), it.value());
    writeMeta();
}

QString SessionWriter::path(const QString& relative) const { return folder_ + QLatin1Char('/') + relative; }

QByteArray Event::bytes() const { return QByteArray::fromHex(o.value(QStringLiteral("hex")).toString().toLatin1()); }

bool Session::isSession(const QString& folder) {
    QFile f(QDir(folder).filePath(QStringLiteral("session.json")));
    if (!f.open(QIODevice::ReadOnly)) return false;
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("format")).toString() == QLatin1String("libretap-session");
}

bool Session::load(const QString& folder, Session* out, QString* error) {
    Session s;
    s.folder = QDir(folder).absolutePath();
    QFile mf(QDir(folder).filePath(QStringLiteral("session.json")));
    if (!mf.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("%1 is not a LibreTap/Tick session (no session.json)").arg(folder);
        return false;
    }
    QJsonParseError pe;
    s.meta = QJsonDocument::fromJson(mf.readAll(), &pe).object();
    if (s.meta.value(QStringLiteral("format")).toString() != QLatin1String("libretap-session")) {
        if (error) *error = QStringLiteral("%1: session.json is not a libretap-session").arg(folder);
        return false;
    }
    QFile ef(QDir(folder).filePath(QStringLiteral("events.jsonl")));
    if (ef.open(QIODevice::ReadOnly)) {
        const QByteArray all = ef.readAll();
        qsizetype pos = 0;
        while (pos < all.size()) {
            qsizetype nl = all.indexOf('\n', pos);
            const bool last = nl < 0;
            const QByteArray line = all.mid(pos, last ? -1 : nl - pos).trimmed();
            pos = last ? all.size() : nl + 1;
            if (line.isEmpty()) continue;
            const QJsonObject o = QJsonDocument::fromJson(line, &pe).object();
            if (pe.error != QJsonParseError::NoError || o.isEmpty()) {
                if (last) s.cutOff = true;   // the recorder stopped mid-line: ignored, as the format says
                else ++s.badLines;
                continue;
            }
            Event e;
            e.t = o.value(QStringLiteral("t")).toDouble();
            e.kind = o.value(QStringLiteral("kind")).toString();
            e.src = o.value(QStringLiteral("src")).toString();
            e.o = o;
            s.events.push_back(std::move(e));
        }
    }
    std::stable_sort(s.events.begin(), s.events.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
    *out = std::move(s);
    return true;
}

QString Session::name() const { return QFileInfo(folder).fileName(); }

QJsonObject Session::source(const QString& id) const {
    for (const QJsonValue& v : sources())
        if (v.toObject().value(QStringLiteral("id")).toString() == id) return v.toObject();
    return {};
}

bool Session::hasSourceKind(const QString& kind) const {
    for (const QJsonValue& v : sources())
        if (v.toObject().value(QStringLiteral("kind")).toString() == kind) return true;
    return false;
}

double Session::start() const {
    if (!events.empty()) return events.front().t;
    return meta.value(QStringLiteral("t_started")).toDouble();
}

double Session::end() const { return events.empty() ? start() : events.back().t; }

int Session::count(const QString& kind) const {
    return int(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.kind == kind; }));
}

}  // namespace lt
