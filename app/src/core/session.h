#pragma once
// A session: one recording by one recorder (LibreTap here, or Tick on the instrument's PC), as a plain folder
// (../docs/FORMAT.md): session.json, events.jsonl (append, flushed line by line), frames/, files/, raw/.
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QString>

#include <vector>

namespace lt {

// Writes a session as it is recorded. event() may be called from any thread.
class SessionWriter {
public:
    ~SessionWriter();
    // Creates the folder (and frames/, raw/) and writes session.json with these fields added to the standard ones.
    bool create(const QString& folder, const QJsonObject& meta, QString* error);
    void setSources(const QJsonArray& sources);   // rewrites session.json
    // Appends one event line and flushes it. Returns the line written (without the newline) as an object.
    QJsonObject event(double t, const QString& kind, const QJsonObject& fields = {});
    // Ends the session: session.json gets "ended" and these fields.
    void finish(const QJsonObject& extra = {});
    QString folder() const { return folder_; }
    QString path(const QString& relative) const;
    double started() const { return started_; }

private:
    void writeMeta();
    QString folder_;
    QJsonObject meta_;
    QFile events_;
    QMutex mutex_;
    double started_ = 0;
};

struct Event {
    double t = 0;
    QString kind, src;
    QJsonObject o;                 // the whole line
    QByteArray bytes() const;      // "hex" decoded
    QString str(const char* key) const { return o.value(QLatin1String(key)).toString(); }
};

struct Session {
    QString folder;
    QJsonObject meta;              // session.json
    std::vector<Event> events;     // sorted by t (stable: equal times keep file order)
    int badLines = 0;              // lines that were not JSON (not counting a cut-off last line)
    bool cutOff = false;           // the last line was incomplete (recorder stopped mid-write) and was ignored

    static bool isSession(const QString& folder);
    static bool load(const QString& folder, Session* out, QString* error);

    QString name() const;          // folder name
    QString recorder() const { return meta.value(QStringLiteral("recorder")).toString(); }
    QJsonArray sources() const { return meta.value(QStringLiteral("sources")).toArray(); }
    QJsonObject source(const QString& id) const;
    bool hasSourceKind(const QString& kind) const;
    QString sourceKind(const QString& id) const { return source(id).value(QStringLiteral("kind")).toString(); }
    double start() const;          // first event (or session.json's "t_started")
    double end() const;            // last event
    int count(const QString& kind) const;
};

}  // namespace lt
