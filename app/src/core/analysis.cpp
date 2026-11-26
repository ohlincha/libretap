#include "analysis.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <climits>
#include <functional>
#include <cmath>
#include <set>

#include "util.h"

namespace lt {

bool looksText(const QByteArray& b) {
    if (b.isEmpty()) return false;
    int ok = 0;
    for (char c : b) {
        const uchar x = uchar(c);
        ok += (x >= 32 && x < 127) || x == 9 || x == 10 || x == 13;
    }
    return ok * 10 >= int(b.size()) * 9;
}

QString familyKey(const QByteArray& b) {
    if (looksText(b)) {
        QString k;
        for (int i = 0; i < b.size(); ++i) {
            const uchar x = uchar(b[i]);
            const bool digitNext = i + 1 < b.size() && uchar(b[i + 1]) >= '0' && uchar(b[i + 1]) <= '9';
            if ((x >= '0' && x <= '9') || x < 32 || k.size() >= 16 || ((x == '-' || x == '+' || x == '.') && digitNext)) break;
            k += QChar(x);
        }
        k = k.trimmed();
        return k.isEmpty() ? QStringLiteral("(number)") : k;
    }
    if (b.isEmpty()) return QStringLiteral("(empty)");
    return b.size() <= 64 ? QStringLiteral("0x%1 (%2 bytes)").arg(uchar(b[0]), 2, 16, QLatin1Char('0')).arg(b.size())
                          : QStringLiteral("0x%1 (long)").arg(uchar(b[0]), 2, 16, QLatin1Char('0'));
}

namespace {
QString linkOf(const Session& s, const Event& e) {
    const QJsonObject src = s.source(e.src);
    const QString kind = src.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("ethernet")) {
        const QString conn = e.str("conn");
        return QStringLiteral("eth ") + (e.str("proto") + QLatin1Char(' ') + conn.section(QStringLiteral(" → "), 1));
    }
    if (kind == QLatin1String("usb-pico") || kind == QLatin1String("usbpcap")) {
        const QString dev = e.str("dev");
        return QStringLiteral("usb ") + (dev.isEmpty() ? QStringLiteral("address %1").arg(e.o.value(QStringLiteral("addr")).toInt()) : dev);
    }
    if (kind == QLatin1String("api-serial")) return src.value(QStringLiteral("label")).toString(e.src).section(QLatin1Char(' '), 0, 0);
    return e.src;
}
}  // namespace

std::vector<Msg> messages(const Project& p, const Timeline& tl) {
    std::vector<Msg> out;
    for (const TItem& it : tl.rows) {
        const Event& e = tl.event(p, it);
        const bool data = e.kind == QLatin1String("data"), usb = e.kind == QLatin1String("usb");
        if (!data && !usb) continue;
        const Session& s = p.sessions[size_t(it.s)].session;
        Msg m;
        m.t = it.t;
        m.s = it.s;
        m.e = it.e;
        m.link = linkOf(s, e);
        const QString dir = e.str("dir");
        if (usb) {
            const QByteArray setup = QByteArray::fromHex(e.str("setup").toLatin1());
            if (setup.size() != 8) continue;   // bus resets, stalls: not messages
            m.control = true;
            m.role = 0;
            m.bytes = setup + e.bytes();
            m.text = e.str("what");
            const QString w = m.text.section(QLatin1Char(' '), 0, 0);
            m.family = QStringLiteral("%1 → ctl %2 %3").arg(m.link, QString::fromLatin1(setup.left(2).toHex(' ')), w);
        } else {
            m.role = (dir == QLatin1String("a") || dir == QLatin1String("out")) ? 0 : 1;
            m.bytes = e.bytes();
            if (m.bytes.isEmpty()) continue;
            m.text = textView(m.bytes, 200);
            m.family = m.link + (m.role ? QStringLiteral(" ← ") : QStringLiteral(" → ")) + familyKey(m.bytes);
        }
        out.push_back(std::move(m));
    }
    return out;
}

// ---- exchanges

QString ExchangeGroup::sentence() const {
    QString s = QStringLiteral("%1 \"%2\" asked %3 time%4").arg(link, questionText).arg(count).arg(count == 1 ? QString() : QStringLiteral("s"));
    if (count > 1 && last > first) s += QStringLiteral(" (every %1 s)").arg((last - first) / (count - 1), 0, 'f', 2);
    if (answers.empty()) return s + QStringLiteral(", never answered");
    if (answers.size() == 1)
        s += QStringLiteral(", answer %1\"%2\"").arg(count > 1 ? QStringLiteral("unchanged ") : QString(), textView(answers[0].first, 60));
    else
        s += QStringLiteral(", %1 different answers (changed %2 times), last \"%3\"").arg(answers.size()).arg(changes)
                 .arg(textView(answers.back().first, 60));
    s += QStringLiteral(", after %1 ms").arg(latency * 1000, 0, 'f', 0);
    if (unanswered) s += QStringLiteral(", %1 unanswered").arg(unanswered);
    return s;
}

QJsonObject ExchangeGroup::toJson() const {
    QJsonArray a;
    for (const auto& [bytes, n] : answers)
        a.append(QJsonObject{{QStringLiteral("text"), textView(bytes, 200)}, {QStringLiteral("hex"), QString::fromLatin1(bytes.toHex())}, {QStringLiteral("count"), n}});
    return {{QStringLiteral("link"), link}, {QStringLiteral("family"), family}, {QStringLiteral("question"), questionText},
            {QStringLiteral("question_hex"), QString::fromLatin1(question.toHex())}, {QStringLiteral("count"), count},
            {QStringLiteral("answers"), a}, {QStringLiteral("changes"), changes}, {QStringLiteral("unanswered"), unanswered},
            {QStringLiteral("first"), first}, {QStringLiteral("last"), last}, {QStringLiteral("latency_s"), latency},
            {QStringLiteral("sentence"), sentence()}};
}

std::vector<ExchangeGroup> exchanges(const std::vector<Msg>& msgs, double maxWait) {
    std::map<QString, std::vector<const Msg*>> byLink;
    for (const Msg& m : msgs) byLink[m.link].push_back(&m);
    std::map<std::pair<QString, QByteArray>, ExchangeGroup> groups;
    std::vector<std::pair<QString, QByteArray>> order;
    for (auto& [link, list] : byLink) {
        for (size_t i = 0; i < list.size(); ++i) {
            const Msg& q = *list[i];
            if (q.role != 0) continue;
            QByteArray question = q.bytes, answer;
            double at = -1;
            if (q.control) {   // the control transfer's own data stage is its answer when it reads
                question = q.bytes.left(8);
                if (uchar(q.bytes[0]) & 0x80) answer = q.bytes.mid(8), at = q.t;
                else question = q.bytes;
            }
            for (size_t j = i + 1; j < list.size() && list[j]->role == 1 && list[j]->t - q.t < maxWait; ++j) {
                if (at < 0) at = list[j]->t;
                answer += list[j]->bytes;
            }
            const auto key = std::make_pair(link, question);
            auto it = groups.find(key);
            if (it == groups.end()) {
                ExchangeGroup g;
                g.link = link;
                g.family = q.family;
                g.question = question;
                g.questionText = q.control ? q.text.section(QStringLiteral(" wLength"), 0, 0) : textView(question, 80);
                g.first = q.t;
                it = groups.emplace(key, g).first;
                order.push_back(key);
            }
            ExchangeGroup& g = it->second;
            ++g.count;
            g.last = q.t;
            if (answer.isEmpty() && at < 0) {
                ++g.unanswered;
                continue;
            }
            if (at >= 0) g.latency += at - q.t;
            auto a = std::find_if(g.answers.begin(), g.answers.end(), [&](const auto& x) { return x.first == answer; });
            if (!g.answers.empty() && g.answers.back().first != answer) ++g.changes;
            if (a == g.answers.end()) g.answers.emplace_back(answer, 1);
            else {
                const auto moved = *a;
                g.answers.erase(a);
                g.answers.emplace_back(moved.first, moved.second + 1);   // the latest answer last
            }
        }
    }
    std::vector<ExchangeGroup> out;
    for (const auto& k : order) {
        ExchangeGroup g = groups[k];
        const int answered = g.count - g.unanswered;
        if (answered > 0) g.latency /= answered;
        out.push_back(g);
    }
    std::stable_sort(out.begin(), out.end(), [](const ExchangeGroup& a, const ExchangeGroup& b) { return a.first < b.first; });
    return out;
}

// ---- moments and diff

bool resolveMoment(const Timeline& tl, const Project& p, const QString& specIn, bool asMark, double* from, double* to, QString* label,
                   QString* error) {
    QString spec = specIn.trimmed();
    if (spec.startsWith(QLatin1String("mark:"))) spec = spec.mid(5), asMark = true;
    if (spec == QLatin1String("end")) {
        *from = tl.start;
        *to = tl.end + 1;
        *label = QStringLiteral("the end");
        return true;
    }
    if (asMark) {
        const int n = spec.toInt();
        for (size_t i = 0; i < tl.marks.size(); ++i) {
            const Event& e = tl.event(p, tl.marks[i]);
            if (e.o.value(QStringLiteral("n")).toInt() != n) continue;
            *from = tl.marks[i].t;
            *to = i + 1 < tl.marks.size() ? tl.marks[i + 1].t : tl.end + 1;
            *label = QStringLiteral("mark %1%2").arg(n).arg(e.str("note").isEmpty() ? QString() : QStringLiteral(" (%1)").arg(e.str("note")));
            return true;
        }
        if (error) *error = QStringLiteral("there is no mark %1").arg(spec);
        return false;
    }
    bool ok = false;
    const double t = spec.toDouble(&ok);
    if (!ok) {
        if (error) *error = QStringLiteral("a moment is a mark number (--mark 3), a time in seconds from the start (--at 12.5) or end");
        return false;
    }
    *from = tl.start;
    *to = tl.start + t;
    *label = QStringLiteral("%1 s").arg(t);
    return true;
}

QJsonObject DiffRow::toJson() const {
    QJsonArray ch;
    for (int i : changed) ch.append(i);
    QJsonObject o{{QStringLiteral("family"), family}, {QStringLiteral("status"), status}, {QStringLiteral("changed_bytes"), ch}};
    if (a) o.insert(QStringLiteral("a"), QJsonObject{{QStringLiteral("t"), a->t}, {QStringLiteral("text"), a->text}, {QStringLiteral("hex"), QString::fromLatin1(a->bytes.toHex())}});
    if (b) o.insert(QStringLiteral("b"), QJsonObject{{QStringLiteral("t"), b->t}, {QStringLiteral("text"), b->text}, {QStringLiteral("hex"), QString::fromLatin1(b->bytes.toHex())}});
    return o;
}

std::vector<DiffRow> diffMoments(const std::vector<Msg>& msgs, double fa, double ta, double fb, double tb) {
    std::map<QString, const Msg*> A, B;
    std::vector<QString> order;
    for (const Msg& m : msgs) {
        const bool inA = m.t >= fa && m.t < ta, inB = m.t >= fb && m.t < tb;
        if (!inA && !inB) continue;
        if (!A.count(m.family) && !B.count(m.family)) order.push_back(m.family);
        if (inA) A[m.family] = &m;
        if (inB) B[m.family] = &m;
    }
    std::vector<DiffRow> out;
    for (const QString& f : order) {
        DiffRow r;
        r.family = f;
        r.a = A.count(f) ? A[f] : nullptr;
        r.b = B.count(f) ? B[f] : nullptr;
        if (r.a && r.b) {
            const QByteArray &x = r.a->bytes, &y = r.b->bytes;
            for (int i = 0; i < std::max(x.size(), y.size()); ++i)
                if (i >= x.size() || i >= y.size() || x[i] != y[i]) r.changed.push_back(i);
            r.status = r.changed.empty() ? QStringLiteral("same") : QStringLiteral("changed");
        } else {
            r.status = r.a ? QStringLiteral("only A") : QStringLiteral("only B");
        }
        out.push_back(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const DiffRow& a, const DiffRow& b) {
        auto rank = [](const DiffRow& r) { return r.status == QLatin1String("changed") ? 0 : r.status == QLatin1String("same") ? 2 : 1; };
        return rank(a) < rank(b);
    });
    return out;
}

// ---- checksums

namespace {
quint32 crc(const QByteArray& d, int from, int to, int bits, quint32 poly, quint32 init, bool reflected) {
    quint32 c = init;
    const quint32 top = 1u << (bits - 1), mask = bits == 8 ? 0xFF : 0xFFFF;
    for (int i = from; i < to; ++i) {
        const quint32 x = uchar(d[i]);
        if (reflected) {
            c ^= x;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
        } else {
            c ^= x << (bits - 8);
            for (int k = 0; k < 8; ++k) c = (c & top) ? ((c << 1) ^ poly) & mask : (c << 1) & mask;
        }
    }
    return c & mask;
}
}  // namespace

std::vector<std::pair<QString, quint32>> checksums(const QByteArray& d, int from, int to) {
    quint32 sum = 0, x = 0;
    for (int i = from; i < to; ++i) sum += uchar(d[i]), x ^= uchar(d[i]);
    return {{QStringLiteral("sum8"), sum & 0xFF},
            {QStringLiteral("xor8"), x},
            {QStringLiteral("two's complement of sum8"), (0x100 - (sum & 0xFF)) & 0xFF},
            {QStringLiteral("one's complement of sum8"), ~sum & 0xFF},
            {QStringLiteral("CRC-8 (poly 07)"), crc(d, from, to, 8, 0x07, 0, false)},
            {QStringLiteral("CRC-8/MAXIM"), crc(d, from, to, 8, 0x8C, 0, true)},
            {QStringLiteral("sum16"), sum & 0xFFFF},
            {QStringLiteral("CRC-16/MODBUS"), crc(d, from, to, 16, 0xA001, 0xFFFF, true)},
            {QStringLiteral("CRC-16/ARC"), crc(d, from, to, 16, 0xA001, 0, true)},
            {QStringLiteral("CRC-16/XMODEM"), crc(d, from, to, 16, 0x1021, 0, false)},
            {QStringLiteral("CRC-16/CCITT-FALSE"), crc(d, from, to, 16, 0x1021, 0xFFFF, false)},
            {QStringLiteral("CRC-16/KERMIT"), crc(d, from, to, 16, 0x8408, 0, true)}};
}

// ---- hints

QJsonObject FamilyHints::toJson() const {
    return {{QStringLiteral("family"), family}, {QStringLiteral("count"), count}, {QStringLiteral("distinct"), distinct},
            {QStringLiteral("text"), text}, {QStringLiteral("line_end"), lineEnd}, {QStringLiteral("min_len"), minLen},
            {QStringLiteral("max_len"), maxLen}, {QStringLiteral("header_hex"), QString::fromLatin1(header.toHex(' '))},
            {QStringLiteral("length"), QJsonArray::fromStringList(length)}, {QStringLiteral("checksum"), QJsonArray::fromStringList(checksum)},
            {QStringLiteral("varying"), QJsonArray::fromStringList(varying)}};
}

QStringList FamilyHints::lines() const {
    QStringList o;
    o << QStringLiteral("%1: %2 messages (%3 different), %4, %5")
             .arg(family).arg(count).arg(distinct).arg(text ? QStringLiteral("text") : QStringLiteral("binary"))
             .arg(minLen == maxLen ? QStringLiteral("%1 bytes").arg(minLen) : QStringLiteral("%1–%2 bytes").arg(minLen).arg(maxLen));
    if (!lineEnd.isEmpty()) o << QStringLiteral("  ends with %1").arg(lineEnd);
    if (!header.isEmpty() && count > 1) o << QStringLiteral("  always starts with %1 (\"%2\")").arg(hexView(header), textView(header));
    for (const QString& s : length) o << QStringLiteral("  length: ") + s;
    for (const QString& s : checksum) o << QStringLiteral("  checksum: ") + s;
    for (const QString& s : varying) o << QStringLiteral("  varies: ") + s;
    return o;
}

std::vector<FamilyHints> hints(const std::vector<Msg>& msgs, const Timeline& tl) {
    std::map<QString, std::vector<const Msg*>> fam;
    std::vector<QString> order;
    for (const Msg& m : msgs) {
        if (!fam.count(m.family)) order.push_back(m.family);
        fam[m.family].push_back(&m);
    }
    std::vector<FamilyHints> out;
    static const QRegularExpression number(QStringLiteral("[-+]?\\d+(?:\\.\\d+)?"));
    for (const QString& f : order) {
        const auto& list = fam[f];
        FamilyHints h;
        h.family = f;
        h.count = int(list.size());
        std::set<QByteArray> distinct;
        h.text = true;
        h.minLen = INT_MAX;
        for (const Msg* m : list) {
            distinct.insert(m->bytes);
            h.text &= !m->control && looksText(m->bytes);
            h.minLen = std::min(h.minLen, int(m->bytes.size()));
            h.maxLen = std::max(h.maxLen, int(m->bytes.size()));
        }
        h.distinct = int(distinct.size());
        std::vector<QByteArray> d(distinct.begin(), distinct.end());
        for (const char* le : {"\r\n", "\n", "\r"})
            if (std::all_of(d.begin(), d.end(), [&](const QByteArray& b) { return b.endsWith(le); })) {
                h.lineEnd = textView(le);
                break;
            }
        // the fixed start
        if (d.size() > 1) {
            int n = h.minLen;
            for (const QByteArray& b : d)
                for (int i = 0; i < n; ++i)
                    if (b[i] != d[0][i]) {
                        n = i;
                        break;
                    }
            h.header = d[0].left(std::min(n, 16));
        }
        // a length field (binary, at least two different lengths)
        if (!h.text && h.minLen != h.maxLen) {
            for (int i = 0; i < std::min(h.minLen, 8); ++i) {
                for (int k = -4; k <= 8; ++k)
                    if (std::all_of(d.begin(), d.end(), [&](const QByteArray& b) { return int(uchar(b[i])) == int(b.size()) - k; }))
                        h.length << QStringLiteral("byte %1 = message length%2").arg(i).arg(k ? QStringLiteral(" − %1").arg(k) : QString());
                if (i + 1 < h.minLen)
                    for (int k = -4; k <= 8; ++k) {
                        if (std::all_of(d.begin(), d.end(), [&](const QByteArray& b) { return int(uchar(b[i]) | uchar(b[i + 1]) << 8) == int(b.size()) - k; }))
                            h.length << QStringLiteral("bytes %1–%2 (16 bit, little endian) = length%3").arg(i).arg(i + 1).arg(k ? QStringLiteral(" − %1").arg(k) : QString());
                        if (std::all_of(d.begin(), d.end(), [&](const QByteArray& b) { return int(uchar(b[i]) << 8 | uchar(b[i + 1])) == int(b.size()) - k; }))
                            h.length << QStringLiteral("bytes %1–%2 (16 bit, big endian) = length%3").arg(i).arg(i + 1).arg(k ? QStringLiteral(" − %1").arg(k) : QString());
                    }
            }
        }
        // checksum candidates over the last byte(s), before any line ending; needs 3 different messages
        if (d.size() >= 3 && h.minLen >= 3) {
            const int le = h.lineEnd.isEmpty() ? 0 : h.lineEnd == QLatin1String("\\r\\n") ? 2 : 1;
            std::set<int> starts{0, 1, 2, int(h.header.size())};
            for (int width : {1, 2, 4}) {   // 1 or 2 raw bytes; 2 or 4 ASCII hex digits are tried with width 2 and 4 below
                for (int s0 : starts) {
                    for (const char* form : {"raw", "ascii"}) {
                        const bool ascii = QLatin1String(form) == QLatin1String("ascii");
                        if (!ascii && width == 4) continue;
                        if (ascii && width == 1) continue;
                        std::vector<QString> hits;
                        bool first = true;
                        for (const QByteArray& b : d) {
                            const int end = int(b.size()) - le - width;
                            if (end <= s0) {
                                hits.clear();
                                first = false;
                                break;
                            }
                            const QByteArray tail = b.mid(end, width);
                            std::vector<QString> ok;
                            for (const auto& [name, v] : checksums(b, s0, end)) {
                                const bool is16 = name.contains(QLatin1String("16"));
                                if (ascii) {
                                    if ((width == 2 && is16) || (width == 4 && !is16)) continue;
                                    bool good = false;
                                    const quint32 got = tail.toUInt(&good, 16);
                                    if (good && got == v) ok.push_back(name + QStringLiteral(" as hex text"));
                                } else if (width == 1 && !is16) {
                                    if (uchar(tail[0]) == v) ok.push_back(name);
                                } else if (width == 2 && is16) {
                                    if (quint32(uchar(tail[0]) | uchar(tail[1]) << 8) == v) ok.push_back(name + QStringLiteral(" (little endian)"));
                                    if (quint32(uchar(tail[0]) << 8 | uchar(tail[1])) == v) ok.push_back(name + QStringLiteral(" (big endian)"));
                                }
                            }
                            if (first) hits = ok, first = false;
                            else {
                                std::vector<QString> keep;
                                for (const QString& x : hits)
                                    if (std::find(ok.begin(), ok.end(), x) != ok.end()) keep.push_back(x);
                                hits = keep;
                            }
                            if (hits.empty()) break;
                        }
                        for (const QString& x : hits)
                            h.checksum << QStringLiteral("last %1 byte%2%3 = %4 of bytes %5 to the end")
                                              .arg(width).arg(width > 1 ? QStringLiteral("s") : QString())
                                              .arg(le ? QStringLiteral(" before the line end") : QString()).arg(x).arg(s0);
                    }
                }
            }
        }
        // what varies, and how it follows the marks
        auto atMarks = [&](const std::function<QString(const QByteArray&)>& value) {
            QStringList s;
            QString prev;
            for (size_t i = 0; i < tl.marks.size(); ++i) {
                const double from = tl.marks[i].t, to = i + 1 < tl.marks.size() ? tl.marks[i + 1].t : 1e300;
                const Msg* last = nullptr;
                for (const Msg* m : list)
                    if (m->t >= from && m->t < to) last = m;
                if (!last) continue;
                const QString v = value(last->bytes);
                if (v != prev) s << QStringLiteral("mark %1: %2").arg(i + 1).arg(v);
                prev = v;
            }
            return s.size() >= 2 ? QStringLiteral("; at the marks: ") + s.join(QStringLiteral(", ")) : QString();
        };
        if (h.distinct > 1 && h.text) {
            std::map<int, std::set<QString>> tokens;
            for (const QByteArray& b : d) {
                int k = 0;
                for (auto it = number.globalMatch(QString::fromLatin1(b)); it.hasNext(); ++k) tokens[k].insert(it.next().captured());
            }
            for (const auto& [k, vals] : tokens) {
                if (vals.size() < 2) continue;
                QStringList v(vals.begin(), vals.end());
                const int idx = k;
                h.varying << QStringLiteral("number %1: %2 values (%3%4)%5").arg(idx + 1).arg(vals.size()).arg(v.mid(0, 6).join(QStringLiteral(", ")))
                                 .arg(vals.size() > 6 ? QStringLiteral(", …") : QString())
                                 .arg(atMarks([idx](const QByteArray& b) {
                                     int j = 0;
                                     for (auto it = number.globalMatch(QString::fromLatin1(b)); it.hasNext(); ++j) {
                                         const QString c = it.next().captured();
                                         if (j == idx) return c;
                                     }
                                     return QString();
                                 }));
                if (h.varying.size() >= 8) break;
            }
        } else if (h.distinct > 1 && h.minLen == h.maxLen) {
            for (int i = 0; i < h.minLen && h.varying.size() < 12; ++i) {
                std::set<int> vals;
                for (const QByteArray& b : d) vals.insert(uchar(b[i]));
                if (vals.size() < 2) continue;
                QStringList v;
                for (int x : vals) {
                    if (v.size() == 6) {
                        v << QStringLiteral("…");
                        break;
                    }
                    v << QStringLiteral("%1").arg(x, 2, 16, QLatin1Char('0'));
                }
                h.varying << QStringLiteral("byte %1: %2 values (%3)%4").arg(i).arg(vals.size()).arg(v.join(QStringLiteral(", ")))
                                 .arg(atMarks([i](const QByteArray& b) { return QStringLiteral("%1").arg(uchar(b[i]), 2, 16, QLatin1Char('0')); }));
            }
        } else if (h.distinct > 1) {
            h.varying << QStringLiteral("length and content");
        }
        out.push_back(h);
    }
    return out;
}

// ---- stats

QJsonObject stats(const Project& p, const Timeline& tl, const std::vector<Msg>& msgs) {
    std::map<QString, std::array<qint64, 4>> links;   // messages to, bytes to, messages from, bytes from
    std::set<QString> families;
    for (const Msg& m : msgs) {
        auto& l = links[m.link];
        l[m.role * 2] += 1;
        l[m.role * 2 + 1] += m.bytes.size();
        families.insert(m.family);
    }
    QJsonArray la;
    for (const auto& [name, v] : links)
        la.append(QJsonObject{{QStringLiteral("link"), name}, {QStringLiteral("messages_to_instrument"), double(v[0])}, {QStringLiteral("bytes_to_instrument"), double(v[1])},
                              {QStringLiteral("messages_from_instrument"), double(v[2])}, {QStringLiteral("bytes_from_instrument"), double(v[3])}});
    QJsonArray pics;
    for (const Picture& pic : tl.pictures) pics.append(QJsonObject{{QStringLiteral("label"), pic.label}, {QStringLiteral("count"), int(pic.frames.size())}});
    QJsonArray sessions;
    for (const ProjectSession& s : p.sessions)
        sessions.append(QJsonObject{{QStringLiteral("name"), s.session.name()}, {QStringLiteral("recorder"), s.session.recorder()}, {QStringLiteral("clock"), s.how}});
    return {{QStringLiteral("seconds"), tl.end - tl.start}, {QStringLiteral("sessions"), sessions}, {QStringLiteral("links"), la},
            {QStringLiteral("messages"), int(msgs.size())}, {QStringLiteral("families"), int(families.size())},
            {QStringLiteral("marks"), int(tl.marks.size())}, {QStringLiteral("syncs"), int(tl.syncs.size())}, {QStringLiteral("pictures"), pics}};
}

// ---- pictures

QImage screenAt(const Project& p, const Timeline& tl, double t, int picture, double* frameTime) {
    if (picture < 0 || picture >= int(tl.pictures.size())) return {};
    ScreenBuilder sb;
    return sb.at(p, tl.pictures[size_t(picture)], t, frameTime);
}

std::vector<KeyFrame> keyFrames(const Project& p, const Timeline& tl, const std::vector<Msg>& msgs, const QString& dir) {
    std::vector<KeyFrame> out;
    if (tl.pictures.empty()) return out;
    const Picture& pic = tl.pictures[0];
    if (pic.frames.empty()) return out;
    // reasons: marks, and commands whose content changed (the instrument's answers change all the time: not reasons)
    std::vector<std::pair<double, QString>> reasons;
    for (const TItem& m : tl.marks) {
        const Event& e = tl.event(p, m);
        reasons.emplace_back(m.t, QStringLiteral("mark %1%2").arg(e.o.value(QStringLiteral("n")).toInt()).arg(e.str("note").isEmpty() ? QString() : QStringLiteral(": ") + e.str("note")));
    }
    std::map<QString, QByteArray> last;
    for (const Msg& m : msgs) {
        if (m.role != 0) continue;
        auto it = last.find(m.family);
        if (it != last.end() && it->second != m.bytes) reasons.emplace_back(m.t, QStringLiteral("%1 changed: %2").arg(m.family, textView(m.bytes, 40)));
        last[m.family] = m.bytes;
    }
    std::sort(reasons.begin(), reasons.end());
    for (const auto& [t, why] : reasons) {
        // the first picture at most 3 s later shows the result; else the last one before
        auto f = std::lower_bound(pic.frames.begin(), pic.frames.end(), t, [](const TItem& r, double v) { return r.t < v; });
        double ft;
        if (f != pic.frames.end() && f->t - t <= 3) ft = f->t;
        else if (f != pic.frames.begin()) ft = std::prev(f)->t;
        else continue;
        if (!out.empty() && ft - out.back().t < 2.0) {
            if (!out.back().reasons.contains(why)) out.back().reasons << why;
            continue;
        }
        out.push_back({ft, 0, {why}, {}});
    }
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
        ScreenBuilder sb;
        for (size_t i = 0; i < out.size(); ++i) {
            const QImage img = sb.at(p, pic, out[i].t + 1e-6);
            const QString file = QDir(dir).filePath(QStringLiteral("key_%1_%2s.png").arg(i + 1, 3, 10, QLatin1Char('0')).arg(out[i].t - tl.start, 0, 'f', 1));
            if (!img.isNull() && img.save(file)) out[i].file = file;
        }
    }
    return out;
}

// ---- export

bool exportForAi(const Project& p, const QString& dir, QString* index, QString* error) {
    if (!QDir().mkpath(dir)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(dir);
        return false;
    }
    Timeline tl;
    tl.build(p);
    const auto msgs = messages(p, tl);
    auto write = [&](const QString& name, const QStringList& lines) {
        QFile f(QDir(dir).filePath(name));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
    };
    QStringList traffic;
    for (const TItem& it : tl.rows) {
        const RowText r = describe(p, it);
        traffic << QStringLiteral("%1  %2  %3%4").arg(clockText(it.t - tl.start), 11).arg(r.who, -34).arg(r.text)
                       .arg(r.hex.isEmpty() || tl.event(p, it).kind != QLatin1String("data") ? QString() : QStringLiteral("   [") + r.hex + QLatin1Char(']'));
    }
    write(QStringLiteral("traffic.txt"), traffic);
    QStringList ex;
    for (const ExchangeGroup& g : exchanges(msgs)) ex << g.sentence();
    write(QStringLiteral("exchanges.txt"), ex);
    QStringList hl;
    for (const FamilyHints& h : hints(msgs, tl)) hl << h.lines() << QString();
    write(QStringLiteral("hints.txt"), hl);
    const auto keys = keyFrames(p, tl, msgs, QDir(dir).filePath(QStringLiteral("keyframes")));
    QStringList md;
    md << QStringLiteral("# %1").arg(p.name) << QString()
       << QStringLiteral("A LibreTap recording prepared for reading: what the vendor software sent to the instrument and back, with "
                         "pictures of its screen. Times are seconds from the start (%1). Format: docs/FORMAT.md of LibreTap.")
              .arg(isoLocal(tl.start))
       << QString() << QStringLiteral("## Sessions") << QString();
    for (const ProjectSession& s : p.sessions) {
        QStringList src;
        for (const QJsonValue& v : s.session.sources()) src << v.toObject().value(QStringLiteral("label")).toString();
        md << QStringLiteral("- %1 (%2): %3; clock: %4").arg(s.session.name(), s.session.recorder(), src.join(QStringLiteral(", ")), s.how);
    }
    md << QString() << QStringLiteral("## Files") << QString()
       << QStringLiteral("- `traffic.txt`: every message in time order (who → whom, text, hex)")
       << QStringLiteral("- `exchanges.txt`: questions and their answers, repeats folded")
       << QStringLiteral("- `hints.txt`: per kind of message: text/binary, line end, header, length field, checksum candidates, what varies")
       << QStringLiteral("- `keyframes/`: the screen when a mark was set or a command changed") << QString();
    md << QStringLiteral("## Marks (what the operator changed)") << QString();
    if (tl.marks.empty()) md << QStringLiteral("(none)");
    for (size_t i = 0; i < tl.marks.size(); ++i) {
        const Event& e = tl.event(p, tl.marks[i]);
        md << QStringLiteral("### Mark %1 at %2 s: %3").arg(e.o.value(QStringLiteral("n")).toInt()).arg(tl.marks[i].t - tl.start, 0, 'f', 1)
                  .arg(e.str("note").isEmpty() ? QStringLiteral("(no note)") : e.str("note"));
        if (i > 0) {
            const auto rows = diffMoments(msgs, tl.marks[i - 1].t, tl.marks[i].t, tl.marks[i].t, i + 1 < tl.marks.size() ? tl.marks[i + 1].t : tl.end + 1);
            QStringList changed;
            for (const DiffRow& r : rows)
                if (r.status != QLatin1String("same"))
                    changed << QStringLiteral("- %1 (%2): %3 → %4").arg(r.family, r.status, r.a ? QStringLiteral("`") + r.a->text + QLatin1Char('`') : QStringLiteral("—"),
                                                                     r.b ? QStringLiteral("`") + r.b->text + QLatin1Char('`') : QStringLiteral("—"));
            md << QString() << QStringLiteral("Compared with the mark before (last message of each kind):") << QString();
            md << (changed.isEmpty() ? QStringList{QStringLiteral("- nothing changed")} : changed.mid(0, 40));
        }
        md << QString();
    }
    md << QStringLiteral("## Key frames") << QString();
    for (const KeyFrame& k : keys)
        md << QStringLiteral("- %1 s: ![](%2) %3").arg(k.t - tl.start, 0, 'f', 1).arg(QDir(dir).relativeFilePath(k.file), k.reasons.join(QStringLiteral("; ")));
    if (keys.empty()) md << QStringLiteral("(no pictures)");
    md << QString() << QStringLiteral("## Exchanges (first 60)") << QString();
    for (const QString& s : ex.mid(0, 60)) md << QStringLiteral("- ") + s;
    write(QStringLiteral("index.md"), md);
    if (index) *index = QDir(dir).filePath(QStringLiteral("index.md"));
    return true;
}

}  // namespace lt
