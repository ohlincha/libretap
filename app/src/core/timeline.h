#pragma once
// A project's sessions on one clock: the rows to list (traffic, marks, syncs, notes), the pictures (camera stills,
// Tick screens) and how to describe each row in plain words. Used by the GUI's timeline and `LibreTapCLI show`.
#include <QImage>
#include <QString>

#include <vector>

#include "project.h"

namespace lt {

struct TItem {
    double t;                      // project time
    int s;                         // session index
    int e;                         // event index in that session
};

struct Picture {                   // one picture source: a camera or a Tick screen
    int s = 0;
    QString src, kind, label;
    std::vector<TItem> frames;
};

struct RowText {
    QString who;                   // "PC → Instrument", "Program wrote (COM1)", "Mark 3", …
    QString text;                  // bytes as text, or the note
    QString hex;
    int dir = -1;                  // 0 PC → instrument, 1 instrument → PC, -1 other
};

class Timeline {
public:
    void build(const Project& p);
    std::vector<TItem> rows;       // everything listed, in time order
    std::vector<TItem> marks, syncs;
    std::vector<Picture> pictures;
    double start = 0, end = 0;
    int rowAt(double t) const;     // the last row at or before t (-1 before the first)
    const Event& event(const Project& p, const TItem& i) const { return p.sessions[i.s].session.events[i.e]; }
};

RowText describe(const Project& p, const TItem& i);

// Rebuilds a picture source at a time: a camera still is the last one before it; a Tick screen is the last full
// frame before it with the changed regions after it painted over (FORMAT.md). Keeps the last result to step
// forward cheaply.
class ScreenBuilder {
public:
    // frameTime: project time of the newest frame used (or < start when there is none yet).
    QImage at(const Project& p, const Picture& pic, double t, double* frameTime = nullptr);
    void reset() { cachePic_ = nullptr; }

private:
    const Picture* cachePic_ = nullptr;
    int cacheIndex_ = -1;          // frames painted up to this index
    QImage image_;
};

}  // namespace lt
