// Session format: write and read back, a cut-off last line, garbage lines, projects.
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "core/project.h"
#include "core/session.h"
#include "core/util.h"

using namespace lt;

class TestFormat : public QObject {
    Q_OBJECT
private slots:
    void roundTrip() {
        QTemporaryDir dir;
        const QString f = dir.filePath(QStringLiteral("s1"));
        {
            SessionWriter w;
            QString err;
            QVERIFY2(w.create(f, {{QStringLiteral("note"), QStringLiteral("hello")}}, &err), qPrintable(err));
            w.setSources({QJsonObject{{QStringLiteral("id"), QStringLiteral("tap")}, {QStringLiteral("kind"), QStringLiteral("serial-tap")}}});
            w.event(100.25, QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), QStringLiteral("a")},
                                                     {QStringLiteral("hex"), QStringLiteral("2a49444e3f0d")}});
            w.event(99.5, QStringLiteral("mark"), {{QStringLiteral("n"), 1}, {QStringLiteral("note"), QStringLiteral("integration 1 s")}});
            w.event(1796031300.123456, QStringLiteral("sync"), {{QStringLiteral("n"), 1}});
            w.finish();
        }
        // lines start with t and kind, so the file reads well
        QFile ev(f + QStringLiteral("/events.jsonl"));
        QVERIFY(ev.open(QIODevice::ReadOnly));
        QVERIFY(ev.readLine().startsWith("{\"t\":100.250000,\"kind\":\"data\","));
        Session s;
        QString err;
        QVERIFY2(Session::load(f, &s, &err), qPrintable(err));
        QCOMPARE(s.events.size(), size_t(3));
        QCOMPARE(s.events[0].kind, QStringLiteral("mark"));   // sorted by t
        QCOMPARE(s.events[1].bytes(), QByteArray("*IDN?\r"));
        QCOMPARE(s.events[1].src, QStringLiteral("tap"));
        QCOMPARE(s.events[2].t, 1796031300.123456);
        QCOMPARE(s.meta.value(QStringLiteral("note")).toString(), QStringLiteral("hello"));
        QCOMPARE(s.sourceKind(QStringLiteral("tap")), QStringLiteral("serial-tap"));
        QVERIFY(s.meta.contains(QStringLiteral("ended")));
        QVERIFY(!s.cutOff);
        QCOMPARE(s.badLines, 0);
    }

    void cutOffLastLine() {
        QTemporaryDir dir;
        const QString f = dir.filePath(QStringLiteral("s2"));
        {
            SessionWriter w;
            QVERIFY(w.create(f, {}, nullptr));
            w.event(1, QStringLiteral("status"), {{QStringLiteral("text"), QStringLiteral("a")}});
            w.event(2, QStringLiteral("status"), {{QStringLiteral("text"), QStringLiteral("b")}});
        }   // no finish(): a crash
        QFile ev(f + QStringLiteral("/events.jsonl"));
        QVERIFY(ev.open(QIODevice::Append));
        ev.write("not json at all\n");                       // a damaged line in the middle
        ev.write("{\"t\":3,\"kind\":\"status\",\"text\":\"c\"}\n");
        ev.write("{\"t\":4,\"kind\":\"data\",\"hex\":\"2a4");  // the pulled USB stick
        ev.close();
        Session s;
        QVERIFY(Session::load(f, &s, nullptr));
        QCOMPARE(s.events.size(), size_t(3));
        QVERIFY(s.cutOff);
        QCOMPARE(s.badLines, 1);
        QCOMPARE(s.events.back().str("text"), QStringLiteral("c"));
    }

    void lastLineWithoutNewline() {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath(QStringLiteral("s3")));
        QFile m(dir.filePath(QStringLiteral("s3/session.json")));
        QVERIFY(m.open(QIODevice::WriteOnly));
        m.write(R"({"format":"libretap-session","version":1,"recorder":"Tick 0.1.0","sources":[],"future_key":{"x":1}})");
        m.close();
        QFile ev(dir.filePath(QStringLiteral("s3/events.jsonl")));
        QVERIFY(ev.open(QIODevice::WriteOnly));
        ev.write("{\"t\":1,\"kind\":\"mark\",\"n\":1,\"unknown\":[1,2]}\n{\"t\":2,\"kind\":\"mark\",\"n\":2}");
        ev.close();
        Session s;
        QVERIFY(Session::load(dir.filePath(QStringLiteral("s3")), &s, nullptr));
        QCOMPARE(s.events.size(), size_t(2));   // complete, just no newline: kept
        QVERIFY(!s.cutOff);
        QCOMPARE(s.recorder(), QStringLiteral("Tick 0.1.0"));
    }

    void notASession() {
        QTemporaryDir dir;
        Session s;
        QString err;
        QVERIFY(!Session::load(dir.path(), &s, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(!Session::isSession(dir.path()));
    }

    void project() {
        QTemporaryDir dir;
        Project p;
        QString err;
        QVERIFY2(Project::create(dir.filePath(QStringLiteral("rec")), &p, &err), qPrintable(err));
        // one session inside, one outside (copied in)
        for (const QString& f : {dir.filePath(QStringLiteral("rec/inside")), dir.filePath(QStringLiteral("elsewhere/tick"))}) {
            SessionWriter w;
            QVERIFY(w.create(f, {}, nullptr));
            w.event(10, QStringLiteral("mark"), {{QStringLiteral("n"), 1}});
            w.event(20, QStringLiteral("mark"), {{QStringLiteral("n"), 2}});
            w.finish();
        }
        QFile(dir.filePath(QStringLiteral("elsewhere/tick/frames/x.png"))).open(QIODevice::WriteOnly);
        QVERIFY2(p.addSession(dir.filePath(QStringLiteral("rec/inside")), &err), qPrintable(err));
        int idx = -1;
        QVERIFY2(p.addSession(dir.filePath(QStringLiteral("elsewhere/tick")), &err, &idx), qPrintable(err));
        QCOMPARE(idx, 1);
        QVERIFY(!p.addSession(dir.filePath(QStringLiteral("rec/inside")), &err));   // twice
        QCOMPARE(p.sessions[1].path, QStringLiteral("tick"));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("rec/tick/frames/x.png"))));
        p.sessions[1].offset = 5;
        p.sessions[1].drift = 0.001;
        p.sessions[1].how = QStringLiteral("manual");
        QVERIFY(p.save(&err));
        Project q;
        QVERIFY2(Project::load(dir.filePath(QStringLiteral("rec")), &q, &err), qPrintable(err));
        QCOMPARE(q.sessions.size(), size_t(2));
        QCOMPARE(q.sessions[1].how, QStringLiteral("manual"));
        QCOMPARE(q.sessions[1].toProject(20), 20 + 5 + 0.001 * 10);
        QCOMPARE(q.start(), 10.0);
        QCOMPARE(q.end(), 25.01);
        // a session folder opens its project
        Project r;
        QVERIFY(Project::load(dir.filePath(QStringLiteral("rec/inside")), &r, &err));
        QCOMPARE(r.sessions.size(), size_t(2));
    }

    void textView() {
        QCOMPARE(lt::textView(QByteArray("A\r\n\t\\\x01\xff", 7)), QStringLiteral("A\\r\\n\\t\\\\\\x01\\xff"));
        QCOMPARE(lt::hexView("*ID"), QStringLiteral("2a 49 44"));
        QCOMPARE(lt::clockText(3723.4567), QStringLiteral("1:02:03.457"));
        QCOMPARE(lt::clockText(63.5, false), QStringLiteral("1:03"));
    }
};

QTEST_GUILESS_MAIN(TestFormat)
#include "test_format.moc"
