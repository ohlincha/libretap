#pragma once
// Cuts the bytes of a two-line tap into chunks, as serialtap.py does: a chunk ends when its line has been silent
// for `gap` seconds, or as soon as the other line starts talking (so a question and its answer never merge).
// Pure logic with explicit times, so it can be tested without ports.
#include <QByteArray>

#include <vector>

namespace lt {

struct Chunk {
    int dir = 0;                   // 0 = line a (PC's TX), 1 = line b (instrument's TX)
    double t = 0;                  // when its first bytes arrived
    double end = 0;                // when its last bytes arrived
    QByteArray data;
};

class Chunker {
public:
    explicit Chunker(double gapSeconds = 0.020) : gap_(gapSeconds) {}
    void setGap(double s) { gap_ = s; }
    double gap() const { return gap_; }
    // Bytes that arrived on a line at time `now`; returns the chunks this completes.
    std::vector<Chunk> feed(int dir, const QByteArray& data, double now);
    // Chunks whose line has been silent long enough by `now`.
    std::vector<Chunk> poll(double now);
    // Everything pending (stop, or before a mark so the mark follows the bytes it comes after).
    std::vector<Chunk> flush();

private:
    struct Pending {
        bool active = false;
        Chunk c;
    };
    void take(int dir, std::vector<Chunk>* out);
    double gap_;
    Pending p_[2];
};

}  // namespace lt
