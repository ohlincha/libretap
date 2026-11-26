// The serial tap with pseudo-terminals standing in for the two USB adapters (Linux openpty): what is written into
// the masters must come out as chunks in the session, with marks in between, and raw bytes in raw/.
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include "core/recorder.h"
#include "core/serialtap.h"
#include "core/session.h"

using namespace lt;

namespace {
struct Pty {
    int master = -1, slave = -1;
    QString path;
    bool open() {
        char name[256];
        if (openpty(&master, &slave, name, nullptr, nullptr) != 0) return false;
        termios t;
        tcgetattr(slave, &t);
        cfmakeraw(&t);
        tcsetattr(slave, TCSANOW, &t);
        path = QString::fromLocal8Bit(name);
        return true;
    }
    void write(const QByteArray& b) const { QVERIFY(::write(master, b.constData(), size_t(b.size())) == b.size()); }
    ~Pty() {
        if (master >= 0) ::close(master);
        if (slave >= 0) ::close(slave);
    }
};

void wait(int ms) { QTest::qWait(ms); }
}  // namespace

class TestSerialTap : public QObject {
    Q_OBJECT
private slots:
    void record() {
        Pty a, b;
        QVERIFY(a.open() && b.open());
        QTemporaryDir ws;
        Recorder rec;
        RecordOptions o;
        o.workspace = ws.path();
        o.tapConfig.portA = a.path;
        o.tapConfig.portB = b.path;
        o.tapConfig.nameB = QStringLiteral("CORA");
        QString err;
        QVERIFY2(rec.start(o, &err), qPrintable(err));
        wait(200);
        a.write("*IDN?\r");
        wait(100);
        b.write("CORA,");
        wait(5);
        b.write("5000\r\n");                                   // two pieces, one chunk
        wait(200);
        QCOMPARE(rec.mark(QStringLiteral("integration 1 s")), 1);
        a.write("SET 1\r");
        wait(8);
        b.write("OK\r\n");                                     // within the gap, but the other side: own chunk
        wait(200);
        QCOMPARE(rec.sync(), 1);
        const QString folder = rec.sessionFolder();
        const QJsonObject st = rec.status();
        QCOMPARE(st.value(QStringLiteral("stats")).toObject().value(QStringLiteral("bytes_a")).toInt(), 12);
        rec.stop();

        Session s;
        QVERIFY(Session::load(folder, &s, &err));
        QStringList seq;
        for (const Event& e : s.events) {
            if (e.kind == QLatin1String("data")) seq << e.str("dir") + QLatin1Char(':') + QString::fromLatin1(e.bytes());
            else if (e.kind == QLatin1String("mark")) seq << QStringLiteral("mark:") + e.str("note");
            else if (e.kind == QLatin1String("sync")) seq << QStringLiteral("sync");
        }
        QCOMPARE(seq, (QStringList{QStringLiteral("a:*IDN?\r"), QStringLiteral("b:CORA,5000\r\n"), QStringLiteral("mark:integration 1 s"),
                                   QStringLiteral("a:SET 1\r"), QStringLiteral("b:OK\r\n"), QStringLiteral("sync")}));
        const QJsonObject src = s.source(QStringLiteral("tap"));
        QCOMPARE(src.value(QStringLiteral("kind")).toString(), QStringLiteral("serial-tap"));
        QCOMPARE(src.value(QStringLiteral("names")).toObject().value(QStringLiteral("b")).toString(), QStringLiteral("CORA"));
        QCOMPARE(s.count(QStringLiteral("config")), 1);
        QFile ra(folder + QStringLiteral("/raw/a.bin")), rb(folder + QStringLiteral("/raw/b.bin"));
        QVERIFY(ra.open(QIODevice::ReadOnly) && rb.open(QIODevice::ReadOnly));
        QCOMPARE(ra.readAll(), QByteArray("*IDN?\rSET 1\r"));
        QCOMPARE(rb.readAll(), QByteArray("CORA,5000\r\nOK\r\n"));
        QVERIFY(QFile::exists(rec.projectFolder() + QStringLiteral("/project.json")));
    }

    void listenOnly() {
        // nothing may ever be written to the tap's ports: the master side must stay silent
        Pty a, b;
        QVERIFY(a.open() && b.open());
        QTemporaryDir ws;
        Recorder rec;
        RecordOptions o;
        o.workspace = ws.path();
        o.tapConfig.portA = a.path;
        o.tapConfig.portB = b.path;
        QVERIFY(rec.start(o, nullptr));
        a.write("ping\r");
        wait(150);
        rec.mark(QStringLiteral("x"));
        rec.stop();
        for (int fd : {a.master, b.master}) {
            char buf[64];
            fcntl(fd, F_SETFL, O_NONBLOCK);
            QCOMPARE(::read(fd, buf, sizeof buf), ssize_t(-1));   // EAGAIN: nothing arrived from our side
        }
    }

    void refusesBadSetup() {
        Recorder rec;
        RecordOptions o;
        QTemporaryDir ws;
        o.workspace = ws.path();
        o.tapConfig.portA = QStringLiteral("/dev/does-not-exist-a");
        o.tapConfig.portB = QStringLiteral("/dev/does-not-exist-b");
        QString err;
        QVERIFY(!rec.start(o, &err));
        QVERIFY(err.contains(QStringLiteral("cannot open")));
        QVERIFY(QDir(ws.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());   // no empty recording left
        o.tapConfig.portB = o.tapConfig.portA;
        QVERIFY(!rec.start(o, &err));
        QVERIFY(err.contains(QStringLiteral("same port")));
    }

    void guess() {
        Pty a;
        QVERIFY(a.open());
        QTimer talk;
        connect(&talk, &QTimer::timeout, this, [&] { a.write("MEAS?\r"); });
        talk.start(20);
        const BaudGuess g = guessBaud({a.path}, 0.1);
        QCOMPARE(g.rows.size(), commonBauds().size());
        QVERIFY(g.sure);                                     // a pty ignores the rate: text at every rate
        QVERIFY(g.baud > 0);
        QVERIFY(!g.sentence.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSerialTap)
#include "test_serialtap.moc"
