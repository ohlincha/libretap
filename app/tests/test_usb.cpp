// The Pico USB sniffer: decoding synthetic usb-sniffer-lite dumps, and a whole recording against a fake Pico on a
// pseudo-terminal (configured over its serial port, capture started, dump fetched at Stop, events in the session).
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>

#include "core/recorder.h"
#include "core/timeline.h"
#include "core/usbpico.h"
#include "usbdump.h"

#ifdef __linux__
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace lt;

class TestUsb : public QObject {
    Q_OBJECT
private slots:
    void decodeFtdi() {
        UsbDecoder d;
        d.feed(usbdump::ftdiInstrument());
        QStringList what;
        for (const UsbEvent& e : d.events) what << e.what;
        const QString all = what.join(QLatin1Char('\n'));
        QVERIFY2(all.contains(QStringLiteral("GET_DESCRIPTOR device #0: USB 2.00, VID 0403 PID 6001")), qPrintable(all));
        QVERIFY(all.contains(QStringLiteral("FTDI USB-serial bridge")));
        QVERIFY(all.contains(QStringLiteral("SET_ADDRESS 5")));
        QVERIFY(all.contains(QStringLiteral("ep 1 IN bulk 64 bytes; ep 2 OUT bulk 64 bytes")));
        QVERIFY(all.contains(QStringLiteral("FTDI SET_BAUD_RATE")) && all.contains(QStringLiteral("about 9600 baud")));
        QVERIFY(all.contains(QStringLiteral("FTDI SET_DATA")));
        // the payload: the question once (the repeat was a retransmission), the answer without FTDI's status bytes
        std::vector<const UsbEvent*> data;
        for (const UsbEvent& e : d.events)
            if (!e.control && e.ep > 0) data.push_back(&e);
        QCOMPARE(data.size(), size_t(2));
        QCOMPARE(data[0]->dir, QStringLiteral("OUT"));
        QCOMPARE(data[0]->ep, 2);
        QCOMPARE(data[0]->data, QByteArray("MEAS?\r"));
        QCOMPARE(data[1]->dir, QStringLiteral("IN"));
        QCOMPARE(data[1]->data, QByteArray("M 0.5123 V\r\n"));
        QVERIFY(data[1]->what.startsWith(QStringLiteral("bulk")));
        QCOMPARE(d.devices.at(5).vid, 0x0403);
        QVERIFY(d.summary().join(QLatin1Char('\n')).contains(QStringLiteral("device 5: VID 0403 PID 6001 (FTDI USB-serial)")));
    }

    void decodeCdc() {
        UsbDecoder d;
        d.feed(usbdump::cdcLineCoding());
        QCOMPARE(d.events.size(), size_t(2));
        QVERIFY2(d.events[0].what.contains(QStringLiteral("SET_LINE_CODING (CDC)")) &&
                     d.events[0].what.contains(QStringLiteral("9600 baud, 8 data bits, parity N, stop 1")), qPrintable(d.events[0].what));
        QVERIFY(d.events[1].what.contains(QStringLiteral("DTR 1, RTS 1")));
        QVERIFY(d.summary().join(QLatin1Char(' ')).contains(QStringLiteral("no enumeration captured")));
    }

    void examplesFromScratch() {
        // optional: usb-sniffer-lite's own example logs (not part of this repository)
        const QString dir = qEnvironmentVariable("LIBRETAP_USL_EXAMPLES");
        if (dir.isEmpty()) QSKIP("set LIBRETAP_USL_EXAMPLES to a folder with usb_fs_enumeration.txt");
        QFile f(dir + QStringLiteral("/usb_fs_enumeration.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        UsbDecoder d;
        d.feed(QString::fromLatin1(f.readAll()));
        QCOMPARE(d.devices.at(64).vid, 0x6666);
        QCOMPARE(d.devices.at(64).eps.size(), size_t(2));
    }

#ifdef __linux__
    void fakePicoRecording() {
        int master = -1, slave = -1;
        char name[256];
        QVERIFY(openpty(&master, &slave, name, nullptr, nullptr) == 0);
        termios t;
        tcgetattr(slave, &t);
        cfmakeraw(&t);
        tcsetattr(slave, TCSANOW, &t);
        // the sniffer's firmware, as far as LibreTap uses it: h (settings), setting keys, s (start), p (stop + dump)
        struct Setting {
            char key;
            const char* label;
            QStringList values;
            int cur;
        };
        std::vector<Setting> st{{'e', "Capture speed", {QStringLiteral("Low"), QStringLiteral("Full")}, 1},
                                {'g', "Capture trigger", {QStringLiteral("Enabled"), QStringLiteral("Disabled")}, 0},
                                {'l', "Capture limit", {QStringLiteral("100 packets"), QStringLiteral("Unlimited")}, 0},
                                {'t', "Time display format", {QStringLiteral("Relative to the first packet"), QStringLiteral("Relative to the SOF")}, 0},
                                {'a', "Data display format", {QStringLiteral("Full"), QStringLiteral("Limit to 16 bytes")}, 1},
                                {'f', "Fold empty frames", {QStringLiteral("Enabled"), QStringLiteral("Disabled")}, 0}};
        std::atomic<bool> stop{false};
        std::atomic<int> starts{0};
        const QByteArray dump = usbdump::ftdiInstrument().replace(QStringLiteral("\n"), QStringLiteral("\r\n")).toLatin1();
        std::thread fw([&] {
            while (!stop) {
                pollfd p{master, POLLIN, 0};
                if (poll(&p, 1, 50) <= 0) continue;
                char c;
                if (::read(master, &c, 1) != 1) continue;
                QByteArray out;
                if (c == 'h') {
                    out = "Settings:\r\n";
                    for (const Setting& s : st) out += QStringLiteral("  %1 - %2: %3\r\n").arg(QLatin1Char(s.key)).arg(QLatin1String(s.label), -20).arg(s.values[s.cur]).toLatin1();
                } else if (c == 's') {
                    ++starts;
                    out = "Capture started\r\n";
                } else if (c == 'p') {
                    out = "Capture stopped\r\n\r\nCapture buffer:\r\n" + dump;
                } else {
                    for (Setting& s : st)
                        if (s.key == c) {
                            s.cur = (s.cur + 1) % int(s.values.size());
                            out = QStringLiteral("%1 changed to %2\r\n").arg(QLatin1String(s.label), s.values[s.cur]).toLatin1();
                        }
                }
                for (qsizetype i = 0; i < out.size();) {   // in pieces, as a USB serial port delivers it
                    const ssize_t n = ::write(master, out.constData() + i, size_t(std::min<qsizetype>(512, out.size() - i)));
                    if (n > 0) i += n;
                }
            }
        });
        QTemporaryDir ws;
        Recorder rec;
        RecordOptions o;
        o.workspace = ws.path();
        o.tap = false;
        o.usb = true;
        o.pico.port = QString::fromLocal8Bit(name);
        QString err;
        const bool ok = rec.start(o, &err);
        if (ok) {
            QTest::qWait(300);
            rec.mark(QStringLiteral("press Measure"));
            QTest::qWait(300);
            rec.stop();
        }
        stop = true;
        fw.join();
        ::close(master);
        ::close(slave);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(starts.load(), 1);
        // the settings the sniffer was switched to
        for (const Setting& s : st)
            if (s.key != 'e') QCOMPARE(s.values[s.cur], QStringList({QStringLiteral("Disabled"), QStringLiteral("Unlimited"), QStringLiteral("Relative to the SOF"),
                                                                     QStringLiteral("Full"), QStringLiteral("Enabled")})[int(&s - &st[1])]);
        Project p;
        QVERIFY(Project::load(rec.projectFolder(), &p, &err));
        const Session& s = p.sessions[0].session;
        QCOMPARE(s.sourceKind(QStringLiteral("usb")), QStringLiteral("usb-pico"));
        QVERIFY(QFile::exists(s.folder + QStringLiteral("/raw/usb_cap.txt")));
        QCOMPARE(s.count(QStringLiteral("data")), 2);
        QVERIFY(s.count(QStringLiteral("usb")) >= 8);
        QCOMPARE(s.count(QStringLiteral("mark")), 1);
        Timeline tl;
        tl.build(p);
        QStringList who;
        for (const TItem& it : tl.rows)
            if (tl.event(p, it).kind == QLatin1String("data")) who << describe(p, it).who + QLatin1Char(' ') + describe(p, it).text;
        QCOMPARE(who, (QStringList{QStringLiteral("PC → Instrument (USB ep 2) MEAS?\\r"), QStringLiteral("Instrument → PC (USB ep 1) M 0.5123 V\\r\\n")}));
    }
#endif
};

QTEST_GUILESS_MAIN(TestUsb)
#include "test_usb.moc"
