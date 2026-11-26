#include "serialtap.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QSerialPort>

#include "session.h"
#include "util.h"

namespace lt {

QString LineFormat::text() const { return QStringLiteral("%1%2%3").arg(bits).arg(QLatin1Char(parity)).arg(stop); }

bool LineFormat::parse(const QString& t, LineFormat* out) {
    const QString s = t.trimmed().toUpper();
    if (s.size() != 3 || !QStringLiteral("5678").contains(s[0]) || !QStringLiteral("NEO").contains(s[1]) ||
        !QStringLiteral("12").contains(s[2]))
        return false;
    out->bits = s[0].digitValue();
    out->parity = s[1].toLatin1();
    out->stop = s[2].digitValue();
    return true;
}

double LineFormat::byteSeconds() const {
    return (1 + bits + (parity == 'N' ? 0 : 1) + stop) / double(std::max(1, baud));
}

static bool configure(QSerialPort* p, const LineFormat& f) {
    bool ok = p->setBaudRate(f.baud);
    ok &= p->setDataBits(QSerialPort::DataBits(f.bits));
    ok &= p->setParity(f.parity == 'E' ? QSerialPort::EvenParity : f.parity == 'O' ? QSerialPort::OddParity : QSerialPort::NoParity);
    ok &= p->setStopBits(f.stop == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop);
    ok &= p->setFlowControl(QSerialPort::NoFlowControl);
    return ok;
}

// Read-only, no flow control, nothing written.
static std::unique_ptr<QSerialPort> openListening(const QString& path, const LineFormat& f, QString* error) {
    auto p = std::make_unique<QSerialPort>();
    p->setPortName(path);
    if (!p->open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1: %2").arg(path, p->errorString());
        return nullptr;
    }
    if (!configure(p.get(), f)) {
        if (error) *error = QStringLiteral("%1 does not accept %2 baud %3").arg(path).arg(f.baud).arg(f.text());
        return nullptr;
    }
    p->clear(QSerialPort::Input);
    return p;
}

SerialTapSource::SerialTapSource(const TapConfig& c, QObject* parent) : Source(parent), c_(c), chunker_(c.gapMs / 1000.0) {
    poll_.setInterval(5);
    connect(&poll_, &QTimer::timeout, this, [this] { write(chunker_.poll(wallNow())); });
}

SerialTapSource::~SerialTapSource() { closeLines(); }

QString SerialTapSource::label() const {
    return QStringLiteral("Serial tap %1 baud %2").arg(c_.line.baud).arg(c_.line.text());
}

QJsonObject SerialTapSource::describe() const {
    return {{QStringLiteral("id"), id()},
            {QStringLiteral("kind"), kind()},
            {QStringLiteral("label"), label()},
            {QStringLiteral("names"), QJsonObject{{QStringLiteral("a"), c_.nameA}, {QStringLiteral("b"), c_.nameB}}},
            {QStringLiteral("ports"), QJsonObject{{QStringLiteral("a"), c_.portA}, {QStringLiteral("b"), c_.portB}}},
            {QStringLiteral("baud"), c_.line.baud},
            {QStringLiteral("format"), c_.line.text()},
            {QStringLiteral("gap_ms"), c_.gapMs},
            {QStringLiteral("raw"), QJsonObject{{QStringLiteral("a"), QStringLiteral("raw/a.bin")}, {QStringLiteral("b"), QStringLiteral("raw/b.bin")}}}};
}

bool SerialTapSource::openLines(QString* error) {
    if (c_.portA.isEmpty() || c_.portB.isEmpty()) {
        if (error) *error = QStringLiteral("the serial tap needs two ports (PC side and instrument side)");
        return false;
    }
    if (c_.portA == c_.portB) {
        if (error) *error = QStringLiteral("the PC side and the instrument side are the same port (%1)").arg(c_.portA);
        return false;
    }
    const QString paths[2] = {c_.portA, c_.portB};
    for (int d = 0; d < 2; ++d) {
        port_[d] = openListening(paths[d], c_.line, error);
        if (!port_[d]) {
            closeLines();
            return false;
        }
        QSerialPort* p = port_[d].get();
        connect(p, &QSerialPort::readyRead, this, [this, d, p] { bytesIn(d, p->readAll()); });
        connect(p, &QSerialPort::errorOccurred, this, [this, d, p](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError)   // unplugged
                emit problem(QStringLiteral("%1 side (%2) stopped working: %3").arg(d ? c_.nameB : c_.nameA, p->portName(), p->errorString()));
        });
    }
    return true;
}

void SerialTapSource::closeLines() {
    for (auto& p : port_)
        if (p) {
            p->disconnect(this);
            p->close();
            p.reset();
        }
}

bool SerialTapSource::start(SessionWriter* writer, QString* error) {
    w_ = writer;
    chunker_ = Chunker(c_.gapMs / 1000.0);
    bytes_[0] = bytes_[1] = 0;
    chunks_ = 0;
    if (!openLines(error)) return false;
    static const char* const names[2] = {"raw/a.bin", "raw/b.bin"};
    for (int d = 0; d < 2; ++d) {
        raw_[d].setFileName(w_->path(QLatin1String(names[d])));
        raw_[d].open(QIODevice::WriteOnly | QIODevice::Append);
    }
    const double now = wallNow();
    w_->event(now, QStringLiteral("config"), {{QStringLiteral("src"), id()}, {QStringLiteral("baud"), c_.line.baud},
                                              {QStringLiteral("bits"), c_.line.bits}, {QStringLiteral("parity"), QString(QLatin1Char(c_.line.parity))},
                                              {QStringLiteral("stop"), c_.line.stop}});
    for (const QString& p : {c_.portA, c_.portB})
        if (const QString h = latencyHint(p); !h.isEmpty())
            emit recorded(w_->event(now, QStringLiteral("status"), {{QStringLiteral("text"), h}}));
    poll_.start();
    return true;
}

void SerialTapSource::bytesIn(int dir, const QByteArray& data) {
    if (!w_ || data.isEmpty()) return;
    raw_[dir].write(data);
    raw_[dir].flush();
    bytes_[dir] += data.size();
    write(chunker_.feed(dir, data, wallNow()));
}

void SerialTapSource::write(const std::vector<Chunk>& chunks) {
    for (const Chunk& c : chunks) {
        ++chunks_;
        emit recorded(w_->event(c.t, QStringLiteral("data"),
                                {{QStringLiteral("src"), id()},
                                 {QStringLiteral("dir"), c.dir ? QStringLiteral("b") : QStringLiteral("a")},
                                 {QStringLiteral("hex"), QString::fromLatin1(c.data.toHex())},
                                 {QStringLiteral("end"), c.end}}));
    }
}

void SerialTapSource::flush() {
    if (w_) write(chunker_.flush());
}

void SerialTapSource::stop() {
    poll_.stop();
    if (w_) {
        // bytes still in the ports' buffers
        for (int d = 0; d < 2; ++d)
            if (port_[d] && port_[d]->bytesAvailable()) bytesIn(d, port_[d]->readAll());
        flush();
    }
    closeLines();
    for (QFile& f : raw_) f.close();
    w_ = nullptr;
}

QJsonObject SerialTapSource::stats() const {
    return {{QStringLiteral("bytes_a"), double(bytes_[0])}, {QStringLiteral("bytes_b"), double(bytes_[1])},
            {QStringLiteral("chunks"), chunks_}};
}

QString latencyHint(const QString& port) {
#ifdef __linux__
    const QString dev = QFileInfo(QFileInfo(port).canonicalFilePath()).fileName();
    QFile f(QStringLiteral("/sys/bus/usb-serial/devices/%1/latency_timer").arg(dev));
    if (dev.isEmpty() || !f.open(QIODevice::ReadOnly)) return {};
    const int ms = f.readAll().trimmed().toInt();
    if (ms > 1)
        return QStringLiteral("%1 holds bytes for %2 ms (FTDI latency timer); for sharper timing run: echo 1 | sudo tee %3")
            .arg(dev).arg(ms).arg(f.fileName());
#else
    (void)port;
#endif
    return {};
}

const std::vector<int>& commonBauds() {
    static const std::vector<int> b{300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    return b;
}

QJsonObject BaudGuess::toJson() const {
    QJsonArray r;
    for (const Row& x : rows)
        r.append(QJsonObject{{QStringLiteral("baud"), x.baud}, {QStringLiteral("bytes"), double(x.bytes)},
                             {QStringLiteral("text_fraction"), x.text}, {QStringLiteral("sample"), textView(x.sample)}});
    return {{QStringLiteral("baud"), baud}, {QStringLiteral("sure"), sure}, {QStringLiteral("sentence"), sentence},
            {QStringLiteral("rates"), r}};
}

BaudGuess guessBaud(const QStringList& ports, double seconds, const std::function<void(int)>& progress) {
    BaudGuess g;
    std::vector<std::unique_ptr<QSerialPort>> open;
    LineFormat f;
    QString err;
    for (const QString& p : ports) {
        auto sp = openListening(p, f, &err);
        if (!sp) {
            g.sentence = err;
            return g;
        }
        open.push_back(std::move(sp));
    }
    for (int b : commonBauds()) {
        if (progress) progress(b);
        f.baud = b;
        QByteArray data;
        for (auto& p : open) {
            configure(p.get(), f);
            p->clear(QSerialPort::Input);
        }
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < seconds * 1000) {
            QEventLoop loop;
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
            loop.exec();
            for (auto& p : open) data += p->readAll();
        }
        qint64 text = 0;
        for (char c : data) {
            const unsigned char x = static_cast<unsigned char>(c);
            if ((x >= 32 && x < 127) || x == 9 || x == 10 || x == 13) ++text;
        }
        g.rows.push_back({b, data.size(), data.isEmpty() ? 0.0 : double(text) / data.size(), data.left(40)});
    }
    const BaudGuess::Row* best = nullptr;
    for (const auto& r : g.rows) {
        const double score = r.bytes >= 8 ? r.text : -1;
        if (!best || score > (best->bytes >= 8 ? best->text : -1) ||
            (score == (best->bytes >= 8 ? best->text : -1) && r.bytes > best->bytes))
            best = &r;
    }
    if (best && best->bytes >= 8 && best->text > 0.9) {
        g.baud = best->baud;
        g.sure = true;
        g.sentence = QStringLiteral("The link talks at %1 baud (readable text, 8N1 assumed; try 7E1 if odd characters appear).").arg(g.baud);
    } else if (best && best->bytes >= 8) {
        g.baud = best->baud;
        g.sentence = QStringLiteral("No clear text at any speed: the protocol may be binary. %1 baud is the best guess; "
                                    "check the vendor software's settings if you can.").arg(g.baud);
    } else {
        g.sentence = QStringLiteral("Nothing arrived. Let the vendor software talk to the instrument while the speed is being found.");
    }
    return g;
}

}  // namespace lt
