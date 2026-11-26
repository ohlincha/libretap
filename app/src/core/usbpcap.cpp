#include "usbpcap.h"

#include <QJsonArray>

#include <functional>
#include <map>

#include "pcap.h"
#include "session.h"
#include "usbpico.h"

namespace lt {

namespace {

// USBPCAP_BUFFER_PACKET_HEADER (little endian), then for control transfers the stage byte, then the data.
struct Urb {
    double t = 0;
    quint64 irp = 0;
    quint32 status = 0;
    int info = 0, bus = 0, dev = 0, ep = 0, transfer = -1, stage = -1;
    QByteArray data;
};

quint32 le(const QByteArray& d, int i, int n) {
    quint32 v = 0;
    for (int k = n - 1; k >= 0; --k) v = v << 8 | uchar(d[i + k]);
    return v;
}

bool parseUrb(const Packet& p, Urb* u) {
    const QByteArray& d = p.data;
    if (p.linktype != 249 || d.size() < 27) return false;
    const int hl = int(le(d, 0, 2));
    if (hl < 27 || hl > d.size()) return false;
    u->t = p.t;
    u->irp = quint64(le(d, 2, 4)) | quint64(le(d, 6, 4)) << 32;
    u->status = le(d, 10, 4);
    u->info = uchar(d[16]);
    u->bus = int(le(d, 17, 2));
    u->dev = int(le(d, 19, 2));
    u->ep = uchar(d[21]);
    u->transfer = uchar(d[22]);
    u->stage = (u->transfer == 2 && hl >= 28) ? uchar(d[27]) : -1;
    u->data = d.mid(hl);
    return true;
}

struct Out {
    double t;
    int key;
    bool control;
    QJsonObject f;
};

// Walks the URBs: names control transfers and collects the transfers (and the device table) through `emit`.
void walk(const std::vector<Packet>& packets, UsbDecoder& dec, std::map<int, bool>& hubs, std::map<int, std::pair<int, qint64>>& counts,
          const std::function<void(const Out&)>& sink) {
    struct Pending {
        double t = 0;
        QByteArray setup, out, in;
    };
    std::map<std::pair<int, quint64>, Pending> pend;
    for (const Packet& p : packets) {
        Urb u;
        if (!parseUrb(p, &u)) continue;
        const int key = u.bus * 1000 + u.dev;
        const bool fromDevice = u.info & 1;
        if (u.transfer == 2) {
            auto& pe = pend[{key, u.irp}];
            if (u.stage == 0 && !fromDevice && u.data.size() >= 8) {
                pe = Pending{};
                pe.t = u.t;
                pe.setup = u.data.left(8);
                pe.out = u.data.mid(8);
            } else if (u.stage == 1) {
                (fromDevice ? pe.in : pe.out) += u.data;
            } else if (u.stage == 3 && fromDevice) {
                pe.in += u.data;
                if (pe.setup.size() == 8) {
                    const QByteArray& s = pe.setup;
                    const int rt = uchar(s[0]), req = uchar(s[1]), val = int(le(s, 2, 2)), idx = int(le(s, 4, 2)), len = int(le(s, 6, 2));
                    const bool in = rt & 0x80;
                    if (((rt >> 5) & 3) == 1 && (rt & 0x1F) == 3) hubs[key] = true;   // class request to a port: a hub
                    if (((rt >> 5) & 3) != 0 && (rt & 0x1F) != 3) hubs.emplace(key, false);
                    const QByteArray data = in ? pe.in : pe.out;
                    const QString what = dec.describeControl(key, rt, req, val, idx, len, data, u.status != 0);
                    auto& c = counts[key];
                    ++c.first;
                    c.second += data.size();
                    QJsonObject f{{QStringLiteral("src"), QStringLiteral("usb")}, {QStringLiteral("addr"), u.dev}, {QStringLiteral("ep"), 0},
                                  {QStringLiteral("dir"), in ? QStringLiteral("in") : QStringLiteral("out")}, {QStringLiteral("what"), what},
                                  {QStringLiteral("setup"), QString::fromLatin1(s.toHex())}};
                    if (!data.isEmpty() && !(((rt >> 5) & 3) == 0 && req == 6)) f.insert(QStringLiteral("hex"), QString::fromLatin1(data.toHex()));
                    if (u.bus != 1) f.insert(QStringLiteral("bus"), u.bus);
                    sink({pe.t, key, true, f});
                }
                pend.erase({key, u.irp});
            }
            continue;
        }
        if (u.transfer != 1 && u.transfer != 3) continue;   // isochronous and the rest: not decoded
        const bool epIn = u.ep & 0x80;
        // a hub's only data: its port-change bitmap, 1-2 bytes on interrupt endpoint 1
        if (!(u.transfer == 1 && epIn && (u.ep & 0x0F) == 1 && u.data.size() <= 2) && !u.data.isEmpty() && epIn == fromDevice) hubs.emplace(key, false);
        // OUT data travels with the request, IN data with the completion
        if (epIn != fromDevice || u.data.isEmpty() || (fromDevice && u.status != 0)) continue;
        auto& c = counts[key];
        ++c.first;
        c.second += u.data.size();
        QJsonObject f{{QStringLiteral("src"), QStringLiteral("usb")}, {QStringLiteral("addr"), u.dev}, {QStringLiteral("ep"), u.ep & 0x0F},
                      {QStringLiteral("dir"), epIn ? QStringLiteral("in") : QStringLiteral("out")},
                      {QStringLiteral("what"), QStringLiteral("%1 %2 bytes").arg(u.transfer == 1 ? QStringLiteral("interrupt") : QStringLiteral("bulk")).arg(u.data.size())},
                      {QStringLiteral("hex"), QString::fromLatin1(u.data.toHex())}};
        if (u.bus != 1) f.insert(QStringLiteral("bus"), u.bus);
        sink({u.t, key, false, f});
    }
}

QString idOf(const UsbDevice& d) {
    return d.vid < 0 ? QString() : QStringLiteral("%1:%2").arg(d.vid, 4, 16, QLatin1Char('0')).arg(d.pid, 4, 16, QLatin1Char('0'));
}

bool load(const QString& file, std::vector<Packet>* packets, QString* error) {
    if (!PcapReader::readFile(file, packets, error)) return false;
    for (const Packet& p : *packets)
        if (p.linktype == 249) return true;
    if (error) *error = QStringLiteral("%1 is not a USBPcap capture (no USB packets, link type 249)").arg(file);
    return false;
}

void devices(UsbDecoder& dec, const std::map<int, bool>& hubs, const std::map<int, std::pair<int, qint64>>& counts, UsbPcapResult* out) {
    out->devices.clear();
    for (const auto& [key, c] : counts) {
        UsbPcapDevice d;
        d.bus = key / 1000;
        d.addr = key % 1000;
        const UsbDevice& u = dec.devices[key];
        d.id = idOf(u);
        const auto h = hubs.find(key);
        const bool hub = (h != hubs.end() && h->second) || (d.id.isEmpty() && h == hubs.end()) || u.kindText() == QLatin1String("hub");
        d.kind = hub ? QStringLiteral("hub") : u.kindText();
        d.transfers = c.first;
        d.bytes = c.second;
        out->devices.push_back(d);
    }
}
}  // namespace

QJsonObject UsbPcapDevice::toJson() const {
    return {{QStringLiteral("bus"), bus}, {QStringLiteral("address"), addr}, {QStringLiteral("id"), id}, {QStringLiteral("kind"), kind},
            {QStringLiteral("transfers"), transfers}, {QStringLiteral("bytes"), double(bytes)}, {QStringLiteral("kept"), kept}};
}

bool scanUsbPcap(const QString& file, UsbPcapResult* out, QString* error) {
    std::vector<Packet> packets;
    if (!load(file, &packets, error)) return false;
    UsbDecoder dec;
    std::map<int, bool> hubs;
    std::map<int, std::pair<int, qint64>> counts;
    walk(packets, dec, hubs, counts, [](const Out&) {});
    devices(dec, hubs, counts, out);
    out->t0 = packets.front().t;
    out->t1 = packets.back().t;
    return true;
}

bool importUsbPcap(const QString& file, SessionWriter* w, const UsbPcapFilter& f, UsbPcapResult* out, QString* error) {
    std::vector<Packet> packets;
    if (!load(file, &packets, error)) return false;
    // first the device table (descriptors may come after a device's first transfers), then the transfers
    UsbDecoder dec;
    std::map<int, bool> hubs;
    std::map<int, std::pair<int, qint64>> counts;
    walk(packets, dec, hubs, counts, [](const Out&) {});
    devices(dec, hubs, counts, out);
    std::set<int> keep;
    for (UsbPcapDevice& d : out->devices) {
        d.kept = f.all || f.addresses.count(d.addr) || (!d.id.isEmpty() && f.ids.contains(d.id, Qt::CaseInsensitive)) ||
                 (f.hubs && d.kind == QLatin1String("hub"));
        if (d.kept) keep.insert(d.bus * 1000 + d.addr);
    }
    UsbDecoder named;
    std::map<int, bool> h2;
    std::map<int, std::pair<int, qint64>> c2;
    out->written = out->dropped = 0;
    // USB-serial bridges carry a serial stream in many small bulk transfers (often one character each): joined into
    // lines (until CR/LF, a 0.3 s pause, or the other direction), like the serial tap's chunks
    std::set<int> serial;
    for (const UsbPcapDevice& d : out->devices)
        if (d.kind.startsWith(QLatin1String("USB-serial")) || d.kind.startsWith(QLatin1String("serial"))) serial.insert(d.bus * 1000 + d.addr);
    struct Pend {
        bool on = false;
        double t = 0, last = 0;
        int n = 0;
        QByteArray bytes;
        QJsonObject f;
    };
    std::map<int, Pend[2]> pend;
    auto write = [&](double t, bool control, QJsonObject fields, int key) {
        const QString id = idOf(dec.devices[key]);
        if (!id.isEmpty()) fields.insert(QStringLiteral("dev"), id);
        w->event(t, control ? QStringLiteral("usb") : QStringLiteral("data"), fields);
        ++out->written;
    };
    auto flush = [&](int key, int d) {
        Pend& p = pend[key][d];
        if (!p.on) return;
        p.f.insert(QStringLiteral("hex"), QString::fromLatin1(p.bytes.toHex()));
        p.f.insert(QStringLiteral("what"), QStringLiteral("bulk %1 bytes%2").arg(p.bytes.size()).arg(p.n > 1 ? QStringLiteral(" (%1 transfers)").arg(p.n) : QString()));
        write(p.t, false, p.f, key);
        p = Pend{};
    };
    walk(packets, named, h2, c2, [&](const Out& o) {
        if (!keep.count(o.key)) {
            ++out->dropped;
            return;
        }
        const bool bulk = !o.control && o.f.value(QStringLiteral("what")).toString().startsWith(QLatin1String("bulk"));
        if (!bulk || !serial.count(o.key)) {
            write(o.t, o.control, o.f, o.key);
            return;
        }
        const int d = o.f.value(QStringLiteral("dir")).toString() == QLatin1String("in") ? 1 : 0;
        flush(o.key, 1 - d);
        Pend& p = pend[o.key][d];
        if (p.on && (o.t - p.last > 0.3 || p.bytes.endsWith('\n') || p.bytes.endsWith('\r'))) flush(o.key, d);
        if (!p.on) {
            p.on = true;
            p.t = o.t;
            p.f = o.f;
        }
        p.bytes += QByteArray::fromHex(o.f.value(QStringLiteral("hex")).toString().toLatin1());
        p.last = o.t;
        ++p.n;
    });
    for (auto& [key, pp] : pend) {
        (void)pp;
        flush(key, 0);
        flush(key, 1);
    }
    out->t0 = packets.front().t;
    out->t1 = packets.back().t;
    return true;
}

}  // namespace lt
