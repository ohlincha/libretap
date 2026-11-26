// Ethernet: reading pcap/pcapng (also while the file grows), packets in plain words, TCP reassembly (out of order,
// retransmission), UDP, the instrument-address filter, and a recording through a stand-in dumpcap. A real dumpcap
// on the loopback interface when one is installed and allowed to capture.
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "core/ethernet.h"
#include "core/pcap.h"
#include "core/recorder.h"
#include "core/timeline.h"
#include "pcapgen.h"

using namespace lt;

namespace {
QStringList conversation(const std::vector<NetFlows::Out>& out) {
    QStringList s;
    for (const auto& o : out)
        if (o.kind == QLatin1String("data"))
            s << o.fields.value(QStringLiteral("proto")).toString() + QLatin1Char(' ') + o.fields.value(QStringLiteral("dir")).toString() + QLatin1Char(':') +
                     QString::fromLatin1(QByteArray::fromHex(o.fields.value(QStringLiteral("hex")).toString().toLatin1()));
    return s;
}
QStringList packets(const std::vector<NetFlows::Out>& out) {
    QStringList s;
    for (const auto& o : out)
        if (o.kind == QLatin1String("packet")) s << o.fields.value(QStringLiteral("text")).toString();
    return s;
}
}  // namespace

class TestEthernet : public QObject {
    Q_OBJECT
private slots:
    void readFormats() {
        const auto frames = pcapgen::scenario();
        for (int k = 0; k < 3; ++k) {
            const QByteArray file = k == 0 ? pcapgen::pcapng(frames) : k == 1 ? pcapgen::pcapng(frames, true) : pcapgen::pcapClassic(frames);
            PcapReader r;
            r.feed(file);
            const auto p = r.take();
            QVERIFY(r.error.isEmpty());
            QCOMPARE(p.size(), frames.size());
            QVERIFY(std::abs(p[5].t - frames[5].t) < 2e-6);
            QCOMPARE(p[5].data, frames[5].data);
            QCOMPARE(p[5].linktype, 1);
        }
        PcapReader bad;
        bad.feed("hello world");
        QVERIFY(!bad.error.isEmpty());
    }

    void growingFile() {
        // dumpcap writes as it goes: any split of the bytes gives the same packets
        const auto frames = pcapgen::scenario();
        const QByteArray file = pcapgen::pcapng(frames);
        PcapReader r;
        size_t n = 0;
        for (qsizetype i = 0; i < file.size(); i += 37) {
            r.feed(file.mid(i, 37));
            n += r.take().size();
        }
        QCOMPARE(n, frames.size());
    }

    void plainWords() {
        const auto f = pcapgen::scenario();
        QCOMPARE(parsePacket({0, 1, f[0].data, 0}).summary, QStringLiteral("ARP: who has 192.168.0.20? tell 192.168.0.10"));
        QVERIFY(parsePacket({0, 1, f[1].data, 0}).summary.startsWith(QStringLiteral("ARP: 192.168.0.20 is at 02:00:00:00:00:20")));
        const NetPacket syn = parsePacket({0, 1, f[2].data, 0});
        QCOMPARE(syn.summary, QStringLiteral("TCP 192.168.0.10:50000 → 192.168.0.20:5025 [SYN]"));
        const NetPacket data = parsePacket({0, 1, f[5].data, 0});
        QCOMPARE(data.payload, QByteArray("*IDN?\n"));   // not the Ethernet padding
        QCOMPARE(parsePacket({0, 1, f[12].data, 0}).summary, QStringLiteral("UDP 192.168.0.10:40000 → 192.168.0.20:5000 4 bytes"));
    }

    void reassembly() {
        NetFlows flows;
        for (const auto& fr : pcapgen::scenario()) flows.add({fr.t, 1, fr.data, 0});
        flows.flush();
        const auto out = flows.take();
        QCOMPARE(conversation(out), (QStringList{QStringLiteral("tcp a:*IDN?\n"), QStringLiteral("tcp b:ACME,1000\n"), QStringLiteral("tcp a:MEAS?\n"),
                                                 QStringLiteral("tcp b:1.234\n"), QStringLiteral("udp a:PING"), QStringLiteral("udp b:PONG"),
                                                 QStringLiteral("udp a:OTHER")}));
        const QString p = packets(out).join(QLatin1Char('\n'));
        QVERIFY2(p.contains(QStringLiteral("TCP connection opened: 192.168.0.10:50000 → 192.168.0.20:5025")), qPrintable(p));
        QVERIFY(p.contains(QStringLiteral("TCP connection accepted")));
        QVERIFY(p.contains(QStringLiteral("TCP retransmission 192.168.0.20:5025 → 192.168.0.10:50000, 5 bytes already seen")));
        QVERIFY(p.contains(QStringLiteral("TCP connection closed by 192.168.0.10:50000 (FIN)")));
        QVERIFY(p.contains(QStringLiteral("ARP: who has")));
        for (const auto& o : out)
            if (o.kind == QLatin1String("data") && o.fields.value(QStringLiteral("proto")) == QLatin1String("tcp"))
                QCOMPARE(o.fields.value(QStringLiteral("conn")).toString(), QStringLiteral("192.168.0.10:50000 → 192.168.0.20:5025"));
        const QJsonObject st = flows.stats();
        QCOMPARE(st.value(QStringLiteral("tcp_retransmissions")).toInt(), 1);
        QCOMPARE(st.value(QStringLiteral("eth_bytes_a")).toInt(), 6 + 6 + 4 + 5);
    }

    void instrumentFilterAndMidStream() {
        // only the instrument's traffic; and a capture that starts in the middle of a connection (no SYN seen)
        auto frames = pcapgen::scenario();
        frames.erase(frames.begin(), frames.begin() + 5);
        NetFlows flows(QStringLiteral("192.168.0.20"));
        for (const auto& fr : frames) flows.add({fr.t, 1, fr.data, 0});
        flows.flush();
        const auto out = flows.take();
        const QStringList c = conversation(out);
        QVERIFY(!c.join(QLatin1Char(' ')).contains(QStringLiteral("OTHER")));
        QCOMPARE(c.first(), QStringLiteral("tcp a:*IDN?\n"));
        QCOMPARE(c[1], QStringLiteral("tcp b:ACME,1000\n"));
        QCOMPARE(flows.stats().value(QStringLiteral("ignored")).toInt(), 1);
    }

#ifdef Q_OS_UNIX
    void recordThroughStandInDumpcap() {
        // a stand-in for dumpcap: writes a ring-buffer file name as dumpcap does, half now and half later
        QTemporaryDir dir;
        const QString capture = dir.filePath(QStringLiteral("cap.pcapng"));
        QVERIFY(pcapgen::save(capture, pcapgen::pcapng(pcapgen::scenario(1796031300.0))));
        const QString script = dir.filePath(QStringLiteral("dumpcap"));
        QFile sf(script);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write(QStringLiteral(
                     "#!/bin/sh\n"
                     "out=; while [ $# -gt 0 ]; do [ \"$1\" = -w ] && out=$2; shift; done\n"
                     "f=\"${out%%.pcapng}_00001_20261130101500.pcapng\"\n"
                     "size=$(wc -c < '%1'); half=$((size / 2))\n"
                     "head -c $half '%1' > \"$f\"; sleep 0.6; tail -c +$((half + 1)) '%1' >> \"$f\"\n"
                     "trap 'exit 0' TERM; while :; do sleep 0.1; done\n")
                     .arg(capture)
                     .toLocal8Bit());
        sf.close();
        sf.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("LIBRETAP_DUMPCAP", script.toLocal8Bit());
        Recorder rec;
        RecordOptions o;
        o.workspace = dir.filePath(QStringLiteral("ws"));
        o.tap = false;
        o.ethernet = true;
        o.eth.iface = QStringLiteral("fake0");
        int live = 0;
        connect(&rec, &Recorder::recorded, this, [&](const QJsonObject& e) { live += e.value(QStringLiteral("kind")).toString() == QLatin1String("data"); });
        QString err;
        QVERIFY2(rec.start(o, &err), qPrintable(err));
        QTRY_VERIFY_WITH_TIMEOUT(live >= 7, 5000);            // shown while recording
        rec.stop();
        qunsetenv("LIBRETAP_DUMPCAP");
        Project p;
        QVERIFY(Project::load(rec.projectFolder(), &p, &err));
        const Session& s = p.sessions[0].session;
        QCOMPARE(s.sourceKind(QStringLiteral("eth")), QStringLiteral("ethernet"));
        QCOMPARE(s.count(QStringLiteral("data")), 7);
        QVERIFY(s.count(QStringLiteral("packet")) >= 6);
        Timeline tl;
        tl.build(p);
        QStringList who;
        for (const TItem& it : tl.rows)
            if (tl.event(p, it).kind == QLatin1String("data")) who << describe(p, it).who;
        QCOMPARE(who.first(), QStringLiteral("PC → Instrument (TCP 5025)"));
        QCOMPARE(who[1], QStringLiteral("Instrument → PC (TCP 5025)"));
        QVERIFY(who.contains(QStringLiteral("PC → Instrument (UDP 5000)")));
    }

    void refusalIsExplained() {
        QTemporaryDir dir;
        const QString script = dir.filePath(QStringLiteral("dumpcap"));
        QFile sf(script);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write("#!/bin/sh\necho \"dumpcap: You don't have permission to capture on that device\" >&2\nexit 1\n");
        sf.close();
        sf.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("LIBRETAP_DUMPCAP", script.toLocal8Bit());
        Recorder rec;
        RecordOptions o;
        o.workspace = dir.filePath(QStringLiteral("ws"));
        o.tap = false;
        o.ethernet = true;
        o.eth.iface = QStringLiteral("fake0");
        QString err;
        QVERIFY(!rec.start(o, &err));
        qunsetenv("LIBRETAP_DUMPCAP");
        QVERIFY2(err.contains(QStringLiteral("permission")), qPrintable(err));
    }
#endif

    void realDumpcapLoopback() {
        const CaptureAccess acc = captureAccess();
        if (acc.program.isEmpty()) QSKIP("dumpcap is not installed");
        if (!acc.ok) QSKIP(qPrintable(QStringLiteral("dumpcap may not capture here: ") + acc.sentence));
        QTemporaryDir dir;
        Recorder rec;
        RecordOptions o;
        o.workspace = dir.path();
        o.tap = false;
        o.ethernet = true;
        o.eth.iface = QStringLiteral("lo");
        QString err;
        if (!rec.start(o, &err)) QSKIP(qPrintable(QStringLiteral("dumpcap refused: ") + err));
        QProcess ping;
        ping.start(QStringLiteral("ping"), {QStringLiteral("-c"), QStringLiteral("2"), QStringLiteral("127.0.0.1")});
        ping.waitForFinished(5000);
        QTest::qWait(800);
        rec.stop();
        Project p;
        QVERIFY(Project::load(rec.projectFolder(), &p, &err));
        QVERIFY(p.sessions[0].session.count(QStringLiteral("packet")) >= 2);
    }
};

QTEST_GUILESS_MAIN(TestEthernet)
#include "test_ethernet.moc"
