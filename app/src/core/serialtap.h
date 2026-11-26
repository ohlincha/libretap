#pragma once
// The serial tap: a passive two-lead DB-9 tap on the cable between the old PC and the instrument
// (../../knowledge/lab/protocol-capture.md in the lab repo). Each lead goes to its own USB-RS232 adapter on this
// computer: line a = the lead carrying the PC's TX (DB-9 pin 3), line b = the instrument's TX (pin 2).
// The adapters only ever listen: ports are opened read-only and nothing is written or set on the modem lines.
// (The OS may raise DTR when a port opens; the tap's monitor plugs wire only RX and ground, so nothing reaches
// the link.)
#include <QFile>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

#include "chunker.h"
#include "source.h"

class QSerialPort;

namespace lt {

struct LineFormat {
    int baud = 9600;
    int bits = 8;
    char parity = 'N';             // N, E, O
    int stop = 1;
    QString text() const;          // "8N1"
    static bool parse(const QString& text, LineFormat* out);   // "8N1", "7E1", …
    double byteSeconds() const;    // time of one character on the wire
};

struct TapConfig {
    QString portA, portB;          // device paths (/dev/ttyUSB0, COM3, a pseudo-terminal in tests)
    QString nameA = QStringLiteral("PC"), nameB = QStringLiteral("Instrument");
    LineFormat line;
    double gapMs = 20;             // silence that ends a chunk
};

class SerialTapSource : public Source {
    Q_OBJECT
public:
    explicit SerialTapSource(const TapConfig& c, QObject* parent = nullptr);
    ~SerialTapSource() override;
    QString id() const override { return QStringLiteral("tap"); }
    QString kind() const override { return QStringLiteral("serial-tap"); }
    QJsonObject describe() const override;
    bool start(SessionWriter* writer, QString* error) override;
    void stop() override;
    void flush() override;
    QJsonObject stats() const override;
    const TapConfig& config() const { return c_; }

protected:
    void bytesIn(int dir, const QByteArray& data);   // bytes that arrived on a line now
    virtual bool openLines(QString* error);
    virtual void closeLines();
    virtual QString label() const;
    TapConfig c_;

private:
    void write(const std::vector<Chunk>& chunks);
    Chunker chunker_;
    SessionWriter* w_ = nullptr;
    std::unique_ptr<QSerialPort> port_[2];
    QFile raw_[2];
    QTimer poll_;
    qint64 bytes_[2] = {0, 0};
    int chunks_ = 0;
};

// FTDI adapters hold received bytes up to 16 ms by default, which blurs the timing between the two lines. Returns
// a sentence with the fix when the adapter at `port` does that (Linux; empty elsewhere or when it is fine).
QString latencyHint(const QString& port);

// Finds the baud rate by listening at each common rate in turn while the link is busy and scoring how much of what
// arrives looks like text (serialtap.py guess). Binary protocols score low everywhere: then `sure` is false.
struct BaudGuess {
    int baud = 0;
    bool sure = false;
    QString sentence;              // plain words for the operator
    QJsonObject toJson() const;
    struct Row { int baud; qint64 bytes; double text; QByteArray sample; };
    std::vector<Row> rows;
};
// Listens `seconds` per rate on all the ports together (both tap lines). Runs an event loop; progress gets the
// rate being tried.
BaudGuess guessBaud(const QStringList& ports, double seconds, const std::function<void(int baud)>& progress = {});
const std::vector<int>& commonBauds();

}  // namespace lt
