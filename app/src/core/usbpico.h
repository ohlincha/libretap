#pragma once
// USB through a Raspberry Pi Pico (RP2040) running ataradov's usb-sniffer-lite, spliced into the instrument's USB
// cable (green D+ to GPIO 10, white D- to GPIO 11, black to GND; red stays on the cable). Low and full speed only
// (<= 12 Mbit/s). Ported from the lab's capture/usbsniff.py.
//
// The sniffer keeps 232 kB of packets, often well under a minute of traffic, so each LibreTap recording is one
// capture: press Record, do one action in the vendor software, press Stop. Stop asks the Pico for its buffer as a
// text dump, which is saved in raw/usb_cap.txt and decoded into events: control transfers by name (descriptors,
// CDC line coding, FTDI / CH34x / PL2303 / CP210x vendor requests) and bulk/interrupt data merged per endpoint (FTDI's
// two status bytes per IN packet removed). The first capture should include unplugging and replugging the
// instrument, so its enumeration (what it is, its endpoints) is seen.
//
// Times: the dump counts from the first frame after the capture started; event t = the moment Record started the
// capture + that offset (about ±50 ms: the Pico's start is not timestamped).
#include <QByteArray>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QTimer>

#include <array>
#include <map>
#include <memory>
#include <vector>

#include "source.h"

class QSerialPort;

namespace lt {

struct UsbEvent {
    double tMs = 0;                // since the first frame of the capture
    int addr = -1, ep = -1;        // -1: not about one endpoint (bus reset, capture error)
    QString dir;                   // IN (device → host), OUT (host → device), or empty
    QString what;                  // plain words: "GET_DESCRIPTOR device #0: …", "bulk 12 bytes", "bus reset"
    QByteArray data;
    bool control = false;          // a control transfer (ep 0)
    bool truncated = false;        // the sniffer showed only the first bytes (its display setting)
    QByteArray setup;              // control transfers: the 8 setup bytes
};

struct UsbDevice {
    int vid = -1, pid = -1, maxp0 = 0, deviceClass = -1;
    std::vector<std::array<int, 3>> ifaces;        // interface class/subclass/protocol from the configuration
    QString kindText() const;                      // "keyboard", "mouse", "storage", "hub", "serial", "vendor", …
    std::map<int, std::pair<QString, int>> eps;   // endpoint address (0x81 = ep 1 IN) → (kind, max packet)
};

// Turns usb-sniffer-lite's text dump into transfers (usbsniff.py's Decoder).
class UsbDecoder {
public:
    explicit UsbDecoder(bool lowSpeed = false, bool ftdi = false);
    UsbDecoder& feed(const QString& text);       // the whole dump; ends with finish()
    std::vector<UsbEvent> events;
    std::map<int, UsbDevice> devices;            // by address
    int errors = 0;
    bool truncated = false;
    QStringList summary() const;                 // devices, endpoints, bytes per endpoint, hints
    // A control transfer in words (also updates `devices` from descriptors and SET_ADDRESS).
    QString describeControl(int addr, int rt, int req, int val, int idx, int len, const QByteArray& data, bool stalled = false,
                            bool incomplete = false);

private:
    void line(int t, const QString& body);
    void handshake(const QString& hs);
    void controlDone(const struct UsbControl& c, bool stalled, bool incomplete);
    QString descriptor(int addr, int type, const QByteArray& d);
    void flush(const std::tuple<int, int, QString>& key);
    void flushAll();
    double now(int t) const;
    bool ftdiOn(int addr) const;
    int maxp(int addr, int ep, const QString& d) const;
    QString epKind(int addr, int ep, const QString& d) const;
    void event(double tMs, int addr, int ep, const QString& dir, const QString& what, const QByteArray& data = {},
               bool trunc = false, bool control = false);

    struct Token {
        QString kind;
        int addr = 0, ep = 0, t = 0;
    };
    struct Data {
        QString pid;
        QByteArray bytes;
        bool trunc = false;
    };
    struct Bulk {
        double t = 0, last = 0;
        QByteArray data;
        bool trunc = false;
    };
    int defaultMaxp_;
    bool ftdi_;
    bool haveFrame_ = false;
    long long frame_ = 0;
    int sofRaw_ = -1;
    long long lsFrames_ = 0;
    bool haveToken_ = false, haveData_ = false;
    Token token_;
    Data data_;
    std::map<int, std::shared_ptr<struct UsbControl>> control_;
    std::map<std::tuple<int, int, QString>, Bulk> bulk_;
    std::map<std::tuple<int, int, QString>, QString> toggle_;
};

QString ftdiBaudText(int value, int index);

struct PicoConfig {
    QString port;                  // the Pico's own serial port (/dev/ttyACM0, COM5)
    bool lowSpeed = false;         // the instrument is a low-speed device (keyboards, some HID)
};

// The Pico as a recording source: configures the sniffer, starts a capture at start(), fetches and decodes the
// dump at stop(). Talks to the Pico (it is our sniffer, not the instrument's link: writing to it is fine).
class PicoSource : public Source {
    Q_OBJECT
public:
    explicit PicoSource(const PicoConfig& c, QObject* parent = nullptr);
    ~PicoSource() override;
    QString id() const override { return QStringLiteral("usb"); }
    QString kind() const override { return QStringLiteral("usb-pico"); }
    QJsonObject describe() const override;
    bool start(SessionWriter* writer, QString* error) override;
    void stop() override;
    QJsonObject stats() const override;

    // Opens the port and checks that a usb-sniffer-lite answers (for Connections); false with a sentence.
    static bool probe(const QString& port, QString* sentence);

private:
    QString command(char c, double idle, double first, double limit, const QByteArray& until = {});
    bool configure(QString* error);
    void decodeAndWrite(const QString& dump);
    PicoConfig c_;
    std::unique_ptr<QSerialPort> port_;
    SessionWriter* w_ = nullptr;
    double t0_ = 0;
    QString text_;                 // what the Pico said since the capture started
    QTimer watch_;                 // buffer full while capturing?
    bool full_ = false;
    int events_ = 0;
    qint64 bytes_ = 0;
    QJsonObject settings_;
};

}  // namespace lt
