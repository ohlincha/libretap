// LibreTapCLI end to end: a headless recorder (the demo instrument) driven by status/mark/sync/stop through the
// control socket, then sessions/show/import/match on what it wrote.
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "synth.h"

namespace {
QProcessEnvironment env(const QString& ws) {
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    e.insert(QStringLiteral("LIBRETAP_HOME"), ws);
    e.insert(QStringLiteral("LIBRETAP_NO_SYSTEM_CAMERAS"), QStringLiteral("1"));
    if (!e.contains(QStringLiteral("LIBRETAP_CONTROL"))) e.insert(QStringLiteral("LIBRETAP_CONTROL"), QStringLiteral("libretap-test-cli"));
    return e;
}

QJsonObject run(const QString& ws, const QStringList& args, int* code = nullptr) {
    QProcess p;
    p.setProcessEnvironment(env(ws));
    p.start(QStringLiteral(CLI_PATH), args + QStringList{QStringLiteral("--json")});
    p.waitForFinished(30000);
    if (code) *code = p.exitCode();
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
}
}  // namespace

class TestCli : public QObject {
    Q_OBJECT
private slots:
    void tickBundle() {
        QTemporaryDir ws;
        // a pretend installed Tick, read-only like /usr/share/libretap/tick
        const QString b = ws.filePath(QStringLiteral("bundle"));
        QDir().mkpath(b);
        for (const char* n : {"Tick.exe", "tick", "SAFETY.md"}) {
            QFile f(QDir(b).filePath(QLatin1String(n)));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(n);
            f.close();
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther |
                             (QLatin1String(n) == QLatin1String("tick") ? QFileDevice::ExeOwner : QFileDevice::Permissions()));
        }
        qputenv("LIBRETAP_TICK_DIR", QFile::encodeName(b));
        const QJsonObject info = run(ws.path(), {QStringLiteral("tick")});
        QVERIFY(info.value(QStringLiteral("ok")).toBool());
        QCOMPARE(info.value(QStringLiteral("files")).toArray().size(), 3);
        const QString stick = ws.filePath(QStringLiteral("stick"));
        QDir().mkpath(stick);
        for (int pass = 0; pass < 2; ++pass) {   // twice: a second copy replaces the read-only-sourced files
            const QJsonObject c = run(ws.path(), {QStringLiteral("tick"), QStringLiteral("copy"), stick});
            QVERIFY2(c.value(QStringLiteral("ok")).toBool(), qPrintable(c.value(QStringLiteral("error")).toString()));
        }
        QVERIFY(QFileInfo::exists(stick + QStringLiteral("/Tick/Tick.exe")));
        QVERIFY(QFileInfo(stick + QStringLiteral("/Tick/Tick.exe")).isWritable());
#ifndef Q_OS_WIN
        QVERIFY(QFileInfo(stick + QStringLiteral("/Tick/tick")).isExecutable());
#endif
        // without a bundle: a clear refusal
        qputenv("LIBRETAP_TICK_DIR", QFile::encodeName(ws.filePath(QStringLiteral("nothing"))));
        int code = 0;
        const QJsonObject none = run(ws.path(), {QStringLiteral("tick"), QStringLiteral("copy"), stick}, &code);
        QCOMPARE(code, 1);
        QVERIFY(none.value(QStringLiteral("error")).toString().contains(QStringLiteral("no Tick")));
        qunsetenv("LIBRETAP_TICK_DIR");
    }

    void controlHeadlessRecorder() {
        QTemporaryDir ws;
        QCOMPARE(run(ws.path(), {QStringLiteral("status")}).value(QStringLiteral("running")).toBool(), false);
        int code = 0;
        run(ws.path(), {QStringLiteral("mark"), QStringLiteral("x")}, &code);
        QVERIFY(code != 0);                                   // nobody to mark

        QProcess rec;
        rec.setProcessEnvironment(env(ws.path()));
        rec.start(QStringLiteral(CLI_PATH), {QStringLiteral("record"), QStringLiteral("--demo"), QStringLiteral("--camera"), QStringLiteral("demo"),
                                             QStringLiteral("--name"), QStringLiteral("cli test"), QStringLiteral("--json")});
        QVERIFY(rec.waitForStarted());
        // the first line says it started
        QTRY_VERIFY_WITH_TIMEOUT(rec.canReadLine(), 20000);
        const QJsonObject first = QJsonDocument::fromJson(rec.readLine()).object();
        QVERIFY(first.value(QStringLiteral("started")).toBool());
        const QString project = first.value(QStringLiteral("project")).toString();
        QVERIFY(project.endsWith(QStringLiteral("-cli_test")));
        QTest::qWait(1200);
        QJsonObject st = run(ws.path(), {QStringLiteral("status")});
        QVERIFY(st.value(QStringLiteral("recording")).toBool());
        QCOMPARE(st.value(QStringLiteral("project")).toString(), project);
        const QJsonObject m = run(ws.path(), {QStringLiteral("mark"), QStringLiteral("integration"), QStringLiteral("1 s")});
        QVERIFY(m.value(QStringLiteral("ok")).toBool());
        QCOMPARE(m.value(QStringLiteral("mark")).toInt(), 1);
        QCOMPARE(m.value(QStringLiteral("note")).toString(), QStringLiteral("integration 1 s"));
        QCOMPARE(run(ws.path(), {QStringLiteral("sync")}).value(QStringLiteral("sync")).toInt(), 1);
        QTest::qWait(1200);
        const QJsonObject stop = run(ws.path(), {QStringLiteral("stop")});
        QVERIFY(stop.value(QStringLiteral("ok")).toBool());
        QVERIFY(rec.waitForFinished(10000));
        QCOMPARE(rec.exitCode(), 0);
        // the live output: one event per line, then the summary
        const QList<QByteArray> lines = rec.readAllStandardOutput().trimmed().split('\n');
        QVERIFY(lines.size() > 5);
        const QJsonObject last = QJsonDocument::fromJson(lines.last()).object();
        QVERIFY(last.value(QStringLiteral("stopped")).toBool());
        QVERIFY(last.value(QStringLiteral("stats")).toObject().value(QStringLiteral("bytes_a")).toInt() > 0);
        QVERIFY(last.value(QStringLiteral("stats")).toObject().value(QStringLiteral("stills")).toInt() >= 1);

        const QJsonObject sessions = run(ws.path(), {QStringLiteral("sessions")});
        QCOMPARE(sessions.value(QStringLiteral("recordings")).toArray().size(), 1);
        const QJsonObject show = run(ws.path(), {QStringLiteral("show"), QStringLiteral("latest"), QStringLiteral("--frames")});
        QVERIFY(show.value(QStringLiteral("ok")).toBool());
        bool sawMark = false, sawData = false;
        for (const QJsonValue& v : show.value(QStringLiteral("rows")).toArray()) {
            const QJsonObject r = v.toObject();
            sawMark |= r.value(QStringLiteral("kind")).toString() == QLatin1String("mark") && r.value(QStringLiteral("text")).toString() == QLatin1String("integration 1 s");
            sawData |= r.value(QStringLiteral("who")).toString() == QStringLiteral("PC → Instrument") && r.value(QStringLiteral("text")).toString() == QStringLiteral("MEAS?\\r");
        }
        QVERIFY(sawMark);
        QVERIFY(sawData);
        QVERIFY(show.value(QStringLiteral("pictures")).toArray().first().toObject().value(QStringLiteral("count")).toInt() >= 1);
        QVERIFY(QFile::exists(show.value(QStringLiteral("pictures")).toArray().first().toObject().value(QStringLiteral("frames")).toArray().first().toObject().value(QStringLiteral("file")).toString()));
        // the text view as serialtap.py show
        QProcess txt;
        txt.setProcessEnvironment(env(ws.path()));
        txt.start(QStringLiteral(CLI_PATH), {QStringLiteral("show"), QStringLiteral("latest")});
        QVERIFY(txt.waitForFinished(10000));
        const QString out = QString::fromUtf8(txt.readAllStandardOutput());
        QVERIFY(out.contains(QStringLiteral("--- mark 1 at")));
        QVERIFY(out.contains(QStringLiteral("MEAS?\\r")));
    }

    void importAndMatch() {
        QTemporaryDir ws;
        synth::write(ws.filePath(QStringLiteral("rec/tap")), ws.filePath(QStringLiteral("usbstick/tick")), 42.25, 0, 300, false);
        // a recording folder around the tap session
        QFile pj(ws.filePath(QStringLiteral("rec/project.json")));
        QVERIFY(pj.open(QIODevice::WriteOnly));
        pj.write(R"({"format":"libretap-project","version":1,"sessions":[{"path":"tap","offset":0,"drift":0,"how":"reference"}]})");
        pj.close();
        const QJsonObject m = run(ws.path(), {QStringLiteral("match"), ws.filePath(QStringLiteral("rec/tap")), ws.filePath(QStringLiteral("usbstick/tick"))});
        QVERIFY(m.value(QStringLiteral("ok")).toBool());
        QVERIFY(std::abs(m.value(QStringLiteral("offset")).toDouble() - 42.25) < 0.006);
        const QJsonObject im = run(ws.path(), {QStringLiteral("import"), QStringLiteral("rec"), ws.filePath(QStringLiteral("usbstick/tick"))});
        QVERIFY(im.value(QStringLiteral("matched")).toBool());
        QVERIFY(QFile::exists(ws.filePath(QStringLiteral("rec/tick/session.json"))));
        const QJsonObject show = run(ws.path(), {QStringLiteral("show"), QStringLiteral("rec")});
        const QJsonArray ss = show.value(QStringLiteral("sessions")).toArray();
        QCOMPARE(ss.size(), 2);
        QCOMPARE(ss[1].toObject().value(QStringLiteral("how")).toString(), QStringLiteral("traffic"));
    }
};

QTEST_GUILESS_MAIN(TestCli)
#include "test_cli.moc"
