#include "timeline.h"

#include <QDir>
#include <QPainter>

#include <algorithm>

#include "util.h"

namespace lt {

void Timeline::build(const Project& p) {
    rows.clear();
    marks.clear();
    syncs.clear();
    pictures.clear();
    for (int s = 0; s < int(p.sessions.size()); ++s) {
        const ProjectSession& ps = p.sessions[s];
        const Session& ss = ps.session;
        for (int e = 0; e < int(ss.events.size()); ++e) {
            const Event& ev = ss.events[e];
            const TItem it{ps.toProject(ev.t), s, e};
            if (ev.kind == QLatin1String("frame")) {
                auto pic = std::find_if(pictures.begin(), pictures.end(), [&](const Picture& x) { return x.s == s && x.src == ev.src; });
                if (pic == pictures.end()) {
                    Picture np;
                    np.s = s;
                    np.src = ev.src;
                    np.kind = ss.sourceKind(ev.src);
                    const QString label = ss.source(ev.src).value(QStringLiteral("label")).toString();
                    np.label = np.kind == QLatin1String("screen") ? QStringLiteral("Screen (Tick on %1)").arg(ss.meta.value(QStringLiteral("host")).toString(QStringLiteral("the instrument PC")))
                                                                  : QStringLiteral("Camera") + (label.isEmpty() ? QString() : QStringLiteral(": ") + label);
                    pictures.push_back(np);
                    pic = pictures.end() - 1;
                }
                pic->frames.push_back(it);
                continue;
            }
            if (ev.kind == QLatin1String("mark")) marks.push_back(it);
            if (ev.kind == QLatin1String("sync")) syncs.push_back(it);
            rows.push_back(it);
        }
    }
    auto byT = [](const TItem& a, const TItem& b) { return a.t < b.t; };
    std::stable_sort(rows.begin(), rows.end(), byT);
    std::stable_sort(marks.begin(), marks.end(), byT);
    std::stable_sort(syncs.begin(), syncs.end(), byT);
    for (Picture& pic : pictures) std::stable_sort(pic.frames.begin(), pic.frames.end(), byT);
    // a camera first: it is what the operator saw; Tick screens are sharper, so they come first when present
    std::stable_sort(pictures.begin(), pictures.end(), [](const Picture& a, const Picture& b) {
        return (a.kind == QLatin1String("screen")) > (b.kind == QLatin1String("screen"));
    });
    start = p.start();
    end = p.end();
}

int Timeline::rowAt(double t) const {
    auto it = std::upper_bound(rows.begin(), rows.end(), t, [](double v, const TItem& r) { return v < r.t; });
    return int(it - rows.begin()) - 1;
}

RowText describe(const Project& p, const TItem& i) {
    const Session& s = p.sessions[i.s].session;
    const Event& e = s.events[i.e];
    RowText r;
    if (e.kind == QLatin1String("data")) {
        const QByteArray b = e.bytes();
        r.text = textView(b);
        r.hex = hexView(b);
        const QString dir = e.str("dir");
        const QJsonObject src = s.source(e.src);
        if (dir == QLatin1String("a") || dir == QLatin1String("b")) {
            const QJsonObject names = src.value(QStringLiteral("names")).toObject();
            const QString a = names.value(QStringLiteral("a")).toString(QStringLiteral("PC"));
            const QString bn = names.value(QStringLiteral("b")).toString(QStringLiteral("Instrument"));
            r.dir = dir == QLatin1String("a") ? 0 : 1;
            r.who = r.dir == 0 ? a + QStringLiteral(" → ") + bn : bn + QStringLiteral(" → ") + a;
        } else {
            QString label = src.value(QStringLiteral("label")).toString(e.src);
            if (const int k = label.indexOf(QStringLiteral(" (")); k > 0) label = label.left(k);   // "COM1 (VENDOR.EXE)" → COM1
            r.dir = dir == QLatin1String("out") ? 0 : dir == QLatin1String("in") ? 1 : -1;
            r.who = (r.dir == 0 ? QStringLiteral("Program wrote") : r.dir == 1 ? QStringLiteral("Program read") : dir) +
                    QStringLiteral(" (%1)").arg(label);
        }
    } else if (e.kind == QLatin1String("mark")) {
        r.who = QStringLiteral("Mark %1").arg(e.o.value(QStringLiteral("n")).toInt());
        r.text = e.str("note");
    } else if (e.kind == QLatin1String("sync")) {
        r.who = QStringLiteral("Sync %1").arg(e.o.value(QStringLiteral("n")).toInt());
        r.text = QStringLiteral("(%1)").arg(s.recorder());
    } else if (e.kind == QLatin1String("config")) {
        r.who = QStringLiteral("Port settings");
        QStringList parts;
        for (auto it = e.o.begin(); it != e.o.end(); ++it)
            if (it.key() != QLatin1String("t") && it.key() != QLatin1String("kind") && it.key() != QLatin1String("src"))
                parts << it.key() + QLatin1Char('=') + (it->isString() ? it->toString() : it->isObject() ? QStringLiteral("{…}") : QString::number(it->toDouble()));
        r.text = e.src + QStringLiteral(": ") + parts.join(QLatin1Char(' '));
    } else if (e.kind == QLatin1String("input")) {
        r.who = QStringLiteral("Operator");
        const QString what = e.str("what");
        r.text = what == QLatin1String("window") ? QStringLiteral("window: %1").arg(e.str("title"))
                 : what == QLatin1String("click") ? QStringLiteral("%1 click at %2,%3 in \"%4\"").arg(e.str("button")).arg(e.o.value(QStringLiteral("x")).toInt()).arg(e.o.value(QStringLiteral("y")).toInt()).arg(e.str("window"))
                 : what == QLatin1String("key") ? QStringLiteral("key %1").arg(e.str("key"))
                                                 : what;
    } else if (e.kind == QLatin1String("open") || e.kind == QLatin1String("close")) {
        r.who = e.kind == QLatin1String("open") ? QStringLiteral("Port opened") : QStringLiteral("Port closed");
        r.text = e.src + (e.o.contains(QStringLiteral("path")) ? QStringLiteral(" ") + e.str("path") : QString());
    } else if (e.kind == QLatin1String("call")) {
        r.who = e.str("call");
        r.text = e.src + QStringLiteral(" ") + e.str("code");
        r.hex = e.str("in");
    } else if (e.kind == QLatin1String("error")) {
        r.who = QStringLiteral("Problem");
        r.text = e.str("text");
    } else {
        r.who = e.kind == QLatin1String("status") ? QStringLiteral("Note") : e.kind;
        r.text = e.str("text");
    }
    return r;
}

QImage ScreenBuilder::at(const Project& p, const Picture& pic, double t, double* frameTime) {
    const auto& fr = pic.frames;
    const int last = int(std::upper_bound(fr.begin(), fr.end(), t, [](double v, const TItem& r) { return v < r.t; }) - fr.begin()) - 1;
    if (frameTime) *frameTime = last >= 0 ? fr[last].t : -1e300;
    if (last < 0) return {};
    const Session& s = p.sessions[pic.s].session;
    auto load = [&](int k) { return QImage(QDir(s.folder).filePath(s.events[fr[k].e].str("file"))); };
    auto isFull = [&](int k) { return s.events[fr[k].e].o.value(QStringLiteral("full")).toBool(true); };
    if (pic.kind != QLatin1String("screen")) return load(last);   // camera stills are whole pictures
    int from = last;
    while (from > 0 && !isFull(from)) --from;                     // the last full frame
    int k = from;
    if (cachePic_ == &pic && cacheIndex_ >= from && cacheIndex_ <= last && !image_.isNull()) {
        k = cacheIndex_ + 1;                                       // continue from the cache
    } else {
        image_ = load(from).convertToFormat(QImage::Format_RGB32);
        k = from + 1;
    }
    QPainter painter;
    for (; k <= last; ++k) {
        const QJsonObject& o = s.events[fr[k].e].o;
        const QImage part = load(k);
        if (isFull(k)) {
            image_ = part.convertToFormat(QImage::Format_RGB32);
            continue;
        }
        if (image_.isNull()) continue;
        if (!painter.isActive()) painter.begin(&image_);
        painter.drawImage(QPoint(o.value(QStringLiteral("x")).toInt(), o.value(QStringLiteral("y")).toInt()), part);
    }
    if (painter.isActive()) painter.end();
    cachePic_ = &pic;
    cacheIndex_ = last;
    return image_;
}

}  // namespace lt
