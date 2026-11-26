#pragma once
// Synthetic USB Sniffer Stream records (firmware/pico-usb-sniffer/, docs/FORMAT.md): what the streaming Pico firmware
// sends, written for the tests. The packets' CRC bytes are zeros (the decoder does not check them; the Pico did).
#include <QByteArray>

#include <initializer_list>
#include <utility>
#include <vector>

#include "core/usbpico.h"

namespace usbstream {

inline QByteArray bytes(std::initializer_list<char> b) { return QByteArray(b.begin(), qsizetype(b.size())); }
inline QByteArray le32(quint32 v) { return bytes({char(v), char(v >> 8), char(v >> 16), char(v >> 24)}); }

inline QByteArray record(int type, quint32 time, const QByteArray& payload) {
    QByteArray r;
    r += char(0xA5);
    r += char(type);
    r += char(payload.size() & 0xff);
    r += char(payload.size() >> 8);
    r += le32(time);
    r += payload;
    const quint16 crc = lt::picoCrc16(r.constData() + 1, r.size() - 1);
    r += char(crc & 0xff);
    r += char(crc >> 8);
    return r;
}

inline char pidByte(int pid) { return char((pid & 0xf) | ((~pid & 0xf) << 4)); }
// PACKET payloads: flags, then the packet from its PID on
inline QByteArray token(int pid, int addr, int ep) {
    const int v = (addr & 0x7f) | (ep & 0xf) << 7;
    return bytes({char(0), pidByte(pid), char(v & 0xff), char(v >> 8)});
}
inline QByteArray data(int pid, const QByteArray& d) { return QByteArray(1, char(0)) + pidByte(pid) + d + QByteArray(2, char(0)); }
inline QByteArray handshake(int pid) { return bytes({char(0), pidByte(pid)}); }

inline QByteArray hello(quint32 t) {
    return record(1, t, bytes({char(1), char(1), char(0), char(0)}) + le32(131072) + le32(81920) + QByteArray("USB Sniffer Stream 1.0.0"));
}
inline QByteArray status(quint32 t, int type, quint32 frames, quint32 dropped, quint32 sent, quint32 lostCapture = 0) {
    QByteArray p = le32(frames) + bytes({char(0xff), char(0xff)}) + le32(dropped) + le32(sent) + le32(0) + le32(lostCapture) + le32(0);
    p += char(3);
    p += char(1);
    return record(type, t, p);
}

// The FTDI instrument of usbdump::ftdiInstrument(), as the Pico streams it: NAKed polling already dropped, times in µs
// from `t0` (the Pico's timer; pass a value near 2^32 to cross its wrap). Pairs of (time, record).
class Ftdi {
public:
    explicit Ftdi(quint32 t0) : t_(t0) {}
    std::vector<std::pair<quint32, QByteArray>> out;
    void packet(const QByteArray& p) { out.emplace_back(t_, record(2, t_, p)), t_ += 9; }
    void control(int addr, const QByteArray& setup, const QByteArray& payload = {}) {
        t_ += 1000;
        packet(token(13, addr, 0));
        packet(data(3, setup));
        packet(handshake(2));
        const bool in = uchar(setup[0]) & 0x80;
        bool d1 = true;
        for (int i = 0; i < payload.size(); i += 64) {
            packet(token(in ? 9 : 1, addr, 0));
            packet(data(d1 ? 11 : 3, payload.mid(i, 64)));
            packet(handshake(2));
            d1 = !d1;
        }
        packet(token(in ? 1 : 9, addr, 0));
        packet(data(11, {}));
        packet(handshake(2));
    }
    void enumeration() {
        const QByteArray dev = QByteArray::fromHex("12010002000000400304016000060102 0301");
        const QByteArray cfg = QByteArray::fromHex("09022000010100802d 090400000 2ffffff02 0705810240 0000 0705020240 0000");
        out.emplace_back(t_, record(3, t_, {}));   // bus reset
        control(0, setup(0x80, 6, 0x0100, 0, 64), dev);
        control(0, setup(0x00, 5, 5, 0, 0));
        control(5, setup(0x80, 6, 0x0100, 0, 18), dev);
        control(5, setup(0x80, 6, 0x0200, 0, 9), cfg.left(9));
        control(5, setup(0x80, 6, 0x0200, 0, 32), cfg);
        control(5, setup(0x00, 9, 1, 0, 0));
        control(5, setup(0x40, 3, 0x4138, 0, 0));   // FTDI SET_BAUD_RATE: 9600
        control(5, setup(0x40, 4, 0x0008, 0, 0));   // FTDI SET_DATA: 8N1
    }
    void question() {
        t_ += 1000;
        packet(token(1, 5, 2));
        packet(data(3, "MEAS?\r"));
        packet(handshake(2));
        packet(token(1, 5, 2));
        packet(data(3, "MEAS?\r"));   // the host missed the ACK: the same packet again
        packet(handshake(2));
        packet(token(9, 5, 1));
        packet(data(3, QByteArray::fromHex("0160")));   // FTDI status bytes only
        packet(handshake(2));
    }
    void answer() {
        t_ += 1000;
        packet(token(9, 5, 1));
        packet(data(11, QByteArray::fromHex("0160") + "M 0.5123 V\r\n"));
        packet(handshake(2));
    }
    void wait(quint32 us) { t_ += us; }
    quint32 now() const { return t_; }

private:
    static QByteArray setup(int rt, int req, int val, int idx, int len) {
        QByteArray s(8, 0);
        s[0] = char(rt), s[1] = char(req), s[2] = char(val & 0xFF), s[3] = char(val >> 8), s[4] = char(idx & 0xFF), s[5] = char(idx >> 8),
        s[6] = char(len & 0xFF), s[7] = char(len >> 8);
        return s;
    }
    quint32 t_;
};

}  // namespace usbstream
