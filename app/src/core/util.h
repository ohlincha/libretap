#pragma once
// Small helpers shared by the library, the CLI and the GUI: the clock, byte views, names and folders.
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace lt {

// Wall clock, seconds since 1970-01-01 UTC with microseconds: the `t` of every event (FORMAT.md).
double wallNow();

// Bytes as text with escapes, as serialtap.py shows them: printable ASCII as is, CR/LF/TAB as \r \n \t,
// a backslash doubled, everything else as \xNN.
QString textView(const QByteArray& b, int maxBytes = -1);
// "2a 49 44"
QString hexView(const QByteArray& b, int maxBytes = -1);

QString isoLocal(double t);       // 2026-11-30T10:15:00.123+01:00
QString stampName(double t);      // 20261130-101500 (local time; folder names)
QString clockText(double seconds, bool millis = true);   // 1:02:03.456 or 2:03.456

// One events.jsonl line: {"t":…,"kind":"…",<fields>}\n with t and kind first so the file reads well.
QByteArray eventLine(double t, const QString& kind, const QJsonObject& fields);

// Where recordings go: $LIBRETAP_HOME, else the GUI's choice (QSettings "workspace"), else ~/LibreTap.
QString workspaceDir();

QString recorderName();           // "LibreTap 0.1.0"

}  // namespace lt
