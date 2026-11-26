#pragma once
// Clock matching between two sessions recorded at the same time by different recorders (FORMAT.md, "Clock
// matching"): other_t + offset + drift * (other_t - t0) = ref_t.
//
// Traffic matching: the same bytes are in both sessions, e.g. what the vendor program wrote (Tick, api-serial,
// dir "out") is what the tap saw on the PC's TX line (serial-tap, dir "a"). Every piece of the other session (a
// write, a tap chunk) is looked up in the reference's byte stream of the same direction; each place it occurs gives
// a candidate time difference. The true difference collects a vote from nearly every piece, so a histogram finds it
// even when the program polls the same command over and over (each repeat votes for every period, but unique
// commands vote only for the truth). A line fit through the best candidate of each piece, with shrinking windows,
// then gives the offset and the drift (old PC clocks run fast or slow by tens of ppm).
// Bytes the program read (dir "in") reach Tick only after the vendor program asks for them, so they lag; they are
// used only when there are too few writes, and their median lag is reported.
//
// Manual sync: `sync` events with the same number pressed on both recorders (about ±0.3 s).
#include <QJsonObject>
#include <QString>

#include "project.h"
#include "session.h"

namespace lt {

struct MatchResult {
    bool ok = false;
    QString method;                // traffic | sync
    double offset = 0, drift = 0, t0 = 0;
    int matches = 0;               // pieces (or syncs) that agree
    int probes = 0;                // pieces looked up
    double residual = 0;           // RMS of the matched pieces around the fit, seconds
    double confidence = 0;         // 0..1: how clearly the best time difference beat the next best
    double readLag = 0;            // median lag of read data (Tick dir "in"), seconds; 0 if none
    QString message;               // a sentence for the operator
    QJsonObject toJson() const;
};

MatchResult matchTraffic(const Session& ref, const Session& other);
MatchResult matchSync(const Session& ref, const Session& other);
MatchResult matchSessions(const Session& ref, const Session& other);   // traffic, else sync

// Matches session `other` of the project to session `ref` and stores the result in the project (not saved).
MatchResult matchInProject(Project& p, int ref, int other);
// Matches every session that is not the reference (index 0) and not matched by hand.
void matchAll(Project& p);

}  // namespace lt
