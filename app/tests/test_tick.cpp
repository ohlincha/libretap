// A real Tick session (Tick 0.2.0 under Wine, Windows XP mode, fakevendor.exe on COM1; tests/data): it reads, its
// screen rebuilds from the full frame and the changed regions, its serial calls are listed, and it combines with
// a LibreTap tap session of the same traffic (clock offset found from the bytes).
#include <QTemporaryDir>
#include <QtTest>

#include "core/match.h"
#include "core/project.h"
#include "core/timeline.h"

using namespace lt;

namespace {
const QString kFixture = QStringLiteral(TEST_DATA "/20261126-225641-tick");
}

class TestTick : public QObject {
    Q_OBJECT
private slots:
    void reads() {
        Session s;
        QString err;
        QVERIFY2(Session::load(kFixture, &s, &err), qPrintable(err));
        QCOMPARE(s.recorder(), QStringLiteral("Tick 0.2.0"));
        QCOMPARE(s.sourceKind(QStringLiteral("com1")), QStringLiteral("api-serial"));
        QCOMPARE(s.sourceKind(QStringLiteral("screen")), QStringLiteral("screen"));
        QVERIFY(!s.cutOff);
        QCOMPARE(s.badLines, 0);
        QCOMPARE(s.count(QStringLiteral("frame")), 9);
        // the serial calls the vendor program made
        QStringList out, in;
        for (const Event& e : s.events)
            if (e.kind == QLatin1String("data")) (e.str("dir") == QLatin1String("out") ? out : in) << QString::fromLatin1(e.bytes());
        QCOMPARE(out.size(), 7);
        QCOMPARE(in.size(), 7);
        QCOMPARE(out.first(), QStringLiteral("*IDN? 0\r"));
        QCOMPARE(in.first(), QStringLiteral("TICKTEST,*IDN? 0\r\n"));
        bool baud = false;
        for (const Event& e : s.events) baud |= e.kind == QLatin1String("config") && e.o.value(QStringLiteral("baud")).toInt() == 9600;
        QVERIFY(baud);
        // in the timeline's words
        Project p;
        QVERIFY(Project::load(kFixture, &p, &err));
        Timeline tl;
        tl.build(p);
        QStringList who;
        for (const TItem& it : tl.rows)
            if (tl.event(p, it).kind == QLatin1String("data")) who << describe(p, it).who;
        QCOMPARE(who.first(), QStringLiteral("Program wrote (COM1)"));
        QCOMPARE(who[1], QStringLiteral("Program read (COM1)"));
    }

    void screen() {
        Project p;
        QVERIFY(Project::load(kFixture, &p, nullptr));
        Timeline tl;
        tl.build(p);
        QCOMPARE(tl.pictures.size(), size_t(1));
        const Picture& pic = tl.pictures[0];
        QCOMPARE(pic.label, QStringLiteral("Screen (Tick on TESTPC)"));
        const QString frames = kFixture + QStringLiteral("/frames/");
        const QImage full(frames + QStringLiteral("000001.png")), f8(frames + QStringLiteral("000008.png")), f2(frames + QStringLiteral("000002.png"));
        ScreenBuilder sb;
        QVERIFY(sb.at(p, pic, pic.frames.front().t - 0.1).isNull());          // before the first picture
        QImage a = sb.at(p, pic, pic.frames.front().t + 0.01);
        QCOMPARE(a.size(), QSize(1024, 768));
        QCOMPARE(a.pixel(500, 600), full.pixel(500, 600));
        // after the second (a region at 0,0 of 672×448): painted over the first
        a = sb.at(p, pic, pic.frames[1].t + 0.01);
        QCOMPARE(a.pixel(100, 100), f2.pixel(100, 100));
        QCOMPARE(a.pixel(900, 700), full.pixel(900, 700));                   // outside the region: unchanged
        // at the end: every region in place (stepping forward from the cache, and fresh)
        const QImage end = sb.at(p, pic, pic.frames.back().t + 0.01);
        QCOMPARE(end.pixel(352 + 10, 320 + 10), f8.pixel(10, 10));
        QCOMPARE(end.pixel(352 + 300, 320 + 100), f8.pixel(300, 100));
        ScreenBuilder fresh;
        QCOMPARE(fresh.at(p, pic, pic.frames.back().t + 0.01), end);
        // going back in time gives the earlier picture again
        ScreenBuilder fresh2;
        QCOMPARE(sb.at(p, pic, pic.frames[1].t + 0.01), fresh2.at(p, pic, pic.frames[1].t + 0.01));
    }

    void beforeAndAfter() {
        Project p;
        QVERIFY(Project::load(kFixture, &p, nullptr));
        Timeline tl;
        tl.build(p);
        const Picture& pic = tl.pictures[0];
        // just after the first picture: before = the first, after = the second (a region at 0,0 of 672×448)
        const BeforeAfter ba = beforeAfter(p, pic, pic.frames[0].t + 0.01);
        QVERIFY(!ba.before.isNull() && !ba.after.isNull());
        QCOMPARE(ba.afterT, pic.frames[1].t);
        QVERIFY(!ba.changed.empty());
        for (const QRect& r : ba.changed) QVERIFY2(QRect(0, 0, 672, 448).contains(r), qPrintable(QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())));
        // after the last picture: nothing changes any more
        const BeforeAfter end = beforeAfter(p, pic, pic.frames.back().t + 0.01);
        QVERIFY(!end.before.isNull());
        QVERIFY(end.after.isNull());
        // the same picture: no change; noise below the threshold: no change; one changed block: found
        QImage a(320, 200, QImage::Format_RGB32);
        a.fill(Qt::white);
        QVERIFY(changedRegions(a, a).empty());
        QImage b = a;
        for (int x = 0; x < 320; x += 7) b.setPixel(x, 50, qRgb(230, 230, 230));   // faint speckle
        QVERIFY(changedRegions(a, b).empty());
        for (int y = 100; y < 120; ++y)
            for (int x = 200; x < 260; ++x) b.setPixel(x, y, qRgb(0, 0, 0));
        const auto ch = changedRegions(a, b);
        QCOMPARE(ch.size(), size_t(1));
        QVERIFY(ch[0].contains(QRect(200, 100, 60, 20)));
        QVERIFY(ch[0].width() <= 60 + 32 && ch[0].height() <= 20 + 32);
    }

    void importAndMatch() {
        // a LibreTap tap session that saw the same bytes, its clock 3.25 s ahead of the old PC's
        Session tick;
        QVERIFY(Session::load(kFixture, &tick, nullptr));
        QTemporaryDir dir;
        Project p;
        QString err;
        QVERIFY(Project::create(dir.filePath(QStringLiteral("rec")), &p, &err));
        {
            SessionWriter w;
            QVERIFY(w.create(dir.filePath(QStringLiteral("rec/tap")), {}, nullptr));
            w.setSources({QJsonObject{{QStringLiteral("id"), QStringLiteral("tap")}, {QStringLiteral("kind"), QStringLiteral("serial-tap")}, {QStringLiteral("baud"), 9600}}});
            for (const Event& e : tick.events)
                if (e.kind == QLatin1String("data")) {
                    const bool out = e.str("dir") == QLatin1String("out");
                    w.event(e.t + 3.25 + (out ? 0.002 : -0.030), QStringLiteral("data"),
                            {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), out ? QStringLiteral("a") : QStringLiteral("b")},
                             {QStringLiteral("hex"), e.str("hex")}});
                }
            w.finish();
        }
        QVERIFY(p.addSession(dir.filePath(QStringLiteral("rec/tap")), &err));
        int idx = -1;
        QVERIFY2(p.addSession(kFixture, &err, &idx), qPrintable(err));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("rec/20261126-225641-tick/frames/000009.png"))));   // copied in
        const MatchResult m = matchInProject(p, 0, idx);
        QVERIFY2(m.ok, qPrintable(m.message));
        QVERIFY2(std::abs(p.sessions[1].offset - 3.252) < 0.003, qPrintable(QString::number(p.sessions[1].offset, 'f', 4)));
        QVERIFY(p.save(&err));
        Timeline tl;
        tl.build(p);
        QCOMPARE(tl.pictures.size(), size_t(1));
        QCOMPARE(tl.pictures[0].s, 1);
    }
};

QTEST_GUILESS_MAIN(TestTick)
#include "test_tick.moc"
