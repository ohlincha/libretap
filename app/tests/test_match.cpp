// Clock matching: a synthetic Tick session of the same link as a tap session, with an old PC's clock off by an
// hour and running 80 ppm fast: traffic matching must recover offset and drift; Sync presses give the offset.
#include <QTemporaryDir>
#include <QtTest>

#include "core/match.h"
#include "core/timeline.h"
#include "synth.h"

using namespace lt;

class TestMatch : public QObject {
    Q_OBJECT
private slots:
    void traffic() {
        QTemporaryDir dir;
        const double offset = 3600 - 12.345, drift = 80e-6;
        const auto truth = synth::write(dir.filePath(QStringLiteral("tap")), dir.filePath(QStringLiteral("tick")), offset, drift, 900, false);
        Session tap, tick;
        QVERIFY(Session::load(dir.filePath(QStringLiteral("tap")), &tap, nullptr));
        QVERIFY(Session::load(dir.filePath(QStringLiteral("tick")), &tick, nullptr));
        const MatchResult m = matchTraffic(tap, tick);
        qInfo().noquote() << m.message << QStringLiteral("offset %1 drift %2 ppm residual %3 ms").arg(m.offset, 0, 'f', 4).arg(m.drift * 1e6, 0, 'f', 2).arg(m.residual * 1000, 0, 'f', 2);
        QVERIFY2(m.ok, qPrintable(m.message));
        QCOMPARE(m.method, QStringLiteral("traffic"));
        // tap_t = tick_t + offset + d (tick_t - t0) with t0 = the first tick event; the truth at t0:
        const double expect = (truth.tickT0 + truth.offset + (m.t0 - truth.tickT0) * (1 + drift)) - m.t0;
        QVERIFY2(std::abs(m.offset - expect) < 0.006, qPrintable(QString::number(m.offset - expect)));   // within the 1-4 ms wire delay
        QVERIFY2(std::abs(m.drift - drift) < 5e-6, qPrintable(QString::number(m.drift * 1e6)));
        QVERIFY(m.matches > 1000);
        QVERIFY(m.residual < 0.003);
        QVERIFY(m.readLag > 0.015 && m.readLag < 0.08);     // ReadFile comes 20-60 ms after the bytes
    }

    void identicalPolls() {
        // only "MEAS?" every 0.5 s: the writes match at every period; the answers' readings decide
        QTemporaryDir dir;
        synth::write(dir.filePath(QStringLiteral("tap")), dir.filePath(QStringLiteral("tick")), 5.0, 0, 60, false, 0);
        Session tap, tick;
        QVERIFY(Session::load(dir.filePath(QStringLiteral("tap")), &tap, nullptr));
        QVERIFY(Session::load(dir.filePath(QStringLiteral("tick")), &tick, nullptr));
        const MatchResult m = matchTraffic(tap, tick);
        QVERIFY2(m.ok, qPrintable(m.message));
        QVERIFY2(std::abs(m.offset - 5.0025) < 0.004, qPrintable(QString::number(m.offset, 'f', 4)));
    }

    void syncPresses() {
        QTemporaryDir dir;
        SessionWriter a, b;
        QVERIFY(a.create(dir.filePath(QStringLiteral("a")), {}, nullptr));
        QVERIFY(b.create(dir.filePath(QStringLiteral("b")), {}, nullptr));
        a.event(1000.0, QStringLiteral("sync"), {{QStringLiteral("n"), 1}});
        b.event(400.2, QStringLiteral("sync"), {{QStringLiteral("n"), 1}});
        a.event(1100.0, QStringLiteral("sync"), {{QStringLiteral("n"), 2}});   // only on one side: ignored
        a.finish();
        b.finish();
        Session sa, sb;
        QVERIFY(Session::load(dir.filePath(QStringLiteral("a")), &sa, nullptr));
        QVERIFY(Session::load(dir.filePath(QStringLiteral("b")), &sb, nullptr));
        const MatchResult m = matchSessions(sa, sb);   // no traffic: falls back to Sync
        QVERIFY(m.ok);
        QCOMPARE(m.method, QStringLiteral("sync"));
        QCOMPARE(m.matches, 1);
        QVERIFY(std::abs(m.offset - 599.8) < 1e-6);
        QCOMPARE(m.drift, 0.0);
    }

    void projectAndScreen() {
        // import into a project, then the timeline: the Tick screen rebuilt from full frames + regions
        QTemporaryDir dir;
        synth::write(dir.filePath(QStringLiteral("rec/tap")), dir.filePath(QStringLiteral("tick")), -7.5, 20e-6, 120, true);
        Project p;
        QString err;
        QVERIFY(Project::create(dir.filePath(QStringLiteral("rec")), &p, &err));
        QVERIFY(p.addSession(dir.filePath(QStringLiteral("rec/tap")), &err));
        int idx;
        QVERIFY2(p.addSession(dir.filePath(QStringLiteral("tick")), &err, &idx), qPrintable(err));
        const MatchResult m = matchInProject(p, 0, idx);
        QVERIFY2(m.ok, qPrintable(m.message));
        QCOMPARE(p.sessions[1].how, QStringLiteral("traffic"));
        QVERIFY(std::abs(p.sessions[1].offset + 7.5) < 0.01);
        QVERIFY(p.save(&err));
        Timeline tl;
        tl.build(p);
        QCOMPARE(tl.pictures.size(), size_t(1));
        QCOMPARE(tl.pictures[0].kind, QStringLiteral("screen"));
        // each tap write and its Tick WriteFile now sit within a few ms of each other
        int close = 0, writes = 0;
        for (size_t i = 0; i + 1 < tl.rows.size(); ++i) {
            const Event& e = tl.event(p, tl.rows[i]);
            if (e.kind != QLatin1String("data") || e.str("dir") != QLatin1String("out")) continue;
            ++writes;
            for (size_t j = i > 6 ? i - 6 : 0; j < std::min(tl.rows.size(), i + 6); ++j)
                if (tl.event(p, tl.rows[j]).str("dir") == QLatin1String("a") && std::abs(tl.rows[j].t - tl.rows[i].t) < 0.01) {
                    ++close;
                    break;
                }
        }
        QVERIFY(writes > 100);
        QCOMPARE(close, writes);
        ScreenBuilder sb;
        const Picture& pic = tl.pictures[0];
        double ft = 0;
        const QImage before = sb.at(p, pic, pic.frames.front().t - 1, &ft);
        QVERIFY(before.isNull());
        // after a region frame (not full) the picture is the full frame with the region painted in
        size_t k = 1;
        while (k < pic.frames.size() && tl.event(p, pic.frames[k]).o.value(QStringLiteral("full")).toBool()) ++k;
        QVERIFY(k < pic.frames.size());
        const QImage img = sb.at(p, pic, pic.frames[k].t + 0.01, &ft);
        QCOMPARE(img.size(), QSize(320, 200));
        QCOMPARE(ft, pic.frames[k].t);
        const QImage region(QDir(p.sessions[1].session.folder).filePath(tl.event(p, pic.frames[k]).str("file")));
        QCOMPARE(img.pixel(150, 90), region.pixel(50, 10));
        QCOMPARE(img.pixel(10, 10), QColor(0, 128, 128).rgb());
        // stepping forward from the cache and starting fresh give the same picture
        const QImage later = sb.at(p, pic, pic.frames.back().t + 0.01);
        ScreenBuilder fresh;
        QCOMPARE(later, fresh.at(p, pic, pic.frames.back().t + 0.01));
    }
};

QTEST_GUILESS_MAIN(TestMatch)
#include "test_match.moc"
