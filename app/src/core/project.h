#pragma once
// A project: sessions recorded at the same time (LibreTap's own, Tick's from the instrument PC) and the clock
// relation between them. It is a folder with project.json and the session folders inside it:
//
//   20261130-101500/                  (a project; every LibreTap recording starts one)
//     project.json
//     20261130-101500-libretap/       (a session)
//     20261130-101448-WIN2000-LAB/    (a Tick session, copied in by "Add Tick session")
//
// project_time = t + offset + drift * (t - t0) for each session; the first session (LibreTap's) is the reference.
#include <QJsonObject>
#include <QString>

#include <vector>

#include "session.h"

namespace lt {

struct ProjectSession {
    QString path;                  // relative to the project folder (or absolute when outside it)
    double offset = 0, drift = 0, t0 = 0;
    QString how = QStringLiteral("reference");   // reference | traffic | sync | manual | none
    QJsonObject match;             // the matching result (matches, residual, …)
    Session session;
    double toProject(double t) const { return t + offset + drift * (t - t0); }
};

class Project {
public:
    QString folder;                // empty: not saved yet (a session opened on its own)
    QString name;
    std::vector<ProjectSession> sessions;

    // A project folder, or a session folder (its project when it sits in one, else a project of one session).
    static bool load(const QString& path, Project* out, QString* error);
    // A new project folder with project.json and no sessions.
    static bool create(const QString& folder, Project* out, QString* error);
    bool save(QString* error) const;
    // Adds a session folder; copies it into the project folder unless it is already inside.
    bool addSession(const QString& sessionFolder, QString* error, int* index = nullptr);
    bool isSaved() const { return !folder.isEmpty(); }

    double start() const;          // project time of the first event
    double end() const;
    QString absolutePath(const ProjectSession& s) const;
};

// Copies a folder recursively.
bool copyFolder(const QString& from, const QString& to, QString* error);

}  // namespace lt
