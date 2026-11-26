#pragma once
// Protocol description files: what is known about an instrument's messages, in a small JSON file anyone can read,
// write and share (docs/PROTOCOL-FORMAT.md). LibreTap uses it to say each message in a sentence, and to show how
// much of a recording is explained (coverage) and what is not yet.
//
// {
//   "format": "libretap-protocol", "version": 1,
//   "name": "DEMO-METER", "notes": "…",
//   "messages": [
//     {"name": "set integration time", "dir": "to-instrument",
//      "text": "^SET INT (\\d+)\\r$",
//      "fields": [{"name": "time", "group": 1, "type": "int", "scale": 0.001, "unit": "s"}],
//      "say": "Set the integration time to {time}"},
//     {"name": "exposure", "usb": {"bRequest": "0xAE", "bmRequestType": "0x40"}, "device": "ba8c:1688",
//      "fields": [{"name": "exposure", "offset": 0, "length": 4, "type": "u32le", "add": 1, "scale": 16e-6, "unit": "s"}],
//      "say": "Spectrometer: exposure {exposure}"},
//     {"name": "status report", "hex": "80 ?? ?? 00", "fields": [{"name": "inputs", "offset": 10, "type": "u8", "format": "bits"}]}
//   ]
// }
// Matching: "dir" (to-instrument, from-instrument, any), "device" (vid:pid of USB events), "source" (session source
// kind), "ep"; then one of "text" (regular expression on the message as Latin-1 text), "hex" (byte pattern from the
// start, ?? = any byte; "…" or a trailing "*" allows more bytes) or "usb" (setup fields of a control transfer).
// Field values: text groups (int, float, hex, text) or bytes at an offset (u8, i8, u16le/be, i16le/be, u32le/be,
// i32le/be, ascii, hex), or "from": "wValue" / "wIndex" / "wLength" / "bRequest" for USB control transfers.
// value = raw × scale + add, then unit; "values": {"9": "laser ON"} names raw values; "format": "hex" / "bits".
// Fields of a data payload count as explained; a "text" or "hex" match explains the whole message.
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>

#include <map>
#include <vector>

#include "project.h"
#include "timeline.h"

namespace lt {

struct ProtoField {
    QString name, type = QStringLiteral("int"), from, unit, format;
    int group = -1, offset = -1, length = 0;
    double scale = 1, add = 0;
    QJsonObject values;
};

struct ProtoMessage {
    QString name, say, dir = QStringLiteral("any"), device, source;
    int ep = -1;
    bool hasText = false, hasHex = false, hasUsb = false, open = false;
    QRegularExpression text;
    QByteArray pattern, mask;      // hex: bytes and which of them must match
    std::vector<std::pair<QString, int>> usb;   // setup field name → value
    std::vector<ProtoField> fields;
};

struct Decoded {
    int message = -1;              // index in Protocol::messages, -1 = not explained
    QString sentence;
    int explained = 0, total = 0;  // bytes
};

class Protocol {
public:
    QString name, notes, file;
    std::vector<ProtoMessage> messages;
    bool load(const QString& path, QString* error);
    bool parse(const QJsonObject& o, QString* error);
    Decoded decode(const Project& p, const TItem& it, const Timeline& tl) const;
};

struct Coverage {
    qint64 explained = 0, total = 0;
    int messages = 0, matched = 0;
    std::map<QString, std::pair<int, qint64>> unexplained;   // family → (messages, bytes)
    double percent() const { return total ? 100.0 * explained / total : 0; }
    QJsonObject toJson() const;
};

}  // namespace lt
