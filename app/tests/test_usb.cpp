// The Pico USB sniffer: decoding synthetic usb-sniffer-lite dumps, and a whole recording against a fake Pico on a
// pseudo-terminal (configured over its serial port, capture started, dump fetched at Stop, events in the session).
// The same for LibreTap's streaming firmware (USB Sniffer Stream): its records, decoded into the same events, and a
// continuous recording against a fake streaming Pico.
#include <QTemporaryDir>
#include <QtTest>

#include <QElapsedTimer>

#include <atomic>
#include <cmath>
#include <thread>

#include "core/recorder.h"
#include "core/timeline.h"
#include "core/usbpico.h"
#include "core/util.h"
#include "usbdump.h"
#include "usbstream.h"

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

    void streamReader() {
        // records cut at every possible place, with junk, a damaged record and a header that only looks like one
        usbstream::Ftdi f(1000);
        f.enumeration();
        f.question();
        QByteArray all = usbstream::hello(900);
        for (const auto& r : f.out) all += r.second;
        const int good = int(f.out.size()) + 1;
        QByteArray noisy = QByteArray("\xa5\x02\xff\x7fjunk\xa5", 9) + all;
        noisy[9 + 46 + 5] = char(noisy[9 + 46 + 5] ^ 0x5a);   // inside the second record (HELLO is 46 bytes)
        for (int piece : {1, 7, 64, 100000}) {
            PicoStreamReader rd;
            std::vector<PicoRecord> got;
            for (int i = 0; i < noisy.size(); i += piece)
                for (PicoRecord& r : rd.feed(noisy.mid(i, piece))) got.push_back(std::move(r));
            QCOMPARE(int(got.size()), good - 1);
            QCOMPARE(got[0].type, int(PicoRecord::Hello));
            QVERIFY(rd.skipped > 9);
        }
        PicoStreamReader rd;
        const auto clean = rd.feed(all);
        QCOMPARE(int(clean.size()), good);
        QCOMPARE(rd.skipped, qint64(0));
        QCOMPARE(clean[1].type, int(PicoRecord::Reset));
        QCOMPARE(clean[2].time, quint32(1000 + 1000));
        QCOMPARE(picoCrc16("123456789", 9), quint16(0x29b1));   // CRC-16/CCITT-FALSE check value
    }

    void streamDecode() {
        // the streamed packets give the same transfers as the text dump of the same traffic
        usbstream::Ftdi f(4294967000u);   // the Pico's timer wraps during the enumeration
        f.enumeration();
        f.question();
        f.wait(20000);
        f.answer();
        PicoStreamReader rd;
        UsbDecoder d;
        std::vector<UsbEvent> ev;
        QByteArray all;
        for (const auto& r : f.out) all += r.second;
        double us = 0;
        quint32 last = 4294967000u;
        std::vector<double> firstData;
        for (const PicoRecord& r : rd.feed(all)) {
            us += double(qint32(r.time - last));
            last = r.time;
            if (r.type == PicoRecord::Packet) d.packet(us / 1000, uchar(r.payload[0]), r.payload.mid(1));
            if (r.type == PicoRecord::Reset) d.reset(us / 1000);
            for (UsbEvent& e : d.take(us / 1000)) ev.push_back(std::move(e));   // as while recording: piece by piece
        }
        for (UsbEvent& e : d.take(0, true)) ev.push_back(std::move(e));
        QStringList what;
        for (const UsbEvent& e : ev) what << e.what;
        const QString all2 = what.join(QLatin1Char('\n'));
        QVERIFY2(all2.contains(QStringLiteral("GET_DESCRIPTOR device #0: USB 2.00, VID 0403 PID 6001")), qPrintable(all2));
        QVERIFY(all2.contains(QStringLiteral("SET_ADDRESS 5")));
        QVERIFY(all2.contains(QStringLiteral("FTDI SET_BAUD_RATE")) && all2.contains(QStringLiteral("about 9600 baud")));
        QCOMPARE(ev.front().what, QStringLiteral("bus reset"));
        std::vector<const UsbEvent*> data;
        for (const UsbEvent& e : ev)
            if (!e.control && e.ep > 0) data.push_back(&e);
        QCOMPARE(data.size(), size_t(2));
        QCOMPARE(data[0]->data, QByteArray("MEAS?\r"));
        QCOMPARE(data[1]->data, QByteArray("M 0.5123 V\r\n"));
        QVERIFY(data[1]->tMs - data[0]->tMs > 20);   // the wrap did not throw the times off
        QVERIFY(data[0]->tMs > 0 && data[0]->tMs < 1000);
        QCOMPARE(d.devices.at(5).vid, 0x0403);
        // a damaged packet is a capture error, as in the dump
        UsbDecoder e;
        e.packet(1.0, 0x02, QByteArray::fromHex("c3010203"));
        QCOMPARE(e.errors, 1);
        QVERIFY(e.take(2.0, true).front().what.startsWith(QStringLiteral("capture error: ERROR [CRC]")));
    }

    void firmwareSample() {
        // written by the firmware's own filter and encoder (firmware/pico-usb-sniffer/tests: test_stream --sample):
        // an FTDI instrument on a bus full of SOFs and NAKed polling, the Pico's timer wrapping half way
        QFile f(QStringLiteral(TEST_DATA "/usb_stream_ftdi.bin"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        PicoStreamReader rd;
        const auto recs = rd.feed(bytes);
        QCOMPARE(rd.skipped, qint64(0));
        QCOMPARE(recs.front().type, int(PicoRecord::Hello));
        QVERIFY(recs.front().payload.endsWith("USB Sniffer Stream 1.0.0"));
        QCOMPARE(recs.back().type, int(PicoRecord::End));
        UsbDecoder d;
        d.feedStream(bytes);
        QStringList what;
        for (const UsbEvent& e : d.events) what << e.what;
        const QString all = what.join(QLatin1Char('\n'));
        QVERIFY2(all.contains(QStringLiteral("GET_DESCRIPTOR device #0: USB 2.00, VID 0403 PID 6001")), qPrintable(all));
        QVERIFY(all.contains(QStringLiteral("SET_ADDRESS 5")) && all.contains(QStringLiteral("about 9600 baud")));
        QVERIFY(all.contains(QStringLiteral("bus reset")));
        QVERIFY(!all.contains(QStringLiteral("capture error")));
        std::vector<const UsbEvent*> data;
        for (const UsbEvent& e : d.events)
            if (!e.control && e.ep > 0) data.push_back(&e);
        QCOMPARE(data.size(), size_t(2));   // FTDI's status-only packets carry nothing
        QCOMPARE(data[0]->data, QByteArray("MEAS?\r"));
        QCOMPARE(data[1]->data, QByteArray("M 0.5123 V\r\n"));
        QVERIFY2(std::abs(data[1]->tMs - data[0]->tMs - 150) < 2, qPrintable(QString::number(data[1]->tMs - data[0]->tMs)));
        QVERIFY(d.events.front().tMs >= 0 && data[0]->tMs > 100 && data[1]->tMs < 1000);   // from HELLO, across the wrap
        QCOMPARE(d.lost, qint64(0));
    }

#ifdef __linux__
    void fakeStreamPicoRecording() {
        int master = -1, slave = -1;
        char name[256];
        QVERIFY(openpty(&master, &slave, name, nullptr, nullptr) == 0);
        termios t;
        tcgetattr(slave, &t);
        cfmakeraw(&t);
        tcsetattr(slave, TCSANOW, &t);
        // USB Sniffer Stream as far as LibreTap uses it: h, v, e/k/o settings, s (records until p), p (END)
        struct Setting {
            char key;
            const char* label;
            QStringList values;
            int cur;
        };
        std::vector<Setting> st{{'e', "Capture speed", {QStringLiteral("Low"), QStringLiteral("Full")}, 1},
                                {'k', "Keep idle traffic", {QStringLiteral("Disabled"), QStringLiteral("Enabled")}, 1},
                                {'o', "Output format", {QStringLiteral("Binary"), QStringLiteral("Text")}, 1}};
        std::atomic<bool> stop{false};
        std::atomic<int> starts{0};
        std::atomic<qint64> sent{0};
        std::atomic<double> questionAt{0}, answerAt{0};
        const quint32 picoBase = 4294967296.0 - 300000;   // the Pico's timer wraps 0.3 s into the recording
        std::thread fw([&] {
            bool streaming = false;
            QElapsedTimer since;
            int phase = 0;
            qint64 lastStatus = 0;
            auto picoNow = [&] { return quint32(picoBase + quint32(since.nsecsElapsed() / 1000)); };
            auto out = [&](const QByteArray& b, bool count) {
                for (qsizetype i = 0; i < b.size();) {   // in odd pieces: records cut anywhere
                    const ssize_t n = ::write(master, b.constData() + i, size_t(std::min<qsizetype>(37, b.size() - i)));
                    if (n > 0) i += n;
                }
                if (count) sent += b.size();
            };
            auto send = [&](const std::vector<std::pair<quint32, QByteArray>>& recs) {
                QByteArray b;
                for (const auto& r : recs) b += r.second;
                out(b, true);
            };
            while (!stop) {
                pollfd p{master, POLLIN, 0};
                if (poll(&p, 1, 10) > 0) {
                    char c;
                    if (::read(master, &c, 1) == 1) {
                        QByteArray o;
                        if (c == 'h') {
                            o = "Settings:\r\n";
                            for (const Setting& s : st)
                                o += QStringLiteral("  %1 - %2: %3\r\n").arg(QLatin1Char(s.key)).arg(QLatin1String(s.label), -20).arg(s.values[s.cur]).toLatin1();
                        } else if (c == 'v') {
                            o = "USB Sniffer Stream 1.0.0 protocol 1\r\n";
                        } else if (c == 's' && !streaming) {
                            ++starts;
                            streaming = true;
                            since.start();
                            out(usbstream::hello(picoNow()), true);
                        } else if (c == 'p' && streaming) {
                            streaming = false;
                            out(usbstream::status(picoNow(), 8, 1000, 99000, 60, 7), true);
                        } else {
                            for (Setting& s : st)
                                if (s.key == c) {
                                    s.cur = (s.cur + 1) % int(s.values.size());
                                    o = QStringLiteral("%1 changed to %2\r\n").arg(QLatin1String(s.label), s.values[s.cur]).toLatin1();
                                }
                        }
                        if (!o.isEmpty()) out(o, false);
                    }
                }
                if (!streaming) continue;
                const qint64 ms = since.elapsed();
                if (phase == 0 && ms > 50) {
                    usbstream::Ftdi f(picoNow());
                    f.enumeration();
                    send(f.out), ++phase;
                } else if (phase == 1 && ms > 200) {
                    usbstream::Ftdi f(picoNow());
                    f.question();
                    questionAt = wallNow();
                    send(f.out), ++phase;
                } else if (phase == 2 && ms > 700) {
                    usbstream::Ftdi f(picoNow());
                    f.answer();
                    answerAt = wallNow();
                    send(f.out), ++phase;
                } else if (phase == 3 && ms > 750) {
                    out(usbstream::record(5, picoNow(), usbstream::le32(7) + usbstream::le32(0)), true);   // LOST
                    ++phase;
                }
                if (ms - lastStatus >= 250) {   // STATUS, as the Pico's clock reference
                    lastStatus = ms;
                    out(usbstream::status(picoNow(), 6, quint32(ms), quint32(ms * 99), 60), true);
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
        double markAt = 0;
        if (ok) {
            QTest::qWait(450);
            markAt = wallNow();
            rec.mark(QStringLiteral("press Measure"));
            QTest::qWait(600);
            QCOMPARE(rec.status().value(QStringLiteral("stats")).toObject().value(QStringLiteral("usb_mode")).toString(), QStringLiteral("stream"));
            rec.stop();
        }
        stop = true;
        fw.join();
        ::close(master);
        ::close(slave);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(starts.load(), 1);
        QCOMPARE(st[0].values[st[0].cur], QStringLiteral("Full"));
        QCOMPARE(st[1].values[st[1].cur], QStringLiteral("Disabled"));
        QCOMPARE(st[2].values[st[2].cur], QStringLiteral("Binary"));
        Project p;
        QVERIFY(Project::load(rec.projectFolder(), &p, &err));
        const Session& s = p.sessions[0].session;
        QCOMPARE(s.sourceKind(QStringLiteral("usb")), QStringLiteral("usb-pico"));
        QCOMPARE(s.source(QStringLiteral("usb")).value(QStringLiteral("mode")).toString(), QStringLiteral("stream"));
        QCOMPARE(s.source(QStringLiteral("usb")).value(QStringLiteral("firmware")).toString(), QStringLiteral("USB Sniffer Stream 1.0.0"));
        QFile raw(s.folder + QStringLiteral("/raw/usb_stream.bin"));
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QCOMPARE(raw.size(), sent.load());   // every byte the Pico sent after s
        QCOMPARE(s.count(QStringLiteral("data")), 2);
        QVERIFY(s.count(QStringLiteral("usb")) >= 9);
        QCOMPARE(s.count(QStringLiteral("mark")), 1);
        const Event *q = nullptr, *a = nullptr, *lost = nullptr, *mark = nullptr;
        QString summary;
        for (const Event& e : s.events) {
            if (e.kind == QLatin1String("data")) (q ? a : q) = &e;
            if (e.kind == QLatin1String("usb") && e.str("what").startsWith(QLatin1String("lost 7 packets"))) lost = &e;
            if (e.kind == QLatin1String("mark")) mark = &e;
            if (e.kind == QLatin1String("status") && e.str("text").startsWith(QLatin1String("USB stream:"))) summary = e.str("text");
        }
        QVERIFY(q && a && lost && mark);
        QCOMPARE(q->bytes(), QByteArray("MEAS?\r"));
        // recorded continuously: the question before the mark, the answer after it, at the times they happened
        QVERIFY2(q->t < mark->t && mark->t < a->t, qPrintable(QStringLiteral("%1 %2 %3").arg(q->t, 0, 'f', 3).arg(mark->t, 0, 'f', 3).arg(a->t, 0, 'f', 3)));
        QVERIFY2(std::abs(q->t - questionAt.load()) < 0.05, qPrintable(QString::number(q->t - questionAt.load())));
        QVERIFY2(std::abs(a->t - answerAt.load()) < 0.05, qPrintable(QString::number(a->t - answerAt.load())));
        QVERIFY(std::abs(mark->t - markAt) < 0.05);
        QVERIFY(lost->t > a->t);
        QVERIFY2(summary.contains(QStringLiteral("1000 frames")) && summary.contains(QStringLiteral("7 lost")), qPrintable(summary));
        QVERIFY(s.count(QStringLiteral("error")) >= 1);   // the loss is also a problem the window shows
        Timeline tl;
        tl.build(p);
        QStringList who;
        for (const TItem& it : tl.rows)
            if (tl.event(p, it).kind == QLatin1String("data")) who << describe(p, it).who + QLatin1Char(' ') + describe(p, it).text;
        QCOMPARE(who, (QStringList{QStringLiteral("PC → Instrument (USB ep 2) MEAS?\\r"), QStringLiteral("Instrument → PC (USB ep 1) M 0.5123 V\\r\\n")}));
    }

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
