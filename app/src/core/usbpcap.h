#pragma once
// Windows USBPcap captures (pcap/pcapng, link type 249), e.g. made with Wireshark on the instrument PC, read into a
// session: control transfers named as the Pico decoder names them (usbpico.h), bulk and interrupt transfers as data
// per endpoint, each with the device's address and USB id.
//
// A capture holds every device on the root hub: keyboard (passwords typed!), mouse, USB sticks. Only devices chosen
// by USB id or address are kept (hubs on request); the rest is counted, never written.
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <set>

namespace lt {

class SessionWriter;

struct UsbPcapFilter {
    QStringList ids;               // "ba8c:1688" (vid:pid, hex)
    std::set<int> addresses;       // device addresses
    bool hubs = false;             // also keep hubs (port status traffic; shows devices being powered)
    bool all = false;              // everything (private data!)
};

struct UsbPcapDevice {
    int bus = 0, addr = 0;
    QString id;                    // vid:pid, empty when the capture has no descriptor for it
    QString kind;                  // keyboard, mouse, storage, hub, HID, vendor-specific, USB-serial (…)
    int transfers = 0;
    qint64 bytes = 0;
    bool kept = false;
    QJsonObject toJson() const;
};

struct UsbPcapResult {
    std::vector<UsbPcapDevice> devices;
    int written = 0, dropped = 0;
    double t0 = 0, t1 = 0;
};

// Reads the devices only (to choose a filter).
bool scanUsbPcap(const QString& file, UsbPcapResult* out, QString* error);
// Writes the chosen devices' transfers into the session (source id "usb", kind "usbpcap").
bool importUsbPcap(const QString& file, SessionWriter* w, const UsbPcapFilter& f, UsbPcapResult* out, QString* error);

}  // namespace lt
