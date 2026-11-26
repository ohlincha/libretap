// End to end, the way it is used in the lab: Tick (for Linux) runs the "vendor program" (Tick's fakevendor_linux)
// on a serial port; the port is a pseudo-terminal whose other end is a relay: it answers as the instrument
// ("TICKTEST,<line>\r\n") and copies each direction to its own pseudo-terminal, where LibreTap's serial tap listens
// (as the two leads of the DB-9 tap would). Then the two sessions are combined: every byte must line up, and the
// clock offset must come out near zero (both ran on this computer's clock).
// Needs ../tick/build/linux/{tick,fakevendor_linux} (built by the Tick project); skipped without them.
#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include "core/match.h"
#include "core/recorder.h"
#include "core/timeline.h"

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
    ~Pty() {
        if (master >= 0) ::close(master);
        if (slave >= 0) ::close(slave);
    }
};

void writeAll(int fd, const QByteArray& b) {
    for (qsizetype i = 0; i < b.size();) {
        const ssize_t n = ::write(fd, b.constData() + i, size_t(b.size() - i));
        if (n > 0) i += n;
    }
}
}  // namespace

class TestEndToEnd : public QObject {
    Q_OBJECT
private slots:
    void tickAndTap() {
        const QString tickDir = QStringLiteral(TEST_DATA "/../../../tick/build/linux");
        const QString tick = QDir(tickDir).filePath(QStringLiteral("tick")), vendor = QDir(tickDir).filePath(QStringLiteral("fakevendor_linux"));
        if (!QFileInfo(tick).isExecutable() || !QFileInfo(vendor).isExecutable()) QSKIP("Tick for Linux is not built (../tick: make linux tools-test)");
        Pty v, a, b;
        QVERIFY(v.open() && a.open() && b.open());
        // the relay and the instrument
        std::atomic<bool> stop{false};
        std::thread relay([&] {
            QByteArray line;
            while (!stop) {
                pollfd p{v.master, POLLIN, 0};
                if (poll(&p, 1, 20) <= 0) continue;
                char buf[256];
                const ssize_t n = ::read(v.master, buf, sizeof buf);
                if (n <= 0) continue;
                const QByteArray got(buf, int(n));
                writeAll(a.master, got);                      // the PC's TX, as the tap's lead A sees it
                line += got;
                int cr;
                while ((cr = line.indexOf('\r')) >= 0) {
                    const QByteArray answer = "TICKTEST," + line.left(cr) + "\r\n";
                    line.remove(0, cr + 1);
                    usleep(8000);                             // the instrument thinks
                    writeAll(v.master, answer);
                    writeAll(b.master, answer);               // the instrument's TX, lead B
                }
            }
        });
        QTemporaryDir dir;
        Recorder rec;
        RecordOptions o;
        o.workspace = dir.filePath(QStringLiteral("ws"));
        o.tapConfig.portA = a.path;
        o.tapConfig.portB = b.path;
        QString err;
        QVERIFY2(rec.start(o, &err), qPrintable(err));
        QProcess t;
        t.setProcessChannelMode(QProcess::MergedChannels);
        t.start(tick, {QStringLiteral("run"), QStringLiteral("--no-tui"), QStringLiteral("--no-screen"), QStringLiteral("--until-exit"),
                       QStringLiteral("--dir"), dir.filePath(QStringLiteral("tick")), QStringLiteral("--control"), dir.filePath(QStringLiteral("tick.sock")),
                       QStringLiteral("--"), vendor, v.path, QStringLiteral("6"), QStringLiteral("150")});
        QVERIFY(t.waitForStarted(5000));
        for (int i = 0; i < 300 && t.state() != QProcess::NotRunning; ++i) QTest::qWait(100);   // the tap reads meanwhile
        const bool done = t.state() == QProcess::NotRunning;
        QTest::qWait(300);
        rec.stop();
        stop = true;
        relay.join();
        QVERIFY2(done, "Tick did not finish");
        const QString tickOut = QString::fromLocal8Bit(t.readAll());
        const QStringList sessions = QDir(dir.filePath(QStringLiteral("tick"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY2(sessions.size() == 1, qPrintable(tickOut));
        // combine
        Project p;
        QVERIFY(Project::load(rec.projectFolder(), &p, &err));
        int idx = -1;
        QVERIFY2(p.addSession(dir.filePath(QStringLiteral("tick/")) + sessions[0], &err, &idx), qPrintable(err));
        const MatchResult m = matchInProject(p, 0, idx);
        if (!m.ok) {
            dir.setAutoRemove(false);
            qWarning().noquote() << "kept" << dir.path() << QJsonDocument(m.toJson()).toJson(QJsonDocument::Compact);
        }
        QVERIFY2(m.ok, qPrintable(m.message));
        QVERIFY2(std::abs(p.sessions[1].offset) < 0.05, qPrintable(QString::number(p.sessions[1].offset, 'f', 4)));
        // every byte lines up: what the program wrote is what lead A carried, what it read is what lead B carried
        QByteArray wrote, read, leadA, leadB;
        for (const Event& e : p.sessions[1].session.events)
            if (e.kind == QLatin1String("data")) (e.str("dir") == QLatin1String("out") ? wrote : read) += e.bytes();
        for (const Event& e : p.sessions[0].session.events)
            if (e.kind == QLatin1String("data")) (e.str("dir") == QLatin1String("a") ? leadA : leadB) += e.bytes();
        QCOMPARE(wrote, QByteArray("*IDN? 0\r*IDN? 1\r*IDN? 2\r*IDN? 3\r*IDN? 4\r*IDN? 5\r"));
        QCOMPARE(leadA, wrote);
        QCOMPARE(leadB, read);
        QVERIFY(read.startsWith("TICKTEST,*IDN? 0\r\n"));
        // and in time: each write within 50 ms of the tap chunk that carried it, on the project clock
        Timeline tl;
        tl.build(p);
        std::vector<double> tw, ta;
        for (const TItem& it : tl.rows) {
            const Event& e = tl.event(p, it);
            if (e.kind != QLatin1String("data")) continue;
            if (e.str("dir") == QLatin1String("out")) tw.push_back(it.t);
            if (e.str("dir") == QLatin1String("a")) ta.push_back(it.t);
        }
        QCOMPARE(ta.size(), size_t(6));   // one chunk per question (writev's two pieces arrive together)
        QVERIFY(tw.size() >= 6);
        for (double x : ta) {
            double best = 1e9;
            for (double y : tw) best = std::min(best, std::abs(x - y));
            QVERIFY2(best < 0.05, qPrintable(QString::number(best)));
        }
    }
};

QTEST_GUILESS_MAIN(TestEndToEnd)
#include "test_endtoend.moc"
