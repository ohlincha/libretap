#include "devices.h"

#include <QJsonArray>
#include <QSerialPortInfo>

#include "ethernet.h"

#include <algorithm>

namespace lt {

static QString chipName(int vid) {
    switch (vid) {
    case 0x0403: return QStringLiteral("FTDI");
    case 0x067b: return QStringLiteral("Prolific PL2303");
    case 0x10c4: return QStringLiteral("Silicon Labs CP210x");
    case 0x1a86: return QStringLiteral("WCH CH340");
    case 0x04d8: return QStringLiteral("Microchip");
    case 0x0557: return QStringLiteral("ATEN");
    case 0x2e8a: return QStringLiteral("Raspberry Pi Pico");
    default: return vid ? QStringLiteral("USB %1").arg(vid, 4, 16, QLatin1Char('0')) : QString();
    }
}

QString Adapter::label() const {
    QString d = description;
    if (d.isEmpty() || d == QLatin1String("n/a")) d = chip;
    return d.isEmpty() ? port : QStringLiteral("%1 (%2)").arg(port, d);
}

QJsonObject Adapter::toJson() const {
    return {{QStringLiteral("path"), path},       {QStringLiteral("port"), port},   {QStringLiteral("chip"), chip},
            {QStringLiteral("description"), description}, {QStringLiteral("manufacturer"), manufacturer},
            {QStringLiteral("serial"), serial},   {QStringLiteral("usb"), usb},
            {QStringLiteral("vid"), QStringLiteral("%1").arg(vid, 4, 16, QLatin1Char('0'))},
            {QStringLiteral("pid"), QStringLiteral("%1").arg(pid, 4, 16, QLatin1Char('0'))},
            {QStringLiteral("label"), label()}};
}

std::vector<Adapter> listAdapters(bool all) {
    std::vector<Adapter> out;
    for (const QSerialPortInfo& i : QSerialPortInfo::availablePorts()) {
        Adapter a;
        a.path = i.systemLocation();
        a.port = i.portName();
        a.usb = i.hasVendorIdentifier() && !a.port.startsWith(QLatin1String("ttyS"));   // ttyS*: built-in UARTs (PCI ids)
        a.vid = a.usb ? i.vendorIdentifier() : 0;
        a.pid = i.hasProductIdentifier() ? i.productIdentifier() : 0;
        a.chip = chipName(a.vid);
        a.description = i.description();
        a.manufacturer = i.manufacturer();
        a.serial = i.serialNumber();
        if (a.vid == 0x2e8a || a.description.contains(QLatin1String("niffer"))) continue;   // a Pico is the USB sniffer, not a tap line
        if (a.usb || all) out.push_back(a);
    }
    for (const QString& f : qEnvironmentVariable("LIBRETAP_FAKE_ADAPTERS").split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        Adapter a;
        a.path = f.section(QLatin1Char('='), 0, 0);
        a.port = a.path.section(QLatin1Char('/'), -2).remove(QStringLiteral("dev/"));
        a.description = f.section(QLatin1Char('='), 1);
        a.usb = true;
        out.push_back(a);
    }
    std::stable_sort(out.begin(), out.end(), [](const Adapter& x, const Adapter& y) {
        if (x.usb != y.usb) return x.usb;
        return x.port.size() != y.port.size() ? x.port.size() < y.port.size() : x.port < y.port;
    });
    return out;
}

std::vector<Adapter> listPicos() {
    std::vector<Adapter> out;
    for (const QSerialPortInfo& i : QSerialPortInfo::availablePorts()) {
        const bool pico = (i.hasVendorIdentifier() && i.vendorIdentifier() == 0x2e8a) || i.description().contains(QLatin1String("niffer"));
        if (!pico) continue;
        Adapter a;
        a.path = i.systemLocation();
        a.port = i.portName();
        a.usb = true;
        a.vid = i.vendorIdentifier();
        a.pid = i.productIdentifier();
        a.chip = QStringLiteral("Raspberry Pi Pico");
        a.description = i.description();
        out.push_back(a);
    }
    const QString fake = qEnvironmentVariable("LIBRETAP_FAKE_PICO");
    if (!fake.isEmpty()) {
        Adapter a;
        a.path = fake;
        a.port = fake.section(QLatin1Char('/'), -2).remove(QStringLiteral("dev/"));
        a.chip = QStringLiteral("Raspberry Pi Pico");
        a.description = QStringLiteral("usb-sniffer-lite");
        a.usb = true;
        out.push_back(a);
    }
    return out;
}

TapGuess guessTap(const std::vector<Adapter>& ad, const QString& preferA, const QString& preferB) {
    TapGuess g;
    auto has = [&](const QString& p) {
        return std::any_of(ad.begin(), ad.end(), [&](const Adapter& a) { return a.path == p; });
    };
    auto label = [&](const QString& p) {
        for (const Adapter& a : ad)
            if (a.path == p) return a.label();
        return p;
    };
    const int usb = int(std::count_if(ad.begin(), ad.end(), [](const Adapter& a) { return a.usb; }));
    if (!preferA.isEmpty() && !preferB.isEmpty() && preferA != preferB && has(preferA) && has(preferB)) {
        g.a = preferA, g.b = preferB;
    } else if (usb >= 2) {
        g.a = ad[0].path, g.b = ad[1].path;
    }
    if (!g.a.isEmpty()) {
        g.ready = true;
        g.sentence = QStringLiteral("Serial tap ready: PC side on %1, instrument side on %2.").arg(label(g.a), label(g.b));
        if (usb > 2) g.sentence += QStringLiteral(" (%1 adapters are plugged in; choose under Advanced if these are the wrong two.)").arg(usb);
    } else if (usb == 1) {
        g.sentence = QStringLiteral("One USB-serial adapter found (%1). The serial tap needs two: plug in the second one.").arg(ad[0].label());
    } else {
        g.sentence = QStringLiteral("No USB-serial adapters found. Plug the serial tap's two adapters into this computer.");
    }
    return g;
}

QJsonObject connectionsJson() {
    const auto ad = listAdapters();
    QJsonArray adapters, cameras;
    for (const Adapter& a : ad) adapters.append(a.toJson());
    const auto cams = Camera::list(false);
    for (const CameraInfo& c : cams) cameras.append(QJsonObject{{QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name}});
    const TapGuess g = guessTap(ad);
    const QString camSentence = cams.empty() ? QStringLiteral("No camera found (fine when Tick records the screen on the instrument PC).")
                                             : QStringLiteral("Camera: %1.").arg(cams.front().name);
    QJsonArray picos, ifaces;
    for (const Adapter& a : listPicos()) picos.append(a.toJson());
    for (const NetInterface& n : listInterfaces()) ifaces.append(QJsonObject{{QStringLiteral("name"), n.name}, {QStringLiteral("label"), n.label}, {QStringLiteral("wired"), n.wired}, {QStringLiteral("up"), n.up}});
    const CaptureAccess acc = captureAccess();
    QJsonArray sentences{g.sentence, camSentence};
    if (!picos.isEmpty())
        sentences.append(QStringLiteral("USB sniffer ready (Pico on %1). It captures in short pieces: press Record, do one action, press Stop.")
                             .arg(picos.first().toObject().value(QStringLiteral("port")).toString()));
    sentences.append(acc.sentence);
    return {{QStringLiteral("adapters"), adapters},
            {QStringLiteral("cameras"), cameras},
            {QStringLiteral("usb_sniffers"), picos},
            {QStringLiteral("interfaces"), ifaces},
            {QStringLiteral("ethernet"), QJsonObject{{QStringLiteral("ok"), acc.ok}, {QStringLiteral("dumpcap"), acc.program}, {QStringLiteral("via_sg"), acc.viaSg}}},
            {QStringLiteral("tap"), QJsonObject{{QStringLiteral("ready"), g.ready}, {QStringLiteral("a"), g.a}, {QStringLiteral("b"), g.b}}},
            {QStringLiteral("sentences"), sentences}};
}

}  // namespace lt
