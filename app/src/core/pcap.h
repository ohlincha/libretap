#pragma once
// Reading packet captures without libpcap: classic pcap (µs or ns, either byte order) and pcapng (section header,
// interface description with its time resolution, enhanced/simple/old packet blocks). Incremental: feed() takes
// the bytes as a capture file grows (dumpcap writing while LibreTap records) and keeps an incomplete block for later.
//
// Then the packets in plain words (net summaries) and the conversations: TCP reassembled per connection and
// direction (out-of-order segments held back, retransmitted bytes dropped), cut into chunks like the serial tap
// (silence or the other side talking), so a text protocol over TCP reads like a serial one; UDP one datagram each.
#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <map>
#include <memory>
#include <vector>

#include "chunker.h"

namespace lt {

struct Packet {
    double t = 0;                  // capture time, seconds since 1970 (the capturing computer's clock)
    int linktype = 1;              // 1 Ethernet, 113 Linux cooked, 276 cooked v2, 0 BSD loopback, 101 raw IP
    QByteArray data;
    int origLen = 0;
};

class PcapReader {
public:
    void feed(const QByteArray& more);             // more bytes of the file
    std::vector<Packet> take();                    // packets parsed so far (then forgotten)
    QString error;                                 // not a capture file, …
    static bool readFile(const QString& path, std::vector<Packet>* out, QString* error);

private:
    bool parseClassicHeader();
    bool parseBlock();
    QByteArray buf_;
    qsizetype pos_ = 0;
    enum class Kind { Unknown, Classic, Ng } kind_ = Kind::Unknown;
    bool swap_ = false, nanos_ = false;
    int classicLink_ = 1;
    struct Iface {
        int link = 1;
        double unit = 1e-6;                        // seconds per timestamp tick
    };
    std::vector<Iface> ifaces_;
    std::vector<Packet> out_;
    quint32 u32(qsizetype at) const;
    quint16 u16(qsizetype at) const;
};

// One packet taken apart.
struct NetPacket {
    QString summary;               // "TCP 10.0.0.1:51000 → 10.0.0.2:5000 [SYN]", "ARP who has 10.0.0.2? tell 10.0.0.1"
    QString proto;                 // tcp, udp, arp, icmp, other
    QString src, dst;              // addresses (IP, or MAC for non-IP)
    int sport = 0, dport = 0;
    quint32 seq = 0, ack = 0;
    int flags = 0;                 // TCP flags (FIN 1, SYN 2, RST 4, PSH 8, ACK 16)
    QByteArray payload;            // TCP/UDP payload
};
NetPacket parsePacket(const Packet& p);

// Packets → timeline events: "data" (reassembled TCP or UDP payload, dir a = towards the instrument, b = from it)
// and "packet" (everything worth a line: connections opened and closed, ARP, ICMP, other protocols,
// retransmissions). Pure ACKs are only counted.
class NetFlows {
public:
    // instrumentIp: when set, packets that do not involve it are ignored, and "a" means "sent to the instrument".
    // Without it the side that opened a TCP connection (or used the higher port) is taken as the PC.
    explicit NetFlows(const QString& instrumentIp = {}, double gapSeconds = 0.020);
    struct Out {
        double t;
        QString kind;              // data | packet
        QJsonObject fields;
    };
    void add(const Packet& p);
    void poll(double t);           // chunks whose side has been quiet long enough by t (no packets meanwhile)
    void flush();                  // the end: everything pending
    std::vector<Out> take();
    QJsonObject stats() const;

private:
    struct Dir {
        bool known = false;
        quint32 next = 0;
        std::map<quint32, QByteArray> held;        // out-of-order segments by sequence number
    };
    struct Conn {
        QString pc, inst;                          // "ip:port" of each side
        Dir d[2];                                  // 0: PC → instrument, 1: back
        Chunker chunker;
        double last = 0;
    };
    void deliver(Conn& c, const QString& key, int dir, const QByteArray& bytes, double t);
    void emitChunks(const QString& key, Conn& c, const std::vector<Chunk>& chunks);
    QString ip_;
    double gap_;
    std::map<QString, std::unique_ptr<Conn>> conns_;
    std::vector<Out> out_;
    qint64 packets_ = 0, acks_ = 0, retrans_ = 0, bytes_[2] = {0, 0}, ignored_ = 0;
};

}  // namespace lt
