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
    if (body == QLatin1String("--- RESET ---")) {
        flushAll();
        event(now(t), -1, -1, {}, QStringLiteral("bus reset"));
        toggle_.clear();
        return;
    }
    if (body.startsWith(QLatin1String("ERROR"))) {
        ++errors;
        event(now(t), -1, -1, {}, QStringLiteral("capture error: ") + body);
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
            event(now(tk.t), tk.addr, tk.ep, tk.kind, QStringLiteral("STALL"));
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
            c->t = now(tk.t);
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
    const double tn = now(tk.t);
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

// ---- the Pico as a source

namespace {
struct Want {
    char key;
    const char* label;
};
const Want kWant[] = {{'e', "Capture speed"}, {'g', "Capture trigger"}, {'l', "Capture limit"},
                      {'t', "Time display format"}, {'a', "Data display format"}, {'f', "Fold empty frames"}};

std::map<char, QString> parseSettings(const QString& text) {
    std::map<char, QString> got;
    for (const Want& w : kWant) {
        const QRegularExpression re(QStringLiteral("%1 - %2\\s*: ([^\r\n]*)").arg(QString(QLatin1Char(w.key)), QRegularExpression::escape(QLatin1String(w.label))));
        const auto m = re.match(text);
        if (m.hasMatch()) got[w.key] = m.captured(1).trimmed();
    }
    return got;
}
}  // namespace

PicoSource::PicoSource(const PicoConfig& c, QObject* parent) : Source(parent), c_(c) {
    watch_.setInterval(100);
    connect(&watch_, &QTimer::timeout, this, [this] {
        if (!port_ || full_) return;
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
    return {{QStringLiteral("id"), id()},
            {QStringLiteral("kind"), kind()},
            {QStringLiteral("label"), QStringLiteral("USB sniffer (Pico), %1 speed").arg(c_.lowSpeed ? QStringLiteral("low") : QStringLiteral("full"))},
            {QStringLiteral("port"), c_.port},
            {QStringLiteral("speed"), c_.lowSpeed ? QStringLiteral("low") : QStringLiteral("full")},
            {QStringLiteral("raw"), QStringLiteral("raw/usb_cap.txt")},
            {QStringLiteral("settings"), settings_}};
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

bool PicoSource::probe(const QString& port, QString* sentence) {
    PicoSource p(PicoConfig{port, false});
    p.port_ = std::make_unique<QSerialPort>();
    p.port_->setPortName(port);
    if (!p.port_->open(QIODevice::ReadWrite)) {
        if (sentence) *sentence = QStringLiteral("cannot open %1: %2").arg(port, p.port_->errorString());
        return false;
    }
    const auto got = parseSettings(p.command('h', 0.4, 2.0, 5));
    p.port_->close();
    if (got.empty()) {
        if (sentence) *sentence = QStringLiteral("%1 does not answer like usb-sniffer-lite").arg(port);
        return false;
    }
    if (sentence) *sentence = QStringLiteral("usb-sniffer-lite answers on %1 (%2 speed)").arg(port, got.count('e') ? got.at('e').toLower() : QStringLiteral("?"));
    return true;
}

bool PicoSource::start(SessionWriter* writer, QString* error) {
    w_ = writer;
    full_ = false;
    events_ = 0;
    bytes_ = 0;
    port_ = std::make_unique<QSerialPort>();
    port_->setPortName(c_.port);
    if (!port_->open(QIODevice::ReadWrite)) {
        if (error) *error = QStringLiteral("cannot open the USB sniffer on %1: %2").arg(c_.port, port_->errorString());
        port_.reset();
        return false;
    }
    port_->setBaudRate(115200);   // a USB CDC port: the rate does not matter
    port_->clear();
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
    if (!full_) {
        text_ += QString::fromLatin1(port_->readAll());
        text_ += command('p', 1.5, 3.0, 120);
        decodeAndWrite(text_);
    }
    port_->close();
    port_.reset();
    w_ = nullptr;
}

void PicoSource::decodeAndWrite(const QString& dump) {
    QFile raw(w_->path(QStringLiteral("raw/usb_cap.txt")));
    if (raw.open(QIODevice::WriteOnly)) raw.write(dump.toLatin1());
    // the packet list follows "Capture buffer:"; settings and messages before it are not packets
    const int at = dump.indexOf(QLatin1String("Capture buffer"));
    UsbDecoder dec(c_.lowSpeed);
    dec.feed(at >= 0 ? dump.mid(at) : dump);
    for (const UsbEvent& e : dec.events) {
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
        const bool payload = !e.control && e.ep > 0 && !e.data.isEmpty();
        if (payload) bytes_ += e.data.size();
        ++events_;
        emit recorded(w_->event(t0_ + e.tMs / 1000.0, payload ? QStringLiteral("data") : QStringLiteral("usb"), f));
    }
    for (const QString& s : dec.summary()) emit recorded(w_->event(wallNow(), QStringLiteral("status"), {{QStringLiteral("src"), id()}, {QStringLiteral("text"), s}}));
}

QJsonObject PicoSource::stats() const { return {{QStringLiteral("usb_events"), events_}, {QStringLiteral("usb_bytes"), double(bytes_)}}; }

}  // namespace lt
