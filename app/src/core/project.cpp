#include "project.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <limits>

namespace lt {

static const char* const kProjectFile = "project.json";

bool copyFolder(const QString& from, const QString& to, QString* error) {
    QDir src(from);
    if (!QDir().mkpath(to)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(to);
        return false;
    }
    QDirIterator it(from, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QString target = QDir(to).filePath(src.relativeFilePath(p));
        if (it.fileInfo().isDir()) {
            QDir().mkpath(target);
        } else if (!QFile::copy(p, target)) {
            if (error) *error = QStringLiteral("cannot copy %1 to %2").arg(p, target);
            return false;
        }
    }
    return true;
}

bool Project::create(const QString& folder, Project* out, QString* error) {
    Project p;
    p.folder = QDir(folder).absolutePath();
    p.name = QFileInfo(p.folder).fileName();
    if (!QDir().mkpath(p.folder)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(p.folder);
        return false;
    }
    if (!p.save(error)) return false;
    *out = std::move(p);
    return true;
}

bool Project::load(const QString& path, Project* out, QString* error) {
    const QString abs = QDir(path).absolutePath();
    QFile f(QDir(abs).filePath(QLatin1String(kProjectFile)));
    if (!f.exists()) {
        if (!Session::isSession(abs)) {
            if (error) *error = QStringLiteral("%1 is neither a LibreTap recording nor a session").arg(abs);
            return false;
        }
        const QString parent = QFileInfo(abs).absolutePath();
        if (QFile::exists(QDir(parent).filePath(QLatin1String(kProjectFile)))) return load(parent, out, error);
        Project p;
        p.name = QFileInfo(abs).fileName();
        ProjectSession s;
        s.path = abs;
        if (!Session::load(abs, &s.session, error)) return false;
        s.t0 = s.session.start();
        p.sessions.push_back(std::move(s));
        *out = std::move(p);
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1").arg(f.fileName());
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QStringLiteral("format")).toString() != QLatin1String("libretap-project")) {
        if (error) *error = QStringLiteral("%1 is not a LibreTap project file").arg(f.fileName());
        return false;
    }
    Project p;
    p.folder = abs;
    p.name = o.value(QStringLiteral("name")).toString(QFileInfo(abs).fileName());
    for (const QJsonValue& v : o.value(QStringLiteral("sessions")).toArray()) {
        const QJsonObject so = v.toObject();
        ProjectSession s;
        s.path = so.value(QStringLiteral("path")).toString();
        s.offset = so.value(QStringLiteral("offset")).toDouble();
        s.drift = so.value(QStringLiteral("drift")).toDouble();
        s.t0 = so.value(QStringLiteral("t0")).toDouble();
        s.how = so.value(QStringLiteral("how")).toString(QStringLiteral("none"));
        s.match = so.value(QStringLiteral("match")).toObject();
        if (!Session::load(p.absolutePath(s), &s.session, error)) return false;
        p.sessions.push_back(std::move(s));
    }
    *out = std::move(p);
    return true;
}

bool Project::save(QString* error) const {
    if (folder.isEmpty()) {
        if (error) *error = QStringLiteral("the project has no folder yet");
        return false;
    }
    QJsonArray arr;
    for (const ProjectSession& s : sessions)
        arr.append(QJsonObject{{QStringLiteral("path"), s.path},
                               {QStringLiteral("offset"), s.offset},
                               {QStringLiteral("drift"), s.drift},
                               {QStringLiteral("t0"), s.t0},
                               {QStringLiteral("how"), s.how},
                               {QStringLiteral("match"), s.match}});
    const QJsonObject o{{QStringLiteral("format"), QStringLiteral("libretap-project")},
                        {QStringLiteral("version"), 1},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("clock"), QStringLiteral("project_time = t + offset + drift * (t - t0), per session")},
                        {QStringLiteral("sessions"), arr}};
    QSaveFile f(QDir(folder).filePath(QLatin1String(kProjectFile)));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(f.fileName());
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(f.fileName());
        return false;
    }
    return true;
}

QString Project::absolutePath(const ProjectSession& s) const {
    if (QDir::isAbsolutePath(s.path) || folder.isEmpty()) return s.path;
    return QDir(folder).filePath(s.path);
}

bool Project::addSession(const QString& sessionFolder, QString* error, int* index) {
    const QString abs = QDir(sessionFolder).absolutePath();
    if (!Session::isSession(abs)) {
        if (error) *error = QStringLiteral("%1 is not a session folder (it has no session.json)").arg(abs);
        return false;
    }
    for (const ProjectSession& s : sessions)
        if (QDir(absolutePath(s)).absolutePath() == abs) {
            if (error) *error = QStringLiteral("%1 is already part of this recording").arg(QFileInfo(abs).fileName());
            return false;
        }
    ProjectSession s;
    QString where = abs;
    if (!folder.isEmpty()) {
        const QString rel = QDir(folder).relativeFilePath(abs);
        // outside (or on another drive, where Windows has no relative path): copy it in, so the project is one folder
        if (rel.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(rel)) {
            QString target = QDir(folder).filePath(QFileInfo(abs).fileName());
            for (int k = 2; QFileInfo::exists(target); ++k) target = QDir(folder).filePath(QFileInfo(abs).fileName() + QStringLiteral("-%1").arg(k));
            if (!copyFolder(abs, target, error)) return false;
            where = target;
        }
        s.path = QDir(folder).relativeFilePath(where);
    } else {
        s.path = abs;
    }
    if (!Session::load(where, &s.session, error)) return false;
    s.t0 = s.session.start();
    s.how = sessions.empty() ? QStringLiteral("reference") : QStringLiteral("none");
    sessions.push_back(std::move(s));
    if (index) *index = int(sessions.size()) - 1;
    return true;
}

double Project::start() const {
    double t = std::numeric_limits<double>::max();
    for (const ProjectSession& s : sessions)
        if (!s.session.events.empty()) t = std::min(t, s.toProject(s.session.events.front().t));
    return t == std::numeric_limits<double>::max() ? 0 : t;
}

double Project::end() const {
    double t = std::numeric_limits<double>::lowest();
    for (const ProjectSession& s : sessions)
        if (!s.session.events.empty()) t = std::max(t, s.toProject(s.session.events.back().t));
    return t == std::numeric_limits<double>::lowest() ? 0 : t;
}

}  // namespace lt
