#pragma once
// Synthetic usb-sniffer-lite text dumps (our own, written for the tests; the format as the sniffer prints it).
#include <QByteArray>
#include <QString>

namespace usbdump {

class Dump {
public:
    QString text;
    void line(int t, const QString& body) { text += QStringLiteral("%1 : %2\n").arg(t, 6).arg(body); }
    void sof() { line(1000, QStringLiteral("SOF #%1").arg(sof_++ % 2048)), t_ = 4; }
    void reset() { line(0, QStringLiteral("--- RESET ---")), t_ = 0; }
    void token(const char* kind, int addr, int ep) { line(next(), QStringLiteral("%1: 0x%2/%3").arg(QLatin1String(kind)).arg(addr, 2, 16, QLatin1Char('0')).arg(ep)); }
    void data(const QString& pid, const QByteArray& d) { line(next(), pid + QStringLiteral(": ") + (d.isEmpty() ? QStringLiteral("ZLP") : QString::fromLatin1(d.toHex(' ')))); }
    void hs(const char* h) { line(next(), QLatin1String(h)); }
    // a control transfer with its data stage (IN when the request type says so) and status stage
    void control(int addr, const QByteArray& setup, const QByteArray& payload = {}, int maxp = 64) {
        sof();
        token("SETUP", addr, 0);
        data(QStringLiteral("DATA0"), setup);
        hs("ACK");
        const bool in = uchar(setup[0]) & 0x80;
        bool d1 = true;
        for (int i = 0; i < payload.size() || (i == 0 && false); i += maxp) {
            token(in ? "IN" : "OUT", addr, 0);
            data(d1 ? QStringLiteral("DATA1") : QStringLiteral("DATA0"), payload.mid(i, maxp));
            hs("ACK");
            d1 = !d1;
        }
        token(in ? "OUT" : "IN", addr, 0);   // status stage
        data(QStringLiteral("DATA1"), {});
        hs("ACK");
    }
    void bulk(const char* dir, int addr, int ep, const QString& pid, const QByteArray& d) {
        token(dir, addr, ep);
        data(pid, d);
        hs("ACK");
    }
    void nak(const char* dir, int addr, int ep) {
        token(dir, addr, ep);
        hs("NAK");
    }

private:
    int next() { return t_ += 9; }
    int sof_ = 300, t_ = 0;
};

inline QByteArray setup(int rt, int req, int val, int idx, int len) {
    QByteArray s(8, 0);
    s[0] = char(rt), s[1] = char(req), s[2] = char(val & 0xFF), s[3] = char(val >> 8), s[4] = char(idx & 0xFF), s[5] = char(idx >> 8),
    s[6] = char(len & 0xFF), s[7] = char(len >> 8);
    return s;
}

// An FTDI USB-serial instrument: enumeration, 9600 baud, one question and its answer (with FTDI's status bytes,
// a retransmitted OUT packet and NAKs, as on a real bus).
inline QString ftdiInstrument() {
    Dump d;
    d.reset();
    const QByteArray dev = QByteArray::fromHex("12010002000000400304016000060102 0301");
    const QByteArray cfg = QByteArray::fromHex("09022000010100802d 090400000 2ffffff02 0705810240 0000 0705020240 0000");
    d.control(0, setup(0x80, 6, 0x0100, 0, 64), dev);
    d.control(0, setup(0x00, 5, 5, 0, 0));
    d.control(5, setup(0x80, 6, 0x0100, 0, 18), dev);
    d.control(5, setup(0x80, 6, 0x0200, 0, 9), cfg.left(9));
    d.control(5, setup(0x80, 6, 0x0200, 0, 32), cfg);
    d.control(5, setup(0x00, 9, 1, 0, 0));
    d.control(5, setup(0x40, 3, 0x4138, 0, 0));     // FTDI SET_BAUD_RATE: 9600
    d.control(5, setup(0x40, 4, 0x0008, 0, 0));     // FTDI SET_DATA: 8N1
    d.sof();
    d.bulk("OUT", 5, 2, QStringLiteral("DATA0"), "MEAS?\r");
    d.bulk("OUT", 5, 2, QStringLiteral("DATA0"), "MEAS?\r");   // the host missed the ACK: the same packet again
    d.nak("IN", 5, 1);
    d.bulk("IN", 5, 1, QStringLiteral("DATA0"), QByteArray::fromHex("0160"));   // only the status bytes
    d.sof();
    d.bulk("IN", 5, 1, QStringLiteral("DATA1"), QByteArray::fromHex("0160") + "M 0.5123 V\r\n");
    return d.text;
}

// A CDC-ACM instrument being set to 9600 8N1 and DTR on.
inline QString cdcLineCoding() {
    Dump d;
    d.control(3, setup(0x21, 0x20, 0, 0, 7), QByteArray::fromHex("80250000000008"));
    d.control(3, setup(0x21, 0x22, 3, 0, 0));
    return d.text;
}

}  // namespace usbdump
