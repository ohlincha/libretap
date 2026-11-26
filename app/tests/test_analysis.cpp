// Analysis: exchanges, diff between marks, hints (checksum and length field found in a synthetic binary protocol),
// checksum algorithms against their published check values, protocol descriptions and coverage, key frames, the AI
// export, and USBPcap import with a device filter (the keyboard must never come through).
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "core/analysis.h"
#include "core/protocol.h"
#include "core/usbpcap.h"
#include "pcapgen.h"

using namespace lt;

namespace {
// A binary frame: 02, length, command, payload, xor of bytes 1..n-2
QByteArray frame(char cmd, const QByteArray& payload) {
    QByteArray f;
    f += char(0x02);
    f += char(4 + payload.size());
    f += cmd;
    f += payload;
    char x = 0;
    for (int i = 1; i < f.size(); ++i) x ^= f[i];
    return f + x;
}

// a recording: polling, settings changed between marks, camera pictures
QString makeRecording(const QString& dir) {
    Project p;
    Project::create(dir, &p, nullptr);
    SessionWriter w;
    w.create(dir + QStringLiteral("/s"), {}, nullptr);
    w.setSources({QJsonObject{{QStringLiteral("id"), QStringLiteral("tap")}, {QStringLiteral("kind"), QStringLiteral("serial-tap")}},
                  QJsonObject{{QStringLiteral("id"), QStringLiteral("camera")}, {QStringLiteral("kind"), QStringLiteral("camera")}}});
    double t = 1000;
    int mark = 0, pic = 0;
    for (int step = 0; step < 4; ++step) {
        w.event(t, QStringLiteral("mark"), {{QStringLiteral("n"), ++mark}, {QStringLiteral("note"), QStringLiteral("setting %1").arg(step)}});
        t += 0.1;
        const QByteArray set = frame('S', QByteArray(1, char(10 * (step + 1))) + QByteArray(step % 2 ? 3 : 1, char(0x55)));
        w.event(t, QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), QStringLiteral("a")}, {QStringLiteral("hex"), QString::fromLatin1(set.toHex())}});
        w.event(t + 0.01, QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), QStringLiteral("b")}, {QStringLiteral("hex"), QString::fromLatin1(frame('A', {}).toHex())}});
        for (int k = 0; k < 5; ++k) {
            t += 0.5;
            w.event(t, QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), QStringLiteral("a")}, {QStringLiteral("hex"), QString::fromLatin1(QByteArray("STAT?\r").toHex())}});
            const QByteArray ans = "OK " + QByteArray::number(10 * (step + 1)) + "\r\n";
            w.event(t + 0.02, QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), QStringLiteral("b")}, {QStringLiteral("hex"), QString::fromLatin1(ans.toHex())}});
        }
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(QColor::fromHsv(step * 60, 200, 200));
        const QString file = QStringLiteral("frames/cam_%1.jpg").arg(++pic, 6, 10, QLatin1Char('0'));
        img.save(dir + QStringLiteral("/s/") + file);
        w.event(t - 2.2, QStringLiteral("frame"), {{QStringLiteral("src"), QStringLiteral("camera")}, {QStringLiteral("file"), file}, {QStringLiteral("full"), true}});
        t += 1;
    }
    w.finish();
    p.addSession(dir + QStringLiteral("/s"), nullptr);
    p.save(nullptr);
    return dir;
}
}  // namespace

class TestAnalysis : public QObject {
    Q_OBJECT
private slots:
    void checkValues() {
        // the standard check input "123456789" and the catalogue's check values
        const QByteArray d("123456789");
        std::map<QString, quint32> v;
        for (const auto& [n, x] : checksums(d, 0, d.size())) v[n] = x;
        QCOMPARE(v[QStringLiteral("CRC-16/MODBUS")], 0x4B37u);
        QCOMPARE(v[QStringLiteral("CRC-16/ARC")], 0xBB3Du);
        QCOMPARE(v[QStringLiteral("CRC-16/XMODEM")], 0x31C3u);
        QCOMPARE(v[QStringLiteral("CRC-16/CCITT-FALSE")], 0x29B1u);
        QCOMPARE(v[QStringLiteral("CRC-16/KERMIT")], 0x2189u);
        QCOMPARE(v[QStringLiteral("CRC-8 (poly 07)")], 0xF4u);
        QCOMPARE(v[QStringLiteral("CRC-8/MAXIM")], 0xA1u);
        QCOMPARE(v[QStringLiteral("xor8")], 0x31u);
        QCOMPARE(v[QStringLiteral("sum8")], 0xDDu);
    }

    void exchangesDiffHints() {
        QTemporaryDir dir;
        Project p;
        QVERIFY(Project::load(makeRecording(dir.filePath(QStringLiteral("rec"))), &p, nullptr));
        Timeline tl;
        tl.build(p);
        const auto msgs = messages(p, tl);
        QCOMPARE(msgs.size(), size_t(48));
        const auto ex = exchanges(msgs);
        auto stat = std::find_if(ex.begin(), ex.end(), [](const ExchangeGroup& g) { return g.question == "STAT?\r"; });
        QVERIFY(stat != ex.end());
        QCOMPARE(stat->count, 20);
        QCOMPARE(stat->answers.size(), size_t(4));
        QCOMPARE(stat->changes, 3);
        QVERIFY(std::abs(stat->latency - 0.02) < 1e-3);
        QVERIFY(stat->sentence().contains(QStringLiteral("asked 20 times")));
        // between mark 1 and mark 2: the S command's value and length changed, the answer to STAT? too
        double fa, ta, fb, tb;
        QString la, lb, err;
        QVERIFY(resolveMoment(tl, p, QStringLiteral("1"), true, &fa, &ta, &la, &err));
        QVERIFY(resolveMoment(tl, p, QStringLiteral("mark:2"), false, &fb, &tb, &lb, &err));
        QCOMPARE(la, QStringLiteral("mark 1 (setting 0)"));
        QVERIFY(!resolveMoment(tl, p, QStringLiteral("9"), true, &fb, &tb, &lb, &err));
        QVERIFY(resolveMoment(tl, p, QStringLiteral("2"), true, &fb, &tb, &lb, &err));
        const auto rows = diffMoments(msgs, fa, ta, fb, tb);
        std::map<QString, DiffRow> byFam;
        for (const DiffRow& r : rows) byFam[r.family] = r;
        QCOMPARE(byFam[QStringLiteral("tap ← OK")].status, QStringLiteral("changed"));
        QCOMPARE(byFam[QStringLiteral("tap → STAT?")].status, QStringLiteral("same"));
        bool sChanged = false;
        for (const auto& [f, r] : byFam)
            if (f.startsWith(QStringLiteral("tap → 0x02")) && r.status == QLatin1String("changed")) sChanged = true;
        // the S frames differ in length (5 and 7 bytes): two families of 0x02, or one changed family
        QVERIFY(sChanged || byFam.size() >= 4);
        // hints: the binary frames' checksum and length field; the polling answer's number follows the marks
        const auto hs = hints(msgs, tl);
        bool xorFound = false, lenFound = false, follows = false;
        for (const FamilyHints& h : hs) {
            for (const QString& c : h.checksum) xorFound |= c.contains(QStringLiteral("xor8")) && c.contains(QStringLiteral("bytes 1 to"));
            for (const QString& l : h.length) lenFound |= l == QStringLiteral("byte 1 = message length");
            for (const QString& v : h.varying) follows |= v.contains(QStringLiteral("at the marks: mark 1: 10, mark 2: 20, mark 3: 30, mark 4: 40"));
        }
        // the S frames with the same length (steps 0 and 2: 5 bytes; 1 and 3: 7 bytes) form families of 2: too few
        // for the checksum search (it needs 3 different messages); all four frames together:
        std::vector<Msg> sOnly;
        for (const Msg& m : msgs)
            if (m.role == 0 && m.bytes.startsWith('\x02')) {
                Msg x = m;
                x.family = QStringLiteral("S");
                sOnly.push_back(x);
            }
        for (const FamilyHints& h : hints(sOnly, tl)) {
            for (const QString& c : h.checksum) xorFound |= c.contains(QStringLiteral("xor8")) && c.contains(QStringLiteral("bytes 1 to"));
            for (const QString& l : h.length) lenFound |= l == QStringLiteral("byte 1 = message length");
        }
        QVERIFY(xorFound);
        QVERIFY(lenFound);
        QVERIFY(follows);
        const QJsonObject st = stats(p, tl, msgs);
        QCOMPARE(st.value(QStringLiteral("marks")).toInt(), 4);
    }

    void protocolAndCoverage() {
        QTemporaryDir dir;
        Project p;
        QVERIFY(Project::load(makeRecording(dir.filePath(QStringLiteral("rec"))), &p, nullptr));
        Timeline tl;
        tl.build(p);
        Protocol proto;
        QString err;
        const QByteArray json = R"({"format":"libretap-protocol","version":1,"name":"test","messages":[
            {"name":"status?","dir":"to-instrument","text":"^STAT\\?\\r$","say":"Status asked"},
            {"name":"status","dir":"from-instrument","text":"^OK (\\d+)\\r\\n$","fields":[{"name":"v","group":1,"type":"int","scale":0.1,"unit":"V"}],"say":"Status: {v}"},
            {"name":"set","dir":"to-instrument","hex":"02 ?? 53 …","fields":[{"name":"value","offset":3,"type":"u8","values":{"30":"thirty"}}],"say":"Set {value}"}
        ]})";
        QVERIFY2(proto.parse(QJsonDocument::fromJson(json).object(), &err), qPrintable(err));
        QStringList said;
        int explained = 0, total = 0;
        for (const TItem& it : tl.rows) {
            const Decoded d = proto.decode(p, it, tl);
            explained += d.explained;
            total += d.total;
            if (d.message >= 0) said << d.sentence;
        }
        QVERIFY(said.contains(QStringLiteral("Status asked")));
        QVERIFY(said.contains(QStringLiteral("Status: 1 V")));
        QVERIFY(said.contains(QStringLiteral("Set 10")));
        QVERIFY(said.contains(QStringLiteral("Set thirty")));
        // everything but the four "A" acknowledgements (4 bytes each) is explained
        QCOMPARE(total - explained, 16);
        Protocol bad;
        QVERIFY(!bad.parse(QJsonDocument::fromJson(R"({"format":"x"})").object(), &err));
    }

    void keyframesAndExport() {
        QTemporaryDir dir;
        Project p;
        QVERIFY(Project::load(makeRecording(dir.filePath(QStringLiteral("rec"))), &p, nullptr));
        Timeline tl;
        tl.build(p);
        const auto keys = keyFrames(p, tl, messages(p, tl), dir.filePath(QStringLiteral("kf")));
        QVERIFY(keys.size() >= 3);
        QVERIFY(QFile::exists(keys.front().file));
        QVERIFY(keys.front().reasons.join(QLatin1Char(' ')).contains(QStringLiteral("mark 1")));
        QVERIFY(!screenAt(p, tl, tl.end).isNull());
        QString index, err;
        QVERIFY(exportForAi(p, dir.filePath(QStringLiteral("ai")), &index, &err));
        QFile f(index);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString md = QString::fromUtf8(f.readAll());
        QVERIFY(md.contains(QStringLiteral("### Mark 2 at")));
        QVERIFY(md.contains(QStringLiteral("STAT?")));
        QVERIFY(md.contains(QStringLiteral("keyframes/key_001")));
        for (const char* n : {"traffic.txt", "exchanges.txt", "hints.txt"}) QVERIFY(QFile::exists(dir.filePath(QStringLiteral("ai/") + QLatin1String(n))));
    }

    void usbpcapFilter() {
        using namespace pcapgen;
        // device 1: a keyboard (HID boot keyboard), device 2: an FTDI serial bridge
        std::vector<Frame> f;
        double t = 2000;
        quint64 irp = 1;
        auto control = [&](int dev, const QByteArray& setup, const QByteArray& in) {
            f.push_back({t += 0.001, usbpcap(irp, false, dev, 0x80, 2, setup, 0)});
            f.push_back({t += 0.001, usbpcap(irp++, true, dev, 0x80, 2, in, 3)});
        };
        auto setup = [](int rt, int req, int val, int idx, int len) {
            return QByteArray(1, char(rt)) + char(req) + le16(val) + le16(idx) + le16(len);
        };
        control(1, setup(0x80, 6, 0x0100, 0, 18), QByteArray::fromHex("12010002000000 08 f003 2400 0001 0102 0001"));
        control(1, setup(0x80, 6, 0x0200, 0, 34), QByteArray::fromHex("09022200010100a032 090400000103010100 0921100100012241 00 0705810308000a"));
        control(2, setup(0x80, 6, 0x0100, 0, 18), QByteArray::fromHex("12010002000000400304016000060102 0301"));
        // the keyboard types a password; the bridge carries "IO L 09\n", one character per transfer, and "OK\n"
        for (char c : QByteArray("secret")) f.push_back({t += 0.05, usbpcap(irp, true, 1, 0x81, 1, QByteArray(2, 0) + char(c - 'a' + 4) + QByteArray(5, 0))}), ++irp;
        for (char c : QByteArray("IO L 09\n")) f.push_back({t += 0.08, usbpcap(irp++, false, 2, 0x02, 3, QByteArray(1, c))});
        f.push_back({t += 0.02, usbpcap(irp++, true, 2, 0x81, 3, "OK\n")});
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("cap.pcapng"));
        QVERIFY(save(file, pcapng(f, false, 249)));
        UsbPcapResult scan;
        QString err;
        QVERIFY2(scanUsbPcap(file, &scan, &err), qPrintable(err));
        QCOMPARE(scan.devices.size(), size_t(2));
        QCOMPARE(scan.devices[0].id, QStringLiteral("03f0:0024"));
        QCOMPARE(scan.devices[0].kind, QStringLiteral("keyboard"));
        QCOMPARE(scan.devices[1].id, QStringLiteral("0403:6001"));
        SessionWriter w;
        QVERIFY(w.create(dir.filePath(QStringLiteral("s")), {}, nullptr));
        w.setSources({QJsonObject{{QStringLiteral("id"), QStringLiteral("usb")}, {QStringLiteral("kind"), QStringLiteral("usbpcap")}}});
        UsbPcapFilter flt;
        flt.ids << QStringLiteral("0403:6001");
        UsbPcapResult r;
        QVERIFY(importUsbPcap(file, &w, flt, &r, &err));
        w.finish();
        QVERIFY(r.dropped >= 8);   // the keyboard's descriptors and key reports
        QFile ev(dir.filePath(QStringLiteral("s/events.jsonl")));
        QVERIFY(ev.open(QIODevice::ReadOnly));
        const QByteArray all = ev.readAll();
        QVERIFY(!all.contains("03f0"));
        QVERIFY(!all.contains("\"addr\":1,"));
        Session s;
        QVERIFY(Session::load(dir.filePath(QStringLiteral("s")), &s, &err));
        QStringList data;
        for (const Event& e : s.events)
            if (e.kind == QLatin1String("data")) data << e.str("dir") + QLatin1Char(':') + QString::fromLatin1(e.bytes());
        QCOMPARE(data, (QStringList{QStringLiteral("out:IO L 09\n"), QStringLiteral("in:OK\n")}));   // one character per transfer, joined
    }
};

QTEST_MAIN(TestAnalysis)
#include "test_analysis.moc"
