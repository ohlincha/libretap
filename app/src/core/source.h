#pragma once
// A source: something the recorder listens to while a session is recorded. Each writes its own events into the
// session (and files into frames/ or raw/) and describes itself for session.json's "sources".
//
// Version 0.1: the serial tap (serialtap.h), its demo (demo.h) and the camera (camerasource.h).
// Phase 2 fits the same shape: the Pico usb-sniffer-lite ("usb-pico": a serial port to the Pico, decoded
// packets as events, the raw text dump in raw/) and Ethernet ("ethernet": dumpcap writing raw/*.pcap, per-packet
// summaries as events). A source that needs a helper process (dumpcap) starts and stops it in start()/stop().
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace lt {

class SessionWriter;

class Source : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~Source() override = default;
    virtual QString id() const = 0;                 // "tap", "camera", …: the events' "src"
    virtual QString kind() const = 0;               // serial-tap, camera, usb-pico, ethernet (FORMAT.md)
    virtual QJsonObject describe() const = 0;       // the session.json entry (id, kind, label, …)
    virtual bool start(SessionWriter* writer, QString* error) = 0;
    virtual void stop() = 0;
    virtual void flush() {}                         // write what is pending now (before a mark or a sync)
    virtual QJsonObject stats() const { return {}; }

signals:
    void recorded(const QJsonObject& event);        // an event just written (for live views)
    void problem(const QString& text);              // also written as an "error" event by the recorder
};

}  // namespace lt
