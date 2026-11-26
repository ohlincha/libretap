#include "chunker.h"

#include <algorithm>

namespace lt {

void Chunker::take(int dir, std::vector<Chunk>* out) {
    if (!p_[dir].active) return;
    out->push_back(std::move(p_[dir].c));
    p_[dir] = Pending{};
}

std::vector<Chunk> Chunker::feed(int dir, const QByteArray& data, double now) {
    std::vector<Chunk> out;
    if (data.isEmpty()) return out;
    take(1 - dir, &out);                                         // the other side started: its chunk is complete
    if (p_[dir].active && now - p_[dir].c.end > gap_) take(dir, &out);
    Pending& p = p_[dir];
    if (!p.active) {
        p.active = true;
        p.c.dir = dir;
        p.c.t = now;
    }
    p.c.end = now;
    p.c.data += data;
    return out;
}

std::vector<Chunk> Chunker::poll(double now) {
    std::vector<Chunk> out;
    for (int d = 0; d < 2; ++d)
        if (p_[d].active && now - p_[d].c.end > gap_) take(d, &out);
    std::sort(out.begin(), out.end(), [](const Chunk& a, const Chunk& b) { return a.t < b.t; });
    return out;
}

std::vector<Chunk> Chunker::flush() {
    std::vector<Chunk> out;
    take(0, &out);
    take(1, &out);
    std::sort(out.begin(), out.end(), [](const Chunk& a, const Chunk& b) { return a.t < b.t; });
    return out;
}

}  // namespace lt
