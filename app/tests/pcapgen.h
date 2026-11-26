#pragma once
// Synthetic captures for the tests: Ethernet frames (ARP, IPv4 TCP/UDP; checksums left at 0) written as pcapng or
// classic pcap.
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtEndian>

#include <vector>

namespace pcapgen {

struct Frame {
    double t;
    QByteArray data;
};

inline QByteArray be16(int v) { return QByteArray(1, char(v >> 8)) + QByteArray(1, char(v & 0xFF)); }
inline QByteArray be32(quint32 v) { return be16(int(v >> 16)) + be16(int(v & 0xFFFF)); }
inline QByteArray ip(const char* s) {
    QByteArray o;
    for (const QByteArray& p : QByteArray(s).split('.')) o += char(p.toInt());
    return o;
}
inline QByteArray mac(int last) { return QByteArray::fromHex("0200000000") + char(last); }

inline QByteArray ether(int dstMac, int srcMac, int type, const QByteArray& payload) {
    QByteArray f = mac(dstMac) + mac(srcMac) + be16(type) + payload;
    while (f.size() < 60) f += char(0);   // Ethernet's minimum frame: padding the IP length must ignore
    return f;
}

inline QByteArray ipv4(const char* src, const char* dst, int proto, const QByteArray& l4) {
    return QByteArray::fromHex("45") + char(0) + be16(20 + int(l4.size())) + be16(0) + be16(0x4000) + char(64) + char(proto) + be16(0) + ip(src) +
           ip(dst) + l4;
}

enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };

inline QByteArray tcp(const char* src, int sp, const char* dst, int dp, quint32 seq, quint32 ack, int flags, const QByteArray& data = {}) {
    const QByteArray h = be16(sp) + be16(dp) + be32(seq) + be32(ack) + char(5 << 4) + char(flags) + be16(65535) + be16(0) + be16(0);
    return ether(dst[std::strlen(dst) - 1] - '0', src[std::strlen(src) - 1] - '0', 0x0800, ipv4(src, dst, 6, h + data));
}

inline QByteArray udp(const char* src, int sp, const char* dst, int dp, const QByteArray& data) {
    const QByteArray h = be16(sp) + be16(dp) + be16(8 + int(data.size())) + be16(0);
    return ether(1, 2, 0x0800, ipv4(src, dst, 17, h + data));
}

inline QByteArray arp(int op, const char* senderIp, int senderMac, const char* targetIp) {
    const QByteArray a = be16(1) + be16(0x0800) + char(6) + char(4) + be16(op) + mac(senderMac) + ip(senderIp) +
                         QByteArray(6, char(op == 1 ? 0 : 0x11)) + ip(targetIp);
    return ether(op == 1 ? 0xFF : 1, senderMac, 0x0806, a);
}

inline QByteArray le32(quint32 v) { return QByteArray(1, char(v)) + char(v >> 8) + char(v >> 16) + char(v >> 24); }
inline QByteArray le16(int v) { return QByteArray(1, char(v)) + char(v >> 8); }

inline QByteArray block(quint32 type, const QByteArray& body) {
    QByteArray b = body;
    while (b.size() % 4) b += char(0);
    const quint32 len = quint32(12 + b.size());
    return le32(type) + le32(len) + b + le32(len);
}

inline QByteArray pcapng(const std::vector<Frame>& frames, bool nanos = false) {
    QByteArray out = block(0x0A0D0D0A, le32(0x1A2B3C4D) + le16(1) + le16(0) + le32(0xFFFFFFFF) + le32(0xFFFFFFFF));
    QByteArray idb = le16(1) + le16(0) + le32(65535);
    if (nanos) idb += le16(9) + le16(1) + QByteArray(1, char(9)) + QByteArray(3, 0) + le16(0) + le16(0);   // if_tsresol = 9 (ns)
    out += block(1, idb);
    for (const Frame& f : frames) {
        const quint64 ts = quint64(f.t * (nanos ? 1e9 : 1e6) + 0.5);
        out += block(6, le32(0) + le32(quint32(ts >> 32)) + le32(quint32(ts)) + le32(quint32(f.data.size())) + le32(quint32(f.data.size())) + f.data);
    }
    return out;
}

inline QByteArray pcapClassic(const std::vector<Frame>& frames) {
    QByteArray out = le32(0xa1b2c3d4) + le16(2) + le16(4) + le32(0) + le32(0) + le32(65535) + le32(1);
    for (const Frame& f : frames) {
        const quint32 s = quint32(f.t), us = quint32((f.t - s) * 1e6 + 0.5);
        out += le32(s) + le32(us) + le32(quint32(f.data.size())) + le32(quint32(f.data.size())) + f.data;
    }
    return out;
}

// A small session: ARP, a TCP connection with a question and an answer in two segments that arrive out of order
// (and one again later), a second exchange, the close; a UDP ping-pong; a packet between two other hosts.
inline std::vector<Frame> scenario(double t0 = 1796031300.0) {
    const char *pc = "192.168.0.10", *inst = "192.168.0.20";
    std::vector<Frame> f;
    double t = t0;
    auto add = [&](double dt, const QByteArray& d) { f.push_back({t += dt, d}); };
    add(0, arp(1, pc, 0x10, inst));
    add(0.001, arp(2, inst, 0x20, pc));
    add(0.010, tcp(pc, 50000, inst, 5025, 1000, 0, SYN));
    add(0.001, tcp(inst, 5025, pc, 50000, 7000, 1001, SYN | ACK));
    add(0.001, tcp(pc, 50000, inst, 5025, 1001, 7001, ACK));
    add(0.100, tcp(pc, 50000, inst, 5025, 1001, 7001, PSH | ACK, "*IDN?\n"));
    add(0.005, tcp(inst, 5025, pc, 50000, 7006, 1007, PSH | ACK, "1000\n"));    // second half first
    add(0.001, tcp(inst, 5025, pc, 50000, 7001, 1007, ACK, "ACME,"));            // then the first
    add(0.001, tcp(pc, 50000, inst, 5025, 1007, 7011, ACK));
    add(0.050, tcp(inst, 5025, pc, 50000, 7001, 1007, ACK, "ACME,"));            // retransmitted
    add(0.200, tcp(pc, 50000, inst, 5025, 1007, 7011, PSH | ACK, "MEAS?\n"));
    add(0.004, tcp(inst, 5025, pc, 50000, 7011, 1013, PSH | ACK, "1.234\n"));
    add(0.300, udp(pc, 40000, inst, 5000, "PING"));
    add(0.002, udp(inst, 5000, pc, 40000, "PONG"));
    add(0.010, udp("192.168.0.30", 40001, "192.168.0.40", 5000, "OTHER"));
    add(0.100, tcp(pc, 50000, inst, 5025, 1013, 7017, FIN | ACK));
    return f;
}

inline bool save(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

}  // namespace pcapgen
