#pragma once
// Analysis: the second half of LibreTap. Works on a project's timeline (all sessions on one clock) and answers the
// questions one asks of a recording, for the operator (GUI) and for an assistant (LibreTapCLI, JSON):
//   messages   every payload on every link, with its direction and its "family" (same kind of message)
//   exchanges  question → answer pairs per link, identical repeats folded ("MEAS? asked 240 times, answer changed")
//   diff       what differs between two moments (marks or times), per message family, byte by byte
//   hints      per family: text or binary, line ending, fixed header, length field, checksum candidates, the parts
//              that vary and how they follow the marks
//   grep, stats, key frames (screen changes near traffic changes), an export for an AI or a person.
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <map>
#include <vector>

#include "project.h"
#include "timeline.h"

namespace lt {

struct Msg {
    double t = 0;                  // project time
    int s = 0, e = 0;              // session, event
    int role = 0;                  // 0 towards the instrument (PC/host/program wrote), 1 from it
    QByteArray bytes;              // payload (USB control: the 8 setup bytes + the data stage)
    QString link;                  // which conversation: "tap", "COM1", "usb 04d8:00df", "eth 192.168.0.20:5025"
    QString family;                // link + direction + kind of message
    QString text;                  // in words (textView, or the USB request's name)
    bool control = false;          // a USB control transfer
};

bool looksText(const QByteArray& b);
QString familyKey(const QByteArray& b);   // "MEAS?", "SET INT", "IO L", "0x80 (16 bytes)"
std::vector<Msg> messages(const Project& p, const Timeline& tl);

struct ExchangeGroup {
    QString link, family;
    QByteArray question;
    QString questionText;
    int count = 0;
    std::vector<std::pair<QByteArray, int>> answers;   // distinct answers, in order of first appearance, with counts
    int changes = 0;                                    // times the answer differed from the previous one
    int unanswered = 0;
    double first = 0, last = 0, latency = 0;            // mean answer time, s
    QString sentence() const;
    QJsonObject toJson() const;
};
std::vector<ExchangeGroup> exchanges(const std::vector<Msg>& msgs, double maxWait = 2.0);

// A moment: "mark:3" (or "3" with marks), "12.5" (seconds from the start), "end". The window is from the mark to
// the next mark (or the end); for a time, everything up to it.
bool resolveMoment(const Timeline& tl, const Project& p, const QString& spec, bool asMark, double* from, double* to, QString* label,
                   QString* error);

struct DiffRow {
    QString family;
    const Msg* a = nullptr;
    const Msg* b = nullptr;
    std::vector<int> changed;      // byte positions that differ (and positions present in only one)
    QString status;                // same | changed | only A | only B
    QJsonObject toJson() const;
};
std::vector<DiffRow> diffMoments(const std::vector<Msg>& msgs, double fromA, double toA, double fromB, double toB);

struct FamilyHints {
    QString family;
    int count = 0, distinct = 0, minLen = 0, maxLen = 0;
    bool text = false;
    QString lineEnd;               // \r, \n, \r\n or empty
    QByteArray header;             // bytes that never change at the start
    QStringList length, checksum, varying;
    QJsonObject toJson() const;
    QStringList lines() const;     // plain words
};
std::vector<FamilyHints> hints(const std::vector<Msg>& msgs, const Timeline& tl);

// Checksum and CRC candidates over data[from, to): name → value (8 or 16 bit).
std::vector<std::pair<QString, quint32>> checksums(const QByteArray& data, int from, int to);

QJsonObject stats(const Project& p, const Timeline& tl, const std::vector<Msg>& msgs);

struct KeyFrame {
    double t = 0;
    int picture = 0;
    QStringList reasons;           // "mark 3: integration 1 s", "SET INT changed (PC → Instrument)"
    QString file;                  // exported picture
};
// Pictures shortly after a mark or a change in the traffic (a message family whose content changed), at most one
// per 2 s; exported as PNG into dir (when not empty).
std::vector<KeyFrame> keyFrames(const Project& p, const Timeline& tl, const std::vector<Msg>& msgs, const QString& dir);

// Folder for an assistant or a person: index.md, traffic.txt, exchanges.txt, hints.txt, diffs between marks,
// key frames. Returns the index path.
bool exportForAi(const Project& p, const QString& dir, QString* index, QString* error);

// The picture of a source at a time (camera still or rebuilt Tick screen); picture = index in tl.pictures.
QImage screenAt(const Project& p, const Timeline& tl, double t, int picture = 0, double* frameTime = nullptr);

}  // namespace lt
