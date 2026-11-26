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
//
// With LibreTap's own firmware on the Pico (USB Sniffer Stream, firmware/pico-usb-sniffer/; it answers `v`), the
// recording is continuous instead: the Pico drops SOFs and NAKed polling itself and streams the rest as framed binary
// records (docs/FORMAT.md), kept in raw/usb_stream.bin; the same decoder turns them into the same events while
// recording. Times come from the Pico's microsecond timer, lined up with the laptop's clock from records the Pico
// sends as soon as it makes them (about ±2 ms). Losses (the Pico's buffers full) are events, never silent.
#include <QByteArray>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QTimer>

#include <array>
#include <deque>
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

    // Streaming (USB Sniffer Stream): one packet at a time with its own time (ms), bytes from the PID on and the
    // firmware's error flags as in its PACKET record; take() hands over the transfers completed so far (bulk and
    // interrupt data once a short packet or a 10 ms pause ends them; all = everything, at the end).
    void packet(double tMs, int flags, const QByteArray& bytes);
    void reset(double tMs);
    std::vector<UsbEvent> take(double nowMs, bool all = false);
    // A whole recorded stream (raw/usb_stream.bin) at once, like feed() for a text dump; times from its HELLO record
    UsbDecoder& feedStream(const QByteArray& bytes);
    qint64 lost = 0;               // packets the stream reported lost

private:
    void line(int t, const QString& body);
    void lineAt(double tMs, const QString& body);
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
        int addr = 0, ep = 0;
        double t = 0;              // ms
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

// The Connections sentence for a Pico that listPicos() found: its USB product string ("USB Sniffer Stream (RP2040)" or
// "USB Sniffer Lite (RP2040)") tells which firmware it runs
QString picoSentence(const QString& port, const QString& description);

// USB Sniffer Stream's records: 0xA5, type, length (2), time (4, µs), payload, CRC-16/CCITT-FALSE (docs/FORMAT.md)
struct PicoRecord {
    enum Type { Hello = 1, Packet, Reset, KeepAlive, Lost, Status, Text, End };
    int type = 0;
    quint32 time = 0;          // the Pico's microsecond timer (wraps after 71.6 minutes)
    QByteArray payload;
};

quint16 picoCrc16(const char* data, qsizetype size, quint16 crc = 0xffff);

// Cuts the byte stream into records; after damage or lost bytes it looks for the next 0xA5 whose header and CRC check out.
class PicoStreamReader {
public:
    std::vector<PicoRecord> feed(const QByteArray& bytes);
    qint64 skipped = 0;        // bytes that were not part of a good record
private:
    QByteArray buf_;
};

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

    // Opens the port and checks that a usb-sniffer-lite or USB Sniffer Stream answers (for Connections); false with
    // a sentence.
    static bool probe(const QString& port, QString* sentence);
    bool streaming() const { return stream_; }   // after start(): USB Sniffer Stream, continuous

private:
    QString command(char c, double idle, double first, double limit, const QByteArray& until = {});
    QString identify();            // "USB Sniffer Stream 1.0.0" (protocol 1) or empty: usb-sniffer-lite
    bool configure(QString* error);
    bool configureStream(QString* error);
    void decodeAndWrite(const QString& dump);
    QJsonObject eventFields(const UsbEvent& e, const UsbDecoder& dec) const;
    // streaming
    void streamData();
    void streamRecord(const PicoRecord& r, double arrival);
    void streamEvents(std::vector<UsbEvent> events);
    double streamWall(double picoUs) const { return offset_ + picoUs / 1e6; }
    bool stream_ = false;
    QString firmware_;
    PicoStreamReader reader_;
    std::unique_ptr<UsbDecoder> dec_;
    QFile raw_;
    bool haveTime_ = false, haveHello_ = false, ended_ = false, lostWarned_ = false;
    quint32 lastRaw_ = 0;
    double picoUs_ = 0, helloUs_ = 0, lastMs_ = 0;
    double offset_ = 0;
    std::deque<std::pair<double, double>> offsets_;   // (arrival, arrival - Pico time) of the last 30 s
    QJsonObject status_;           // the latest STATUS/END counters
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
