#include "pcap.h"

#include <QFile>
#include <QHostAddress>
#include <QtEndian>

#include <cmath>

namespace lt {

// ---- reading

quint32 PcapReader::u32(qsizetype at) const {
    const auto* p = reinterpret_cast<const uchar*>(buf_.constData() + at);
    const quint32 le = quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
    return swap_ ? qbswap(le) : le;
}

quint16 PcapReader::u16(qsizetype at) const {
    const auto* p = reinterpret_cast<const uchar*>(buf_.constData() + at);
    const quint16 le = quint16(p[0] | p[1] << 8);
    return swap_ ? qbswap(le) : le;
}

void PcapReader::feed(const QByteArray& more) {
    buf_ += more;
    if (kind_ == Kind::Unknown) {
        if (buf_.size() - pos_ < 4) return;
        swap_ = false;
        const quint32 m = u32(pos_);
        if (m == 0x0A0D0D0A) kind_ = Kind::Ng;
        else if (m == 0xa1b2c3d4 || m == 0xa1b23c4d || m == 0xd4c3b2a1 || m == 0x4d3cb2a1) kind_ = Kind::Classic;
        else {
            error = QStringLiteral("not a pcap or pcapng file");
            return;
        }
        if (kind_ == Kind::Classic && !parseClassicHeader()) {
            kind_ = Kind::Unknown;
            return;
        }
    }
    while (error.isEmpty() && parseBlock()) {}
    if (pos_ > (1 << 20)) {
        buf_.remove(0, pos_);
        pos_ = 0;
    }
}

bool PcapReader::parseClassicHeader() {
    if (buf_.size() - pos_ < 24) return false;
    const quint32 m = u32(pos_);
    swap_ = m == 0xd4c3b2a1 || m == 0x4d3cb2a1;
    nanos_ = m == 0xa1b23c4d || m == 0x4d3cb2a1;
    classicLink_ = int(u32(pos_ + 20) & 0xFFFF);
    pos_ += 24;
    return true;
}

bool PcapReader::parseBlock() {
    const qsizetype left = buf_.size() - pos_;
    if (kind_ == Kind::Classic) {
        if (left < 16) return false;
        const quint32 incl = u32(pos_ + 8);
        if (incl > (256u << 20)) {
            error = QStringLiteral("damaged capture file (packet of %1 bytes)").arg(incl);
            return false;
        }
        if (left < 16 + qsizetype(incl)) return false;
        Packet p;
        p.t = u32(pos_) + u32(pos_ + 4) * (nanos_ ? 1e-9 : 1e-6);
        p.origLen = int(u32(pos_ + 12));
        p.linktype = classicLink_;
        p.data = buf_.mid(pos_ + 16, incl);
        out_.push_back(std::move(p));
        pos_ += 16 + incl;
        return true;
    }
    if (left < 12) return false;
    // the section header's type reads the same in both byte orders; its byte-order magic decides
    if (static_cast<uchar>(buf_[pos_]) == 0x0A && static_cast<uchar>(buf_[pos_ + 3]) == 0x0A && static_cast<uchar>(buf_[pos_ + 1]) == 0x0D) {
        swap_ = false;
        const quint32 bom = u32(pos_ + 8);
        if (bom == 0x4D3C2B1A) swap_ = true;
        else if (bom != 0x1A2B3C4D) {
            error = QStringLiteral("damaged pcapng section header");
            return false;
        }
        ifaces_.clear();
    }
    const quint32 type = u32(pos_), len = u32(pos_ + 4);
    if (len < 12 || len % 4 || len > (256u << 20)) {
        error = QStringLiteral("damaged pcapng block (length %1)").arg(len);
        return false;
    }
    if (left < qsizetype(len)) return false;
    const qsizetype b = pos_;
    if (type == 1 && len >= 20) {   // interface description
        Iface f;
        f.link = u16(b + 8);
        for (qsizetype o = b + 16; o + 4 <= b + len - 4;) {
            const int code = u16(o), olen = u16(o + 2);
            if (code == 0) break;
            if (code == 9 && olen >= 1) {
                const int v = static_cast<uchar>(buf_[o + 4]);
                f.unit = (v & 0x80) ? std::ldexp(1.0, -(v & 0x7F)) : std::pow(10.0, -v);
            }
            o += 4 + ((olen + 3) & ~3);
        }
        ifaces_.push_back(f);
    } else if ((type == 6 || type == 2) && len >= 32) {   // enhanced (or old) packet
        const int iface = type == 6 ? int(u32(b + 8)) : u16(b + 8);
        const Iface f = iface < int(ifaces_.size()) ? ifaces_[size_t(iface)] : Iface{};
        const quint64 ts = quint64(u32(b + 12)) << 32 | u32(b + 16);
        const quint32 cap = std::min<quint32>(u32(b + 20), len - 32);
        Packet p;
        p.t = double(ts) * f.unit;
        p.linktype = f.link;
        p.origLen = int(u32(b + 24));
        p.data = buf_.mid(b + 28, cap);
        out_.push_back(std::move(p));
    } else if (type == 3 && len >= 16) {   // simple packet: no time; the last one's
        Packet p;
        p.t = out_.empty() ? 0 : out_.back().t;
        p.linktype = ifaces_.empty() ? 1 : ifaces_[0].link;
        p.origLen = int(u32(b + 8));
        p.data = buf_.mid(b + 12, std::min<qsizetype>(p.origLen, len - 16));
        out_.push_back(std::move(p));
    }
    pos_ += len;
    return true;
}

std::vector<Packet> PcapReader::take() {
    std::vector<Packet> o;
    o.swap(out_);
    return o;
}

bool PcapReader::readFile(const QString& path, std::vector<Packet>* out, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1").arg(path);
        return false;
    }
    PcapReader r;
    r.feed(f.readAll());
    if (!r.error.isEmpty()) {
        if (error) *error = QStringLiteral("%1: %2").arg(path, r.error);
        return false;
    }
    *out = r.take();
    return true;
}

// ---- packets in plain words

namespace {
quint16 be16(const QByteArray& d, int i) { return quint16(uchar(d[i]) << 8 | uchar(d[i + 1])); }
quint32 be32(const QByteArray& d, int i) { return quint32(be16(d, i)) << 16 | be16(d, i + 2); }
QString ip4(const QByteArray& d, int i) { return QStringLiteral("%1.%2.%3.%4").arg(uchar(d[i])).arg(uchar(d[i + 1])).arg(uchar(d[i + 2])).arg(uchar(d[i + 3])); }
QString mac(const QByteArray& d, int i) { return QString::fromLatin1(d.mid(i, 6).toHex(':')); }
QString ip6(const QByteArray& d, int i) {
    Q_IPV6ADDR a;
    for (int k = 0; k < 16; ++k) a[k] = quint8(d[i + k]);
    return QHostAddress(a).toString();
}
QString ep(const QString& ip, int port) { return ip.contains(QLatin1Char(':')) ? QStringLiteral("[%1]:%2").arg(ip).arg(port) : QStringLiteral("%1:%2").arg(ip).arg(port); }

QString flagText(int f) {
    QStringList s;
    if (f & 2) s << QStringLiteral("SYN");
    if (f & 1) s << QStringLiteral("FIN");
    if (f & 4) s << QStringLiteral("RST");
    if (f & 8) s << QStringLiteral("PSH");
    if (f & 16) s << QStringLiteral("ACK");
    return s.join(QLatin1Char(','));
}

void transport(NetPacket& n, int proto, const QByteArray& d, int off, int end) {
    if (proto == 6 && end - off >= 20) {
        n.proto = QStringLiteral("tcp");
        n.sport = be16(d, off);
        n.dport = be16(d, off + 2);
        n.seq = be32(d, off + 4);
        n.ack = be32(d, off + 8);
        const int hl = (uchar(d[off + 12]) >> 4) * 4;
        n.flags = uchar(d[off + 13]) & 0x3F;
        if (off + hl <= end) n.payload = d.mid(off + hl, end - off - hl);
        n.summary = QStringLiteral("TCP %1 → %2 [%3]%4").arg(ep(n.src, n.sport), ep(n.dst, n.dport), flagText(n.flags),
                                                            n.payload.isEmpty() ? QString() : QStringLiteral(" %1 bytes").arg(n.payload.size()));
    } else if (proto == 17 && end - off >= 8) {
        n.proto = QStringLiteral("udp");
        n.sport = be16(d, off);
        n.dport = be16(d, off + 2);
        const int ulen = std::min<int>(be16(d, off + 4), end - off);
        n.payload = d.mid(off + 8, std::max(0, ulen - 8));
        n.summary = QStringLiteral("UDP %1 → %2 %3 bytes").arg(ep(n.src, n.sport), ep(n.dst, n.dport)).arg(n.payload.size());
    } else if ((proto == 1 || proto == 58) && end - off >= 4) {
        n.proto = QStringLiteral("icmp");
        const int type = uchar(d[off]);
        const bool v6 = proto == 58;
        const QString what = (!v6 && type == 8) || (v6 && type == 128) ? QStringLiteral("echo request (ping)")
                             : (!v6 && type == 0) || (v6 && type == 129) ? QStringLiteral("echo reply")
                             : (!v6 && type == 3) || (v6 && type == 1) ? QStringLiteral("destination unreachable")
                             : QStringLiteral("type %1").arg(type);
        n.summary = QStringLiteral("%1 %2 %3 → %4").arg(v6 ? QStringLiteral("ICMPv6") : QStringLiteral("ICMP"), what, n.src, n.dst);
    } else {
        n.proto = QStringLiteral("other");
        n.summary = QStringLiteral("IP protocol %1 %2 → %3").arg(proto).arg(n.src, n.dst);
    }
}
}  // namespace

NetPacket parsePacket(const Packet& p) {
    NetPacket n;
    const QByteArray& d = p.data;
    int off = 0, type = -1;
    QString srcMac;
    switch (p.linktype) {
    case 1:   // Ethernet
        if (d.size() < 14) break;
        srcMac = mac(d, 6);
        type = be16(d, 12);
        off = 14;
        while ((type == 0x8100 || type == 0x88a8) && d.size() >= off + 4) type = be16(d, off + 2), off += 4;   // VLAN tags
        break;
    case 113:   // Linux cooked (dumpcap -i any)
        if (d.size() >= 16) type = be16(d, 14), off = 16;
        break;
    case 276:
        if (d.size() >= 20) type = be16(d, 0), off = 20;
        break;
    case 0: {   // BSD loopback
        if (d.size() < 4) break;
        const quint32 fam = quint32(uchar(d[0])) | quint32(uchar(d[1])) << 8 | quint32(uchar(d[2])) << 16 | quint32(uchar(d[3])) << 24;
        type = fam == 2 ? 0x0800 : (fam == 24 || fam == 28 || fam == 30) ? 0x86DD : -1;
        off = 4;
        break;
    }
    case 101: case 228: case 229:
        if (!d.isEmpty()) type = (uchar(d[0]) >> 4) == 6 ? 0x86DD : 0x0800;
        break;
    default:
        n.proto = QStringLiteral("other");
        n.summary = QStringLiteral("link type %1, %2 bytes").arg(p.linktype).arg(d.size());
        return n;
    }
    if (type == 0x0800 && d.size() >= off + 20) {
        const int ihl = (uchar(d[off]) & 0x0F) * 4;
        const int total = std::min<int>(be16(d, off + 2), int(d.size()) - off);
        n.src = ip4(d, off + 12);
        n.dst = ip4(d, off + 16);
        if ((be16(d, off + 6) & 0x1FFF) != 0) {   // a later fragment
            n.proto = QStringLiteral("other");
            n.summary = QStringLiteral("IP fragment %1 → %2").arg(n.src, n.dst);
            return n;
        }
        transport(n, uchar(d[off + 9]), d, off + ihl, off + total);
    } else if (type == 0x86DD && d.size() >= off + 40) {
        n.src = ip6(d, off + 8);
        n.dst = ip6(d, off + 24);
        transport(n, uchar(d[off + 6]), d, off + 40, std::min<int>(int(d.size()), off + 40 + be16(d, off + 4)));
    } else if (type == 0x0806 && d.size() >= off + 28) {
        n.proto = QStringLiteral("arp");
        const int op = be16(d, off + 6);
        n.src = ip4(d, off + 14);
        n.dst = ip4(d, off + 24);
        n.summary = op == 1 ? QStringLiteral("ARP: who has %1? tell %2").arg(n.dst, n.src)
                            : QStringLiteral("ARP: %1 is at %2").arg(n.src, mac(d, off + 8));
    } else {
        n.proto = QStringLiteral("other");
        n.src = srcMac;
        const QString name = type == 0x88cc ? QStringLiteral("LLDP (a switch introducing itself)")
                             : type >= 0 && type < 0x600 ? QStringLiteral("802.3 frame (spanning tree or similar)")
                                                         : QStringLiteral("Ethernet type 0x%1").arg(type, 4, 16, QLatin1Char('0'));
        n.summary = srcMac.isEmpty() ? name : QStringLiteral("%1 from %2").arg(name, srcMac);
    }
    return n;
}

// ---- conversations

NetFlows::NetFlows(const QString& ip, double gap) : ip_(ip), gap_(gap) {}

void NetFlows::emitChunks(const QString&, Conn& c, const std::vector<Chunk>& chunks) {
    for (const Chunk& ch : chunks)
        out_.push_back({ch.t, QStringLiteral("data"),
                        {{QStringLiteral("src"), QStringLiteral("eth")},
                         {QStringLiteral("dir"), ch.dir ? QStringLiteral("b") : QStringLiteral("a")},
                         {QStringLiteral("hex"), QString::fromLatin1(ch.data.toHex())},
                         {QStringLiteral("end"), ch.end},
                         {QStringLiteral("proto"), QStringLiteral("tcp")},
                         {QStringLiteral("conn"), c.pc + QStringLiteral(" → ") + c.inst}}});
}

void NetFlows::deliver(Conn& c, const QString& key, int dir, const QByteArray& bytes, double t) {
    if (bytes.isEmpty()) return;
    bytes_[dir] += bytes.size();
    emitChunks(key, c, c.chunker.feed(dir, bytes, t));
}

void NetFlows::add(const Packet& p) {
    ++packets_;
    const NetPacket n = parsePacket(p);
    if (!ip_.isEmpty() && n.src != ip_ && n.dst != ip_) {
        ++ignored_;
        return;
    }
    for (auto& [k, c] : conns_) emitChunks(k, *c, c->chunker.poll(p.t));
    auto packetEvent = [&](const QString& text) {
        out_.push_back({p.t, QStringLiteral("packet"), {{QStringLiteral("src"), QStringLiteral("eth")}, {QStringLiteral("proto"), n.proto}, {QStringLiteral("text"), text}}});
    };
    if (n.proto == QLatin1String("tcp")) {
        const QString a = ep(n.src, n.sport), b = ep(n.dst, n.dport);
        const QString key = a < b ? a + QLatin1Char('|') + b : b + QLatin1Char('|') + a;
        auto& slot = conns_[key];
        if (!slot) {
            slot = std::make_unique<Conn>();
            slot->chunker = Chunker(gap_);
            bool srcIsPc;
            if (!ip_.isEmpty()) srcIsPc = n.dst == ip_;
            else if ((n.flags & 2) && !(n.flags & 16)) srcIsPc = true;    // SYN: the client
            else if ((n.flags & 2) && (n.flags & 16)) srcIsPc = false;    // SYN-ACK: the server answers
            else srcIsPc = n.sport >= n.dport;                            // the higher (ephemeral) port is the client
            slot->pc = srcIsPc ? a : b;
            slot->inst = srcIsPc ? b : a;
        }
        Conn& c = *slot;
        const int dir = a == c.pc ? 0 : 1;
        Dir& d = c.d[dir];
        if (n.flags & 2) {
            d.known = true;
            d.next = n.seq + 1;
            d.held.clear();
            packetEvent(n.flags & 16 ? QStringLiteral("TCP connection accepted: %1 → %2").arg(a, b)
                                     : QStringLiteral("TCP connection opened: %1 → %2").arg(a, b));
            return;
        }
        // joining mid-stream: this side's acknowledgement says where the other side's bytes continue
        if ((n.flags & 16) && !c.d[1 - dir].known) {
            c.d[1 - dir].known = true;
            c.d[1 - dir].next = n.ack;
        }
        if (!n.payload.isEmpty()) {
            quint32 seq = n.seq;
            QByteArray data = n.payload;
            if (!d.known) {
                d.known = true;
                d.next = seq;
            }
            const qint32 diff = qint32(seq - d.next);
            if (diff + data.size() <= 0) {
                ++retrans_;
                packetEvent(QStringLiteral("TCP retransmission %1 → %2, %3 bytes already seen").arg(a, b).arg(data.size()));
            } else if (diff > 0) {
                auto it = d.held.find(seq);
                if (it == d.held.end() || it->second.size() < data.size()) d.held[seq] = data;   // out of order: later
            } else {
                if (diff < 0) data = data.mid(-diff);
                d.next += quint32(data.size());
                deliver(c, key, dir, data, p.t);
                // segments that were waiting for this one
                for (bool more = true; more;) {
                    more = false;
                    for (auto it = d.held.begin(); it != d.held.end(); ++it) {
                        const qint32 dd = qint32(it->first - d.next);
                        if (dd > 0) continue;
                        QByteArray h = it->second;
                        d.held.erase(it);
                        if (dd + h.size() > 0) {
                            h = h.mid(-dd);
                            d.next += quint32(h.size());
                            deliver(c, key, dir, h, p.t);
                        }
                        more = true;
                        break;
                    }
                }
            }
        }
        if (n.flags & 1) packetEvent(QStringLiteral("TCP connection closed by %1 (FIN)").arg(a));
        else if (n.flags & 4) packetEvent(QStringLiteral("TCP connection reset by %1 (RST)").arg(a));
        else if (n.payload.isEmpty()) ++acks_;
        return;
    }
    if (n.proto == QLatin1String("udp")) {
        const bool toInst = !ip_.isEmpty() ? n.dst == ip_ : n.sport >= n.dport;
        const QString a = ep(n.src, n.sport), b = ep(n.dst, n.dport);
        bytes_[toInst ? 0 : 1] += n.payload.size();
        out_.push_back({p.t, QStringLiteral("data"),
                        {{QStringLiteral("src"), QStringLiteral("eth")},
                         {QStringLiteral("dir"), toInst ? QStringLiteral("a") : QStringLiteral("b")},
                         {QStringLiteral("hex"), QString::fromLatin1(n.payload.toHex())},
                         {QStringLiteral("proto"), QStringLiteral("udp")},
                         {QStringLiteral("conn"), toInst ? a + QStringLiteral(" → ") + b : b + QStringLiteral(" → ") + a}}});
        return;
    }
    packetEvent(n.summary);
}

void NetFlows::poll(double t) {
    for (auto& [k, c] : conns_) emitChunks(k, *c, c->chunker.poll(t));
}

void NetFlows::flush() {
    for (auto& [k, c] : conns_) emitChunks(k, *c, c->chunker.flush());
}

std::vector<NetFlows::Out> NetFlows::take() {
    std::vector<Out> o;
    o.swap(out_);
    std::stable_sort(o.begin(), o.end(), [](const Out& a, const Out& b) { return a.t < b.t; });
    return o;
}

QJsonObject NetFlows::stats() const {
    return {{QStringLiteral("eth_packets"), double(packets_)}, {QStringLiteral("eth_bytes_a"), double(bytes_[0])}, {QStringLiteral("eth_bytes_b"), double(bytes_[1])},
            {QStringLiteral("tcp_acks"), double(acks_)}, {QStringLiteral("tcp_retransmissions"), double(retrans_)},
            {QStringLiteral("ignored"), double(ignored_)}, {QStringLiteral("connections"), int(conns_.size())}};
}

}  // namespace lt
