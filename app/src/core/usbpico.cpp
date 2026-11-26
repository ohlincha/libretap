#include "usbpico.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSerialPort>

#include <cmath>

#include "session.h"
#include "util.h"

namespace lt {

struct UsbControl {
    double t = 0;
    int addr = 0, rt = 0, req = 0, val = 0, idx = 0, len = 0;
    QByteArray data;
    bool trunc = false;
};

namespace {

const QRegularExpression kLine(QStringLiteral("^\\s*(\\d+|\\.\\.\\.)\\s*:\\s*(.*?)\\s*$"));
const QRegularExpression kToken(QStringLiteral("^(SETUP|IN|OUT): 0x([0-9a-fA-F]+)/([0-9a-fA-F])$"));
const QRegularExpression kData(QStringLiteral("^(DATA0|DATA1|DATA2|MDATA)(?: \\((\\d+)\\))?: (.*)$"));
const QRegularExpression kSof(QStringLiteral("^SOF #(\\d+)$"));
const QRegularExpression kFold(QStringLiteral("^Folded (\\d+) frames?$"));

QString stdName(int r) {
    static const std::map<int, const char*> m{{0, "GET_STATUS"}, {1, "CLEAR_FEATURE"}, {3, "SET_FEATURE"}, {5, "SET_ADDRESS"},
                                              {6, "GET_DESCRIPTOR"}, {7, "SET_DESCRIPTOR"}, {8, "GET_CONFIGURATION"},
                                              {9, "SET_CONFIGURATION"}, {10, "GET_INTERFACE"}, {11, "SET_INTERFACE"}, {12, "SYNCH_FRAME"}};
    auto it = m.find(r);
    return it != m.end() ? QString::fromLatin1(it->second) : QStringLiteral("standard 0x%1").arg(r, 2, 16, QLatin1Char('0'));
}

QString descName(int t) {
    static const std::map<int, const char*> m{{1, "device"}, {2, "configuration"}, {3, "string"}, {4, "interface"}, {5, "endpoint"},
                                              {6, "device qualifier"}, {0x21, "HID"}, {0x22, "HID report"}};
    auto it = m.find(t);
    return it != m.end() ? QString::fromLatin1(it->second) : QStringLiteral("type 0x%1").arg(t, 2, 16, QLatin1Char('0'));
}

QString className(int r) {
    static const std::map<int, const char*> m{{0x20, "SET_LINE_CODING (CDC)"}, {0x21, "GET_LINE_CODING (CDC)"},
                                              {0x22, "SET_CONTROL_LINE_STATE (CDC)"}, {0x23, "SEND_BREAK (CDC)"},
                                              {0x01, "GET_REPORT (HID)"}, {0x09, "SET_REPORT (HID)"}, {0x0A, "SET_IDLE (HID)"},
                                              {0x0B, "SET_PROTOCOL (HID)"}};
    auto it = m.find(r);
    return it != m.end() ? QString::fromLatin1(it->second) : QStringLiteral("class request 0x%1").arg(r, 2, 16, QLatin1Char('0'));
}

// USB-serial bridges: their payload is a plain serial stream (a serial tap on the instrument side may be simpler).
QString bridgeName(int vid) {
    switch (vid) {
    case 0x0403: return QStringLiteral("FTDI");
    case 0x067B: return QStringLiteral("Prolific PL2303");
    case 0x10C4: return QStringLiteral("Silicon Labs CP210x");
    case 0x1A86: return QStringLiteral("WCH CH340/CH341");
    default: return {};
    }
}

QString vendorName(int vid, int req, bool in) {
    static const std::map<int, std::map<int, const char*>> t{
        {0x0403, {{0, "FTDI RESET"}, {1, "FTDI SET_MODEM_CTRL"}, {2, "FTDI SET_FLOW_CTRL"}, {3, "FTDI SET_BAUD_RATE"}, {4, "FTDI SET_DATA"},
                  {5, "FTDI GET_MODEM_STATUS"}, {9, "FTDI SET_LATENCY_TIMER"}, {10, "FTDI GET_LATENCY_TIMER"}, {11, "FTDI SET_BITMODE"}}},
        {0x1A86, {{0x5F, "CH34x READ_VERSION"}, {0x95, "CH34x READ_REG"}, {0x9A, "CH34x WRITE_REG"}, {0xA1, "CH34x SERIAL_INIT"},
                  {0xA4, "CH34x MODEM_CTRL"}}},
        {0x067B, {{0x01, "PL2303 VENDOR_WRITE"}, {0x101, "PL2303 VENDOR_READ"}}},
        {0x10C4, {{0x00, "CP210x IFC_ENABLE"}, {0x01, "CP210x SET_BAUDDIV"}, {0x03, "CP210x SET_LINE_CTL"}, {0x07, "CP210x SET_MHS"},
                  {0x1E, "CP210x SET_BAUDRATE"}, {0x13, "CP210x SET_FLOW"}}}};
    auto v = t.find(vid);
    if (v != t.end()) {
        auto r = v->second.find(req | (in && vid == 0x067B ? 0x100 : 0));
        if (r == v->second.end()) r = v->second.find(req);
        if (r != v->second.end()) return QString::fromLatin1(r->second);
    }
    return QStringLiteral("vendor request 0x%1").arg(req, 2, 16, QLatin1Char('0'));
}

quint16 u16(const QByteArray& d, int i) { return quint16(uchar(d[i]) | uchar(d[i + 1]) << 8); }
QString hex4(int v) { return QStringLiteral("%1").arg(v, 4, 16, QLatin1Char('0')); }
}  // namespace

QString UsbDevice::kindText() const {
    if (deviceClass == 9) return QStringLiteral("hub");
    if (!bridgeName(vid).isEmpty()) return QStringLiteral("USB-serial (%1)").arg(bridgeName(vid));
    for (const auto& i : ifaces) {
        if (i[0] == 3 && i[2] == 1) return QStringLiteral("keyboard");
        if (i[0] == 3 && i[2] == 2) return QStringLiteral("mouse");
    }
    for (const auto& i : ifaces) {
        if (i[0] == 9) return QStringLiteral("hub");
        if (i[0] == 8) return QStringLiteral("storage");
        if (i[0] == 3) return QStringLiteral("HID");
        if (i[0] == 2 || i[0] == 10) return QStringLiteral("serial (CDC)");
        if (i[0] == 0xFF) return QStringLiteral("vendor-specific");
    }
    return vid >= 0 ? QStringLiteral("device") : QStringLiteral("unknown");
}

QString picoSentence(const QString& port, const QString& description) {
    if (description.contains(QLatin1String("Sniffer Stream")))
        return QStringLiteral("USB sniffer ready (Pico on %1, USB Sniffer Stream). It records continuously, like the other sources.").arg(port);
    return QStringLiteral("USB sniffer ready (Pico on %1). It captures in short pieces: press Record, do one action, press Stop.").arg(port);
}

QString ftdiBaudText(int val, int idx) {
    static const double frac[] = {0, 0.5, 0.25, 0.125, 0.375, 0.625, 0.75, 0.875};
    const double div = (val & 0x3FFF) + frac[((val >> 14) & 3) | ((idx & 1) << 2)];
    const int baud = div == 0 ? 3000000 : div == 1 ? 2000000 : int(std::lround(3000000 / div));
    return QString::number(baud);
}

UsbDecoder::UsbDecoder(bool lowSpeed, bool ftdi) : defaultMaxp_(lowSpeed ? 8 : 64), ftdi_(ftdi) {}

double UsbDecoder::now(int t) const { return haveFrame_ ? (frame_ * 1000 + t) / 1000.0 : (lsFrames_ * 1000 + t) / 1000.0; }

bool UsbDecoder::ftdiOn(int addr) const {
    if (ftdi_) return true;
    auto it = devices.find(addr);
    return it != devices.end() && it->second.vid == 0x0403;
}

int UsbDecoder::maxp(int addr, int ep, const QString& d) const {
    auto it = devices.find(addr);
    if (it != devices.end()) {
        auto e = it->second.eps.find(ep | (d == QLatin1String("IN") ? 0x80 : 0));
        if (e != it->second.eps.end()) return e->second.second;
    }
    return defaultMaxp_;
}

QString UsbDecoder::epKind(int addr, int ep, const QString& d) const {
    auto it = devices.find(addr);
    if (it != devices.end()) {
        auto e = it->second.eps.find(ep | (d == QLatin1String("IN") ? 0x80 : 0));
        if (e != it->second.eps.end()) return e->second.first;
    }
    return {};
}

void UsbDecoder::event(double tMs, int addr, int ep, const QString& dir, const QString& what, const QByteArray& data, bool trunc, bool control) {
    UsbEvent e;
    e.tMs = tMs;
    e.addr = addr;
    e.ep = ep;
    e.dir = dir;
    e.what = what;
    e.data = data;
    e.truncated = trunc;
    e.control = control;
    events.push_back(std::move(e));
}

UsbDecoder& UsbDecoder::feed(const QString& text) {
    for (const QString& raw : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
        const auto m = kLine.match(raw);
        if (!m.hasMatch()) continue;
        const int t = m.captured(1) == QLatin1String("...") ? 0 : m.captured(1).toInt();
        line(t, m.captured(2));
    }
    flushAll();
    std::stable_sort(events.begin(), events.end(), [](const UsbEvent& a, const UsbEvent& b) { return a.tMs < b.tMs; });
    return *this;
}

void UsbDecoder::line(int t, const QString& body) {
    if (const auto m = kSof.match(body); m.hasMatch()) {
        const int n = m.captured(1).toInt();
        if (sofRaw_ < 0) {
            frame_ = 0;
            haveFrame_ = true;
        } else {
            const int d = ((n - sofRaw_) % 2048 + 2048) % 2048;
            frame_ += d ? d : 2048;
        }
        sofRaw_ = n;
        return;
    }
    if (body == QLatin1String("LS SOF")) {
        ++lsFrames_;
        return;
    }
    if (const auto m = kFold.match(body); m.hasMatch()) {
        if (!haveFrame_) lsFrames_ += m.captured(1).toInt();
        return;
    }
    lineAt(now(t), body);
}

// A packet line at a known time (ms): from the dump (via line) or from the stream (via packet)
void UsbDecoder::lineAt(double t, const QString& body) {
    if (body == QLatin1String("--- RESET ---")) {
        flushAll();
        event(t, -1, -1, {}, QStringLiteral("bus reset"));
        toggle_.clear();
        return;
    }
    if (body.startsWith(QLatin1String("ERROR"))) {
        ++errors;
        event(t, -1, -1, {}, QStringLiteral("capture error: ") + body);
        return;
    }
    if (const auto m = kToken.match(body); m.hasMatch()) {
        token_ = {m.captured(1), m.captured(2).toInt(nullptr, 16), m.captured(3).toInt(nullptr, 16), t};
        haveToken_ = true;
        haveData_ = false;
        return;
    }
    if (const auto m = kData.match(body); m.hasMatch()) {
        QString rest = m.captured(3);
        const bool trunc = rest.endsWith(QLatin1String("..."));
        if (trunc) {
            truncated = true;
            rest.chop(3);
        }
        data_ = {m.captured(1), rest.trimmed() == QLatin1String("ZLP") ? QByteArray() : QByteArray::fromHex(rest.toLatin1()), trunc};
        haveData_ = true;
        return;
    }
    if (body == QLatin1String("ACK") || body == QLatin1String("NAK") || body == QLatin1String("STALL") || body == QLatin1String("NYET"))
        handshake(body);
}

void UsbDecoder::handshake(const QString& hs) {
    if (!haveToken_) return;
    const Token tk = token_;
    const bool hasData = haveData_;
    const Data data = data_;
    haveToken_ = haveData_ = false;
    if (hs == QLatin1String("NAK")) return;
    if (hs == QLatin1String("STALL")) {
        if (tk.ep == 0 && control_.count(tk.addr)) {
            auto c = control_[tk.addr];
            control_.erase(tk.addr);
            controlDone(*c, true, false);
        } else {
            event(tk.t, tk.addr, tk.ep, tk.kind, QStringLiteral("STALL"));
        }
        return;
    }
    if (!hasData) return;
    if (tk.kind == QLatin1String("SETUP")) {
        if (data.bytes.size() >= 8) {
            if (auto it = control_.find(tk.addr); it != control_.end()) {
                auto prev = it->second;
                control_.erase(it);
                controlDone(*prev, false, true);
            }
            auto c = std::make_shared<UsbControl>();
            c->t = tk.t;
            c->addr = tk.addr;
            c->rt = uchar(data.bytes[0]);
            c->req = uchar(data.bytes[1]);
            c->val = u16(data.bytes, 2);
            c->idx = u16(data.bytes, 4);
            c->len = u16(data.bytes, 6);
            control_[tk.addr] = c;
        }
        toggle_[{tk.addr, 0, QStringLiteral("IN")}] = QStringLiteral("DATA0");
        toggle_[{tk.addr, 0, QStringLiteral("OUT")}] = QStringLiteral("DATA0");
        return;
    }
    const QString d = tk.kind == QLatin1String("IN") ? QStringLiteral("IN") : QStringLiteral("OUT");
    const auto key = std::make_tuple(tk.addr, tk.ep, d);
    if (tk.ep != 0 && (data.pid == QLatin1String("DATA0") || data.pid == QLatin1String("DATA1")) && toggle_.count(key) && toggle_[key] == data.pid)
        return;   // the same packet again: the host did not see the ACK
    toggle_[key] = data.pid;
    if (tk.ep == 0) {
        auto it = control_.find(tk.addr);
        if (it == control_.end()) return;
        UsbControl& c = *it->second;
        const QString dataDir = (c.rt & 0x80) ? QStringLiteral("IN") : QStringLiteral("OUT");
        if (d == dataDir && c.len > 0 && c.data.size() < c.len && !data.bytes.isEmpty()) {
            c.data += data.bytes;
            c.trunc |= data.trunc;
        } else if (d != dataDir || c.len == 0) {
            auto keep = it->second;
            control_.erase(it);
            controlDone(*keep, false, false);
        }
        return;
    }
    QByteArray payload = data.bytes;
    if (ftdiOn(tk.addr) && d == QLatin1String("IN") && payload.size() >= 2) {
        payload = payload.mid(2);   // FTDI: two modem-status bytes in front of every IN packet
        if (payload.isEmpty()) return;
    }
    const double tn = tk.t;
    auto b = bulk_.find(key);
    if (b != bulk_.end() && tn - b->second.last > 10) {   // a pause: a new transfer
        flush(key);
        b = bulk_.end();
    }
    if (b == bulk_.end()) b = bulk_.emplace(key, Bulk{tn, tn, {}, false}).first;
    b->second.data += payload;
    b->second.trunc |= data.trunc;
    b->second.last = tn;
    const bool shortPacket = payload.size() + (ftdiOn(tk.addr) && d == QLatin1String("IN") ? 2 : 0) < maxp(tk.addr, tk.ep, d);
    if (shortPacket || epKind(tk.addr, tk.ep, d) == QLatin1String("interrupt")) flush(key);   // interrupt: a report per packet
}

void UsbDecoder::flush(const std::tuple<int, int, QString>& key) {
    auto it = bulk_.find(key);
    if (it == bulk_.end()) return;
    const Bulk b = it->second;
    bulk_.erase(it);
    if (b.data.isEmpty()) return;
    const auto& [addr, ep, d] = key;
    QString kind = epKind(addr, ep, d);
    if (kind.isEmpty()) kind = QStringLiteral("data");
    event(b.t, addr, ep, d, QStringLiteral("%1 %2 bytes").arg(kind).arg(b.data.size()), b.data, b.trunc);
}

void UsbDecoder::flushAll() {
    while (!bulk_.empty()) flush(bulk_.begin()->first);
    while (!control_.empty()) {
        auto c = control_.begin()->second;
        control_.erase(control_.begin());
        controlDone(*c, false, true);
    }
}

void UsbDecoder::packet(double tMs, int flags, const QByteArray& b) {
    static const char* const pidNames[16] = {"RESERVED", "OUT", "ACK", "DATA0", "PING", "SOF", "NYET", "DATA2",
                                             "SPLIT", "IN", "NAK", "DATA1", "PRE/ERR", "SETUP", "STALL", "MDATA"};
    if ((flags & 0xbf) || b.isEmpty()) {   // the firmware's error flags (bit 6 is unused)
        static const char* const names[] = {"STUFF", "CRC", "PID", "SYNC", "NBIT", "SIZE", nullptr, "TRUNCATED"};
        QStringList which;
        for (int i = 0; i < 8; ++i)
            if ((flags & (1 << i)) && names[i]) which << QLatin1String(names[i]);
        lineAt(tMs, QStringLiteral("ERROR [%1]: %2").arg(which.join(QStringLiteral(", ")), QString::fromLatin1(b.left(17).toHex(' '))));
        return;
    }
    const int pid = uchar(b[0]) & 0x0f;
    const QLatin1String name(pidNames[pid]);
    switch (pid) {
    case 1: case 9: case 13:   // OUT, IN, SETUP
        if (b.size() >= 3) {
            const int v = uchar(b[1]) | uchar(b[2]) << 8;
            lineAt(tMs, QStringLiteral("%1: 0x%2/%3").arg(name).arg(v & 0x7f, 2, 16, QLatin1Char('0')).arg((v >> 7) & 0xf, 0, 16));
        }
        break;
    case 3: case 11: case 7: case 15:   // DATA0/1/2, MDATA: PID, data, CRC16
        if (b.size() >= 3) {
            const QByteArray d = b.mid(1, b.size() - 3);
            lineAt(tMs, name + (d.isEmpty() ? QStringLiteral(": ZLP") : QStringLiteral(": ") + QString::fromLatin1(d.toHex(' '))));
        }
        break;
    case 2: case 10: case 14: case 6:   // ACK, NAK, STALL, NYET
        lineAt(tMs, name);
        break;
    default:   // SOF (only with "keep idle traffic"), PING, SPLIT, PRE: nothing to decode
        break;
    }
}

void UsbDecoder::reset(double tMs) { lineAt(tMs, QStringLiteral("--- RESET ---")); }

std::vector<UsbEvent> UsbDecoder::take(double nowMs, bool all) {
    if (all) {
        flushAll();
    } else {
        std::vector<std::tuple<int, int, QString>> done;
        for (const auto& [key, b] : bulk_)
            if (nowMs - b.last > 10) done.push_back(key);
        for (const auto& key : done) flush(key);
        std::vector<int> stale;   // a control transfer without its status stage for 5 s will not complete
        for (const auto& [addr, c] : control_)
            if (nowMs - c->t > 5000) stale.push_back(addr);
        for (int addr : stale) {
            auto c = control_[addr];
            control_.erase(addr);
            controlDone(*c, false, true);
        }
    }
    std::vector<UsbEvent> out;
    out.swap(events);
    std::stable_sort(out.begin(), out.end(), [](const UsbEvent& a, const UsbEvent& b) { return a.tMs < b.tMs; });
    return out;
}

UsbDecoder& UsbDecoder::feedStream(const QByteArray& bytes) {
    PicoStreamReader rd;
    double us = 0, hello = -1;
    quint32 last = 0;
    bool have = false;
    for (const PicoRecord& r : rd.feed(bytes)) {
        if (have) us += double(qint32(r.time - last));   // the Pico's 32-bit timer, unwrapped
        have = true;
        last = r.time;
        if (hello < 0) hello = us;
        const double ms = (us - hello) / 1000.0;
        if (r.type == PicoRecord::Hello && r.payload.size() >= 2) {
            defaultMaxp_ = r.payload[1] ? 64 : 8;
        } else if (r.type == PicoRecord::Packet && !r.payload.isEmpty()) {
            packet(ms, uchar(r.payload[0]), r.payload.mid(1));
        } else if (r.type == PicoRecord::Reset) {
            reset(ms);
        } else if (r.type == PicoRecord::Lost && r.payload.size() >= 8) {
            const quint32 n = (quint32(uchar(r.payload[0])) | quint32(uchar(r.payload[1])) << 8 | quint32(uchar(r.payload[2])) << 16 | quint32(uchar(r.payload[3])) << 24) +
                              (quint32(uchar(r.payload[4])) | quint32(uchar(r.payload[5])) << 8 | quint32(uchar(r.payload[6])) << 16 | quint32(uchar(r.payload[7])) << 24);
            lost += n;
            event(ms, -1, -1, {}, QStringLiteral("lost %1 packets: the sniffer's buffers were full").arg(n));
        }
    }
    flushAll();
    std::stable_sort(events.begin(), events.end(), [](const UsbEvent& a, const UsbEvent& b) { return a.tMs < b.tMs; });
    return *this;
}

void UsbDecoder::controlDone(const UsbControl& c, bool stalled, bool incomplete) {
    const int typ = (c.rt >> 5) & 3;
    const bool in = c.rt & 0x80;
    const QString name = describeControl(c.addr, c.rt, c.req, c.val, c.idx, c.len, c.data, stalled, incomplete);
    event(c.t, c.addr, 0, in ? QStringLiteral("IN") : QStringLiteral("OUT"), name,
          (typ == 0 && c.req == 6) ? QByteArray() : c.data, c.trunc, true);
    QByteArray setup(8, 0);
    setup[0] = char(c.rt), setup[1] = char(c.req), setup[2] = char(c.val), setup[3] = char(c.val >> 8), setup[4] = char(c.idx),
    setup[5] = char(c.idx >> 8), setup[6] = char(c.len), setup[7] = char(c.len >> 8);
    events.back().setup = setup;
}

QString UsbDecoder::describeControl(int addr, int rt, int req, int val, int idx, int len, const QByteArray& data, bool stalled, bool incomplete) {
    UsbControl c;
    c.addr = addr, c.rt = rt, c.req = req, c.val = val, c.idx = idx, c.len = len, c.data = data;
    const int typ = (c.rt >> 5) & 3;
    const bool in = c.rt & 0x80;
    UsbDevice& dev = devices[c.addr];
    QString name;
    if (typ == 0) {
        name = stdName(c.req);
        if (c.req == 6) {
            const int dt = c.val >> 8;
            name += QStringLiteral(" %1 #%2").arg(descName(dt)).arg(c.val & 0xFF);
            if (!stalled) name += QStringLiteral(": ") + descriptor(c.addr, dt, c.data);
        } else if (c.req == 5) {
            name += QStringLiteral(" %1").arg(c.val);
            devices[c.val] = devices[0];   // the device at address 0 moves to its new address
        } else if (c.req == 9) {
            name += QStringLiteral(" %1").arg(c.val);
        } else if (c.req == 1 || c.req == 3) {
            name += QStringLiteral(" feature %1 index 0x%2").arg(c.val).arg(c.idx, 0, 16);
        } else {
            name += QStringLiteral(" wValue=0x%1 wIndex=0x%2").arg(hex4(c.val), hex4(c.idx));
        }
    } else {
        name = typ == 1 ? className(c.req) : vendorName(dev.vid, c.req, in);
        name += QStringLiteral(" wValue=0x%1 wIndex=0x%2 wLength=%3").arg(hex4(c.val), hex4(c.idx)).arg(c.len);
        if (typ == 1 && c.req == 0x20 && c.data.size() >= 7) {
            const quint32 baud = quint32(uchar(c.data[0])) | quint32(uchar(c.data[1])) << 8 | quint32(uchar(c.data[2])) << 16 | quint32(uchar(c.data[3])) << 24;
            const int stop = uchar(c.data[4]), par = uchar(c.data[5]), bits = uchar(c.data[6]);
            name += QStringLiteral(" → %1 baud, %2 data bits, parity %3, stop %4")
                        .arg(baud).arg(bits)
                        .arg(par < 5 ? QString(QLatin1Char("NOEMS"[par])) : QString::number(par))
                        .arg(stop == 0 ? QStringLiteral("1") : stop == 1 ? QStringLiteral("1.5") : stop == 2 ? QStringLiteral("2") : QString::number(stop));
        }
        if (typ == 1 && c.req == 0x22) name += QStringLiteral(" (DTR %1, RTS %2)").arg(c.val & 1).arg((c.val >> 1) & 1);
        if (dev.vid == 0x0403 && c.req == 3) name += QStringLiteral(" (divisor 0x%1: about %2 baud)").arg(hex4(c.val), ftdiBaudText(c.val, c.idx));
    }
    if (stalled) name += QStringLiteral("  [STALL]");
    if (incomplete) name += QStringLiteral("  [incomplete]");
    return name;
}

QString UsbDecoder::descriptor(int addr, int dt, const QByteArray& d) {
    UsbDevice& dev = devices[addr];
    if (dt == 1 && d.size() >= 18) {
        const int usb = u16(d, 2), vid = u16(d, 8), pid = u16(d, 10);
        dev.vid = vid;
        dev.pid = pid;
        dev.deviceClass = uchar(d[4]);
        dev.maxp0 = uchar(d[7]);
        QString s = QStringLiteral("USB %1.%2, VID %3 PID %4, class %5/%6/%7, ep0 %8 bytes")
                        .arg(usb >> 8, 0, 16).arg(usb & 0xFF, 2, 16, QLatin1Char('0')).arg(hex4(vid), hex4(pid))
                        .arg(uchar(d[4]), 2, 16, QLatin1Char('0')).arg(uchar(d[5]), 2, 16, QLatin1Char('0'))
                        .arg(uchar(d[6]), 2, 16, QLatin1Char('0')).arg(uchar(d[7]));
        if (!bridgeName(vid).isEmpty()) s += QStringLiteral("  ← %1 USB-serial bridge: the payload is a plain serial stream").arg(bridgeName(vid));
        return s;
    }
    if (dt == 1) return QStringLiteral("%1 bytes (partial)").arg(d.size());
    if (dt == 2) {
        QStringList parts;
        int i = 0;
        while (i + 2 <= d.size()) {
            const int ln = uchar(d[i]), t = uchar(d[i + 1]);
            if (ln < 2) break;
            const QByteArray b = d.mid(i, ln);
            if (t == 4 && b.size() >= 9) dev.ifaces.push_back({uchar(b[5]), uchar(b[6]), uchar(b[7])});
            if (t == 4 && b.size() >= 9)
                parts << QStringLiteral("interface %1 alt %2 class %3/%4/%5").arg(uchar(b[2])).arg(uchar(b[3]))
                             .arg(uchar(b[5]), 2, 16, QLatin1Char('0')).arg(uchar(b[6]), 2, 16, QLatin1Char('0')).arg(uchar(b[7]), 2, 16, QLatin1Char('0'));
            else if (t == 5 && b.size() >= 7) {
                const int epa = uchar(b[2]), attr = uchar(b[3]), mp = u16(b, 4) & 0x7FF;
                static const char* const kinds[] = {"control", "isochronous", "bulk", "interrupt"};
                dev.eps[epa] = {QString::fromLatin1(kinds[attr & 3]), mp};
                parts << QStringLiteral("ep %1 %2 %3 %4 bytes").arg(epa & 0xF).arg(epa & 0x80 ? QStringLiteral("IN") : QStringLiteral("OUT"))
                             .arg(QString::fromLatin1(kinds[attr & 3])).arg(mp);
            }
            i += ln;
        }
        return parts.isEmpty() ? QStringLiteral("%1 bytes").arg(d.size()) : QStringLiteral("%1 bytes; ").arg(d.size()) + parts.join(QStringLiteral("; "));
    }
    if (dt == 3 && d.size() >= 2 && uchar(d[1]) == 3) {
        if (d.size() == 4 && uchar(d[0]) == 4) return QStringLiteral("languages ") + QString::fromLatin1(d.mid(2).toHex());
        const QByteArray s = d.mid(2);
        return QLatin1Char('"') + QString::fromUtf16(reinterpret_cast<const char16_t*>(s.constData()), s.size() / 2) + QLatin1Char('"');
    }
    return QStringLiteral("%1 bytes").arg(d.size());
}

QStringList UsbDecoder::summary() const {
    QStringList out;
    bool any = false;
    for (const auto& [a, d] : devices) {
        if (a == 0 && devices.size() > 1) continue;   // the same device before SET_ADDRESS
        if (d.vid < 0) continue;
        any = true;
        QString s = QStringLiteral("device %1: VID %2 PID %3").arg(a).arg(hex4(d.vid), hex4(d.pid));
        if (!bridgeName(d.vid).isEmpty()) s += QStringLiteral(" (%1 USB-serial)").arg(bridgeName(d.vid));
        out << s;
        for (const auto& [e, k] : d.eps)
            out << QStringLiteral("  ep %1 %2 %3, max packet %4").arg(e & 0xF).arg(e & 0x80 ? QStringLiteral("IN") : QStringLiteral("OUT"), k.first).arg(k.second);
    }
    std::map<std::tuple<int, int, QString>, int> tot;
    for (const UsbEvent& e : events)
        if (!e.control && e.ep > 0) tot[{e.addr, e.ep, e.dir}] += int(e.data.size());
    for (const auto& [k, n] : tot) out << QStringLiteral("data: device %1 ep %2 %3: %4 bytes").arg(std::get<0>(k)).arg(std::get<1>(k)).arg(std::get<2>(k)).arg(n);
    if (errors) out << QStringLiteral("%1 capture errors (wrong speed setting, or a poor splice?)").arg(errors);
    if (!any) out << QStringLiteral("no enumeration captured: unplug and replug the instrument while capturing to see what it is");
    return out;
}

// ---- USB Sniffer Stream's records

quint16 picoCrc16(const char* d, qsizetype n, quint16 crc) {
    for (qsizetype i = 0; i < n; ++i) {
        crc ^= quint16(uchar(d[i])) << 8;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
    }
    return crc;
}

std::vector<PicoRecord> PicoStreamReader::feed(const QByteArray& bytes) {
    std::vector<PicoRecord> out;
    buf_ += bytes;
    qsizetype i = 0;
    const qsizetype n = buf_.size();
    const char* b = buf_.constData();
    while (i < n) {
        if (uchar(b[i]) != 0xA5) {
            ++i, ++skipped;
            continue;
        }
        if (i + 8 > n) break;
        const int type = uchar(b[i + 1]);
        const int len = uchar(b[i + 2]) | uchar(b[i + 3]) << 8;
        if (type < PicoRecord::Hello || type > PicoRecord::End || len > 2048) {
            ++i, ++skipped;
            continue;
        }
        if (i + 10 + len > n) break;
        const quint16 crc = quint16(uchar(b[i + 8 + len]) | uchar(b[i + 9 + len]) << 8);
        if (picoCrc16(b + i + 1, 7 + len) != crc) {
            ++i, ++skipped;
            continue;
        }
        PicoRecord r;
        r.type = type;
        r.time = quint32(uchar(b[i + 4])) | quint32(uchar(b[i + 5])) << 8 | quint32(uchar(b[i + 6])) << 16 | quint32(uchar(b[i + 7])) << 24;
        r.payload = buf_.mid(i + 8, len);
        out.push_back(std::move(r));
        i += 10 + len;
    }
    buf_.remove(0, i);
    return out;
}

// ---- the Pico as a source

namespace {
struct Want {
    char key;
    const char* label;
};
const Want kWant[] = {{'e', "Capture speed"}, {'g', "Capture trigger"}, {'l', "Capture limit"},
                      {'t', "Time display format"}, {'a', "Data display format"}, {'f', "Fold empty frames"}};
const Want kWantStream[] = {{'e', "Capture speed"}, {'k', "Keep idle traffic"}, {'o', "Output format"}};

template <size_t N>
std::map<char, QString> parseSettings(const QString& text, const Want (&want)[N]) {
    std::map<char, QString> got;
    for (const Want& w : want) {
        const QRegularExpression re(QStringLiteral("%1 - %2\\s*: ([^\r\n]*)").arg(QString(QLatin1Char(w.key)), QRegularExpression::escape(QLatin1String(w.label))));
        const auto m = re.match(text);
        if (m.hasMatch()) got[w.key] = m.captured(1).trimmed();
    }
    return got;
}
std::map<char, QString> parseSettings(const QString& text) { return parseSettings(text, kWant); }

const QRegularExpression kStreamId(QStringLiteral("(USB Sniffer Stream (\\S+)) protocol (\\d+)"));

quint32 le32(const QByteArray& p, int i) {
    return quint32(uchar(p[i])) | quint32(uchar(p[i + 1])) << 8 | quint32(uchar(p[i + 2])) << 16 | quint32(uchar(p[i + 3])) << 24;
}
}  // namespace

PicoSource::PicoSource(const PicoConfig& c, QObject* parent) : Source(parent), c_(c) {
    watch_.setInterval(100);
    connect(&watch_, &QTimer::timeout, this, [this] {
        if (!port_) return;
        if (stream_) {   // between the Pico's records (at least one a second): end transfers that paused
            streamData();
            return;
        }
        if (full_) return;
        text_ += QString::fromLatin1(port_->readAll());
        if (text_.contains(QLatin1String("Capture stopped"))) {   // the buffer is full: the sniffer stopped by itself
            full_ = true;
            text_ += command(0, 1.5, 0.5, 120);
            decodeAndWrite(text_);
            emit recorded(w_->event(wallNow(), QStringLiteral("status"),
                                    {{QStringLiteral("text"), QStringLiteral("The USB sniffer's memory is full: it stopped capturing. "
                                                                             "Stop, then Record again for the next action.")}}));
            emit problem(QStringLiteral("The USB sniffer's memory is full. Stop, then Record again for the next action."));
        }
    });
}

PicoSource::~PicoSource() {
    if (port_) port_->close();
}

QJsonObject PicoSource::describe() const {
    QJsonObject o{{QStringLiteral("id"), id()},
                  {QStringLiteral("kind"), kind()},
                  {QStringLiteral("label"), QStringLiteral("USB sniffer (Pico), %1 speed").arg(c_.lowSpeed ? QStringLiteral("low") : QStringLiteral("full"))},
                  {QStringLiteral("port"), c_.port},
                  {QStringLiteral("speed"), c_.lowSpeed ? QStringLiteral("low") : QStringLiteral("full")},
                  {QStringLiteral("raw"), stream_ ? QStringLiteral("raw/usb_stream.bin") : QStringLiteral("raw/usb_cap.txt")},
                  {QStringLiteral("settings"), settings_}};
    if (!firmware_.isEmpty()) {
        o.insert(QStringLiteral("firmware"), firmware_);
        o.insert(QStringLiteral("mode"), stream_ ? QStringLiteral("stream") : QStringLiteral("capture"));
    }
    return o;
}

// Sends c (0: nothing) and reads until `idle` s of silence after something arrived, `first` s with nothing, `limit`
// in total, or `until` appears.
QString PicoSource::command(char c, double idle, double first, double limit, const QByteArray& until) {
    if (c) {
        port_->write(QByteArray(1, c));
        port_->waitForBytesWritten(500);
    }
    QByteArray buf;
    QElapsedTimer clock, quiet;
    clock.start();
    quiet.start();
    while (true) {
        if (port_->waitForReadyRead(50) || port_->bytesAvailable()) {
            const QByteArray chunk = port_->readAll();
            if (!chunk.isEmpty()) {
                buf += chunk;
                quiet.restart();
                if (!until.isEmpty() && buf.contains(until)) break;
            }
        }
        if (!buf.isEmpty() && quiet.elapsed() > idle * 1000) break;
        if (buf.isEmpty() && clock.elapsed() > first * 1000) break;
        if (clock.elapsed() > limit * 1000) break;
    }
    return QString::fromLatin1(buf);
}

// usb-sniffer-lite ignores `v`; USB Sniffer Stream answers "USB Sniffer Stream 1.0.0 protocol 1"
QString PicoSource::identify() {
    QString text = command(0, 0.3, 0.4, 2);   // what the firmware prints when the port opens (both: their help)
    text += command('v', 0.3, 0.6, 3, "protocol");
    const auto m = kStreamId.match(text);
    if (m.hasMatch() && m.captured(3).toInt() == 1) return m.captured(1);
    return {};
}

bool PicoSource::configure(QString* error) {
    std::map<char, QString> want{{'e', c_.lowSpeed ? QStringLiteral("Low") : QStringLiteral("Full")},
                                 {'g', QStringLiteral("Disabled")},
                                 {'l', QStringLiteral("Unlimited")},
                                 {'t', QStringLiteral("Relative to the SOF")},
                                 {'a', QStringLiteral("Full")},
                                 {'f', QStringLiteral("Enabled")}};
    auto got = parseSettings(command('h', 0.4, 3.0, 10));
    if (got.empty()) {
        if (error) *error = QStringLiteral("no answer from the USB sniffer on %1: is it the Pico, with usb-sniffer-lite on it?").arg(c_.port);
        return false;
    }
    static const QRegularExpression changed(QStringLiteral("changed to ([^\r\n]*)"));
    for (const auto& [k, value] : want) {
        for (int i = 0; i < 10 && got[k] != value; ++i) {
            const auto m = changed.match(command(k, 0.2, 1.0, 3));
            if (m.hasMatch()) got[k] = m.captured(1).trimmed();
        }
        if (got[k] != value) {
            if (error) *error = QStringLiteral("could not set the sniffer's %1 to %2").arg(QLatin1String(std::find_if(std::begin(kWant), std::end(kWant), [&](const Want& w) { return w.key == k; })->label), value);
            return false;
        }
    }
    settings_ = {};
    for (const Want& w : kWant) settings_.insert(QLatin1String(w.label), got[w.key]);
    return true;
}

// USB Sniffer Stream: the speed, idle traffic dropped on the Pico, binary records
bool PicoSource::configureStream(QString* error) {
    std::map<char, QString> want{{'e', c_.lowSpeed ? QStringLiteral("Low") : QStringLiteral("Full")},
                                 {'k', QStringLiteral("Disabled")},
                                 {'o', QStringLiteral("Binary")}};
    auto got = parseSettings(command('h', 0.4, 3.0, 10), kWantStream);
    static const QRegularExpression changed(QStringLiteral("changed to ([^\r\n]*)"));
    for (const auto& [k, value] : want) {
        for (int i = 0; i < 4 && got[k] != value; ++i) {
            const auto m = changed.match(command(k, 0.2, 1.0, 3));
            if (m.hasMatch()) got[k] = m.captured(1).trimmed();
        }
        if (got[k] != value) {
            if (error) *error = QStringLiteral("could not set the sniffer's %1 to %2").arg(QLatin1String(std::find_if(std::begin(kWantStream), std::end(kWantStream), [&](const Want& w) { return w.key == k; })->label), value);
            return false;
        }
    }
    settings_ = {};
    for (const Want& w : kWantStream) settings_.insert(QLatin1String(w.label), got[w.key]);
    return true;
}

bool PicoSource::probe(const QString& port, QString* sentence) {
    PicoSource p(PicoConfig{port, false});
    p.port_ = std::make_unique<QSerialPort>();
    p.port_->setPortName(port);
    if (!p.port_->open(QIODevice::ReadWrite)) {
        if (sentence) *sentence = QStringLiteral("cannot open %1: %2").arg(port, p.port_->errorString());
        return false;
    }
    p.port_->setDataTerminalReady(true);   // both firmwares only talk to an open port (DTR)
    const QString fw = p.identify();
    const auto got = parseSettings(p.command('h', 0.4, 2.0, 5));
    p.port_->close();
    if (got.empty()) {
        if (sentence) *sentence = QStringLiteral("%1 does not answer like usb-sniffer-lite").arg(port);
        return false;
    }
    const QString speed = got.count('e') ? got.at('e').toLower() : QStringLiteral("?");
    if (sentence)
        *sentence = fw.isEmpty() ? QStringLiteral("usb-sniffer-lite answers on %1 (%2 speed; it captures in short pieces)").arg(port, speed)
                                 : QStringLiteral("%1 answers on %2 (%3 speed; it records continuously)").arg(fw, port, speed);
    return true;
}

bool PicoSource::start(SessionWriter* writer, QString* error) {
    w_ = writer;
    full_ = false;
    events_ = 0;
    bytes_ = 0;
    stream_ = false;
    port_ = std::make_unique<QSerialPort>();
    port_->setPortName(c_.port);
    if (!port_->open(QIODevice::ReadWrite)) {
        if (error) *error = QStringLiteral("cannot open the USB sniffer on %1: %2").arg(c_.port, port_->errorString());
        port_.reset();
        return false;
    }
    port_->setBaudRate(115200);   // a USB CDC port: the rate does not matter
    port_->setDataTerminalReady(true);   // the firmware only talks to an open port (Linux sets DTR by itself, Windows not)
    port_->clear();
    const QString fw = identify();
    if (!fw.isEmpty()) {
        firmware_ = fw;
        stream_ = true;
        if (!configureStream(error)) {
            port_->close();
            port_.reset();
            stream_ = false;
            return false;
        }
        raw_.setFileName(w_->path(QStringLiteral("raw/usb_stream.bin")));
        if (!raw_.open(QIODevice::WriteOnly)) {
            if (error) *error = QStringLiteral("cannot write %1").arg(raw_.fileName());
            port_->close();
            port_.reset();
            stream_ = false;
            return false;
        }
        reader_ = PicoStreamReader();
        dec_ = std::make_unique<UsbDecoder>(c_.lowSpeed);
        haveTime_ = haveHello_ = ended_ = lostWarned_ = false;
        picoUs_ = helloUs_ = lastMs_ = offset_ = 0;
        offsets_.clear();
        status_ = {};
        port_->readAll();   // replies to the settings
        connect(port_.get(), &QSerialPort::readyRead, this, &PicoSource::streamData);
        port_->write("s");
        t0_ = wallNow();
        w_->event(t0_, QStringLiteral("status"), {{QStringLiteral("src"), id()},
                                                  {QStringLiteral("text"), QStringLiteral("USB recording started (%1 speed, %2: continuous)")
                                                                               .arg(c_.lowSpeed ? QStringLiteral("low") : QStringLiteral("full"), fw)}});
        watch_.start();
        return true;
    }
    firmware_ = QStringLiteral("usb-sniffer-lite");
    if (!configure(error)) {
        port_->close();
        port_.reset();
        return false;
    }
    t0_ = wallNow();
    text_ = command('s', 0.3, 1.0, 3);
    w_->event(t0_, QStringLiteral("status"), {{QStringLiteral("src"), id()},
                                              {QStringLiteral("text"), QStringLiteral("USB capture started (%1 speed)").arg(c_.lowSpeed ? QStringLiteral("low") : QStringLiteral("full"))}});
    watch_.start();
    return true;
}

void PicoSource::stop() {
    watch_.stop();
    if (!port_) return;
    if (stream_) {
        disconnect(port_.get(), &QSerialPort::readyRead, this, &PicoSource::streamData);
        port_->write("p");
        port_->waitForBytesWritten(500);
        QElapsedTimer clock;
        clock.start();
        while (!ended_ && clock.elapsed() < 3000) {   // the rest of the Pico's buffers, then its END record
            port_->waitForReadyRead(100);
            streamData();
        }
        streamEvents(dec_->take(0, true));
        QString s = QStringLiteral("USB stream: %1 frames, %2 idle packets dropped on the Pico, %3 packets recorded")
                        .arg(status_.value(QStringLiteral("frames")).toInteger()).arg(status_.value(QStringLiteral("dropped")).toInteger())
                        .arg(status_.value(QStringLiteral("sent")).toInteger());
        const qint64 lost = status_.value(QStringLiteral("lost_capture")).toInteger() + status_.value(QStringLiteral("lost_output")).toInteger();
        if (lost) s += QStringLiteral(", %1 lost (the sniffer's buffers were full)").arg(lost);
        if (!ended_) s += QStringLiteral("; the sniffer did not confirm the end");
        if (reader_.skipped) s += QStringLiteral("; %1 damaged bytes skipped").arg(reader_.skipped);
        emit recorded(w_->event(wallNow(), QStringLiteral("status"), {{QStringLiteral("src"), id()}, {QStringLiteral("text"), s}}));
        for (const QString& line : dec_->summary())
            emit recorded(w_->event(wallNow(), QStringLiteral("status"), {{QStringLiteral("src"), id()}, {QStringLiteral("text"), line}}));
        raw_.close();
    } else if (!full_) {
        text_ += QString::fromLatin1(port_->readAll());
        text_ += command('p', 1.5, 3.0, 120);
        decodeAndWrite(text_);
    }
    port_->close();
    port_.reset();
    w_ = nullptr;
}

QJsonObject PicoSource::eventFields(const UsbEvent& e, const UsbDecoder& dec) const {
    QJsonObject f{{QStringLiteral("src"), id()}, {QStringLiteral("what"), e.what}};
    if (e.addr >= 0) f.insert(QStringLiteral("addr"), e.addr);
    if (e.ep >= 0) f.insert(QStringLiteral("ep"), e.ep);
    if (!e.dir.isEmpty()) f.insert(QStringLiteral("dir"), e.dir == QLatin1String("IN") ? QStringLiteral("in") : QStringLiteral("out"));
    if (!e.data.isEmpty()) f.insert(QStringLiteral("hex"), QString::fromLatin1(e.data.toHex()));
    if (e.truncated) f.insert(QStringLiteral("truncated"), true);
    if (!e.setup.isEmpty()) f.insert(QStringLiteral("setup"), QString::fromLatin1(e.setup.toHex()));
    if (e.addr >= 0 && dec.devices.count(e.addr) && dec.devices.at(e.addr).vid >= 0)
        f.insert(QStringLiteral("dev"), QStringLiteral("%1:%2").arg(dec.devices.at(e.addr).vid, 4, 16, QLatin1Char('0')).arg(dec.devices.at(e.addr).pid, 4, 16, QLatin1Char('0')));
    f.insert(QStringLiteral("t_ms"), e.tMs);
    return f;
}

void PicoSource::decodeAndWrite(const QString& dump) {
    QFile raw(w_->path(QStringLiteral("raw/usb_cap.txt")));
    if (raw.open(QIODevice::WriteOnly)) raw.write(dump.toLatin1());
    // the packet list follows "Capture buffer:"; settings and messages before it are not packets
    const int at = dump.indexOf(QLatin1String("Capture buffer"));
    UsbDecoder dec(c_.lowSpeed);
    dec.feed(at >= 0 ? dump.mid(at) : dump);
    for (const UsbEvent& e : dec.events) {
        const bool payload = !e.control && e.ep > 0 && !e.data.isEmpty();
        if (payload) bytes_ += e.data.size();
        ++events_;
        emit recorded(w_->event(t0_ + e.tMs / 1000.0, payload ? QStringLiteral("data") : QStringLiteral("usb"), eventFields(e, dec)));
    }
    for (const QString& s : dec.summary()) emit recorded(w_->event(wallNow(), QStringLiteral("status"), {{QStringLiteral("src"), id()}, {QStringLiteral("text"), s}}));
}

// ---- streaming

void PicoSource::streamData() {
    if (!port_ || !w_) return;
    const QByteArray in = port_->readAll();
    if (!in.isEmpty()) {
        const double arrival = wallNow();
        raw_.write(in);
        for (const PicoRecord& r : reader_.feed(in)) streamRecord(r, arrival);
    }
    if (haveHello_) streamEvents(dec_->take(lastMs_));
}

void PicoSource::streamRecord(const PicoRecord& r, double arrival) {
    // the Pico's 32-bit microsecond timer, unwrapped (records come at least once a second)
    if (!haveTime_) {
        picoUs_ = r.time;
        haveTime_ = true;
    } else {
        picoUs_ += double(qint32(r.time - lastRaw_));
    }
    lastRaw_ = r.time;
    const double us = picoUs_;
    if (!haveHello_) {
        helloUs_ = us;
        haveHello_ = true;
    }
    const double ms = (us - helloUs_) / 1000.0;
    if (r.type == PicoRecord::Hello || r.type == PicoRecord::Status || r.type == PicoRecord::End || r.type == PicoRecord::Text) {
        // made just before they were sent: arrival - Pico time is the clock offset plus the link's delay; the smallest
        // value over the last 30 s is the best estimate (and follows the drift between the two clocks)
        offsets_.emplace_back(arrival, arrival - us / 1e6);
        while (offsets_.size() > 1 && arrival - offsets_.front().first > 30) offsets_.pop_front();
        offset_ = offsets_.front().second;
        for (const auto& o : offsets_) offset_ = std::min(offset_, o.second);
    }
    lastMs_ = std::max(lastMs_, ms);
    switch (r.type) {
    case PicoRecord::Packet:
        if (!r.payload.isEmpty()) dec_->packet(ms, uchar(r.payload[0]), r.payload.mid(1));
        break;
    case PicoRecord::Reset:
        dec_->reset(ms);
        break;
    case PicoRecord::Lost: {
        if (r.payload.size() < 8) break;
        const quint32 cap = le32(r.payload, 0), out = le32(r.payload, 4);
        const QString what = QStringLiteral("lost %1 packets: the sniffer's buffers were full (%2 not captured, %3 not sent)").arg(cap + out).arg(cap).arg(out);
        emit recorded(w_->event(streamWall(us), QStringLiteral("usb"), {{QStringLiteral("src"), id()}, {QStringLiteral("what"), what},
                                                                       {QStringLiteral("lost"), double(cap + out)}, {QStringLiteral("t_ms"), ms}}));
        if (!lostWarned_) {
            lostWarned_ = true;
            emit problem(QStringLiteral("The USB sniffer could not keep up: %1 packets lost (more traffic than its link carries).").arg(cap + out));
        }
        break;
    }
    case PicoRecord::Status:
    case PicoRecord::End:
        if (r.payload.size() >= 28) {
            status_ = {{QStringLiteral("frames"), double(le32(r.payload, 0))},
                       {QStringLiteral("dropped"), double(le32(r.payload, 6))},
                       {QStringLiteral("sent"), double(le32(r.payload, 10))},
                       {QStringLiteral("errors"), double(le32(r.payload, 14))},
                       {QStringLiteral("lost_capture"), double(le32(r.payload, 18))},
                       {QStringLiteral("lost_output"), double(le32(r.payload, 22))},
                       {QStringLiteral("ring_peak"), int(uchar(r.payload[26]))},
                       {QStringLiteral("out_peak"), int(uchar(r.payload[27]))}};
        }
        if (r.type == PicoRecord::End) ended_ = true;
        break;
    default:
        break;
    }
}

void PicoSource::streamEvents(std::vector<UsbEvent> events) {
    for (UsbEvent& e : events) {
        const bool payload = !e.control && e.ep > 0 && !e.data.isEmpty();
        if (payload) bytes_ += e.data.size();
        ++events_;
        emit recorded(w_->event(streamWall(helloUs_ + e.tMs * 1000.0), payload ? QStringLiteral("data") : QStringLiteral("usb"), eventFields(e, *dec_)));
    }
}

QJsonObject PicoSource::stats() const {
    QJsonObject o{{QStringLiteral("usb_events"), events_}, {QStringLiteral("usb_bytes"), double(bytes_)}};
    if (stream_) {
        o.insert(QStringLiteral("usb_mode"), QStringLiteral("stream"));
        o.insert(QStringLiteral("usb_frames"), status_.value(QStringLiteral("frames")));
        o.insert(QStringLiteral("usb_lost"), status_.value(QStringLiteral("lost_capture")).toDouble() + status_.value(QStringLiteral("lost_output")).toDouble());
    }
    return o;
}

}  // namespace lt
