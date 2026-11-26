// LibreTapCLI: record without the window, control a running LibreTap (GUI or headless), read recordings.
// Every command takes --json for machine-readable output (one JSON object; `record --json` prints one per event).
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>

#include "core/camerasource.h"
#include "core/control.h"
#include "core/devices.h"
#include "core/ethernet.h"
#include "core/pcap.h"
#include "core/usbpico.h"
#include "core/match.h"
#include "core/project.h"
#include "core/recorder.h"
#include "core/serialtap.h"
#include "core/timeline.h"
#include "core/util.h"

using namespace lt;

namespace {

const char* const kUsage = R"(LibreTapCLI — record what old vendor software says to its instrument.

  LibreTapCLI record [--a PORT --b PORT] [--baud N|auto] [--format 8N1] [--gap MS] [--names PC INSTR]
                     [--camera ID|auto|none] [--every S] [--demo] [--out DIR] [--name NAME] [--note TEXT]
                     [--usb-pico PORT [--usb-speed full|low]] [--ethernet IFACE|auto [--instrument-ip IP]]
                     [--seconds S]           record until `stop`, Ctrl-C or --seconds (no window needed)
                                             (the tap is used when --a/--b are given, or when nothing else is)
  LibreTapCLI status                         the running LibreTap (window or record): recording? counts
  LibreTapCLI start                          start recording in the running window (its connections)
  LibreTapCLI mark [NOTE]                    mark this moment (what you are about to change)
  LibreTapCLI sync                           a manual clock sync (press Sync in Tick at the same moment)
  LibreTapCLI stop                           stop recording
  LibreTapCLI ports [--all]                  what is plugged in: serial adapters, cameras, in sentences
  LibreTapCLI guess PORT [PORT]              find the baud rate while the link is busy
  LibreTapCLI sessions                       recordings in the workspace (~/LibreTap or $LIBRETAP_HOME)
  LibreTapCLI show REC [--hex] [--frames]    a recording as text (REC: folder, name, or "latest")
  LibreTapCLI match A B [--save]             clock offset and drift of session B against session A
  LibreTapCLI import REC SESSION             add a session folder (e.g. from Tick) to a recording and match it
  LibreTapCLI decode-usb DUMP [--low] [--ftdi]   decode a usb-sniffer-lite text dump into transfers
  LibreTapCLI pcap FILE [--ip IP]            a pcap/pcapng file as conversations and packet summaries

Add --json to any command for JSON output. The window and `record` listen on a local socket (libretap-<user>,
or $LIBRETAP_CONTROL), so status/start/mark/sync/stop reach whichever is running.
)";

bool gJson = false;

void out(const QString& s) {
    std::fputs(s.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void outJson(const QJsonObject& o) { out(QString::fromUtf8(QJsonDocument(o).toJson(gJson ? QJsonDocument::Compact : QJsonDocument::Indented))); }

int fail(const QString& text, int code = 1) {
    if (gJson) outJson({{QStringLiteral("ok"), false}, {QStringLiteral("error"), text}});
    else std::fprintf(stderr, "%s\n", text.toUtf8().constData());
    return code;
}

std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

QString newestProject(const QString& ws) {
    QDir d(ws);
    QStringList names = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (int i = names.size() - 1; i >= 0; --i)
        if (QFileInfo::exists(d.filePath(names[i] + QStringLiteral("/project.json")))) return d.filePath(names[i]);
    return {};
}

// A folder, a name in the workspace (project or session inside one), or "latest".
QString resolve(const QString& x) {
    if (QFileInfo(x).isDir()) return QFileInfo(x).absoluteFilePath();
    const QString ws = workspaceDir();
    if (x == QLatin1String("latest")) return newestProject(ws);
    if (QFileInfo(QDir(ws).filePath(x)).isDir()) return QDir(ws).filePath(x);
    for (const QString& p : QDir(ws).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo(QDir(ws).filePath(p + QLatin1Char('/') + x)).isDir()) return QDir(ws).filePath(p + QLatin1Char('/') + x);
    return {};
}

QString describeEventLine(const QJsonObject& e, double t0) {
    const QString kind = e.value(QStringLiteral("kind")).toString();
    const double rel = e.value(QStringLiteral("t")).toDouble() - t0;
    if (kind == QLatin1String("data")) {
        const QByteArray b = QByteArray::fromHex(e.value(QStringLiteral("hex")).toString().toLatin1());
        return QStringLiteral("%1  %2  %3").arg(rel, 9, 'f', 3).arg(e.value(QStringLiteral("dir")).toString() == QLatin1String("a") ? QStringLiteral("PC>INSTR") : QStringLiteral("INSTR>PC"), textView(b));
    }
    if (kind == QLatin1String("mark"))
        return QStringLiteral("--- mark %1 at %2 s  %3").arg(e.value(QStringLiteral("n")).toInt()).arg(rel, 0, 'f', 3).arg(e.value(QStringLiteral("note")).toString());
    if (kind == QLatin1String("sync")) return QStringLiteral("--- sync %1 at %2 s").arg(e.value(QStringLiteral("n")).toInt()).arg(rel, 0, 'f', 3);
    if (kind == QLatin1String("frame")) return {};
    return QStringLiteral("    (%1) %2").arg(kind, e.value(QStringLiteral("text")).toString());
}

int cmdRecord(const QStringList& args) {
    QCommandLineParser p;
    p.addOptions({{QStringLiteral("a"), QStringLiteral("PC side port"), QStringLiteral("port")},
                  {QStringLiteral("b"), QStringLiteral("instrument side port"), QStringLiteral("port")},
                  {QStringLiteral("baud"), QStringLiteral("baud rate or auto"), QStringLiteral("n"), QStringLiteral("9600")},
                  {QStringLiteral("format"), QStringLiteral("frame format"), QStringLiteral("f"), QStringLiteral("8N1")},
                  {QStringLiteral("gap"), QStringLiteral("ms of silence that ends a chunk"), QStringLiteral("ms"), QStringLiteral("20")},
                  {QStringLiteral("names"), QStringLiteral("names of the two sides"), QStringLiteral("a,b")},
                  {QStringLiteral("camera"), QStringLiteral("camera id, auto or none"), QStringLiteral("id"), QStringLiteral("none")},
                  {QStringLiteral("every"), QStringLiteral("seconds between stills"), QStringLiteral("s"), QStringLiteral("0.5")},
                  {QStringLiteral("demo"), QStringLiteral("a pretend instrument")},
                  {QStringLiteral("no-tap"), QStringLiteral("camera only")},
                  {QStringLiteral("out"), QStringLiteral("workspace"), QStringLiteral("dir")},
                  {QStringLiteral("name"), QStringLiteral("added to the folder name"), QStringLiteral("name")},
                  {QStringLiteral("note"), QStringLiteral("session note"), QStringLiteral("text")},
                  {QStringLiteral("seconds"), QStringLiteral("stop after"), QStringLiteral("s")},
                  {QStringLiteral("hex"), QStringLiteral("hex under each chunk")},
                  {QStringLiteral("usb-pico"), QStringLiteral("the Pico USB sniffer's port"), QStringLiteral("port")},
                  {QStringLiteral("usb-speed"), QStringLiteral("full or low"), QStringLiteral("speed"), QStringLiteral("full")},
                  {QStringLiteral("ethernet"), QStringLiteral("network interface or auto"), QStringLiteral("iface")},
                  {QStringLiteral("instrument-ip"), QStringLiteral("only traffic to/from this address"), QStringLiteral("ip")},
                  {QStringLiteral("json"), QStringLiteral("JSON")}});
    if (!p.parse(QStringList{QStringLiteral("record")} + args)) return fail(p.errorText(), 2);
    RecordOptions o;
    o.demo = p.isSet(QStringLiteral("demo"));
    o.usb = p.isSet(QStringLiteral("usb-pico"));
    o.pico.port = p.value(QStringLiteral("usb-pico"));
    o.pico.lowSpeed = p.value(QStringLiteral("usb-speed")) == QLatin1String("low");
    o.ethernet = p.isSet(QStringLiteral("ethernet"));
    o.eth.iface = p.value(QStringLiteral("ethernet")) == QLatin1String("auto") ? QString() : p.value(QStringLiteral("ethernet"));
    o.eth.instrumentIp = p.value(QStringLiteral("instrument-ip"));
    // the serial tap: when its ports are given, or when nothing else is asked for
    o.tap = !p.isSet(QStringLiteral("no-tap")) && (p.isSet(QStringLiteral("a")) || (!o.usb && !o.ethernet));
    o.workspace = p.value(QStringLiteral("out"));
    o.name = p.value(QStringLiteral("name"));
    o.note = p.value(QStringLiteral("note"));
    o.stillEvery = p.value(QStringLiteral("every")).toDouble();
    TapConfig& tc = o.tapConfig;
    tc.gapMs = p.value(QStringLiteral("gap")).toDouble();
    if (!LineFormat::parse(p.value(QStringLiteral("format")), &tc.line)) return fail(QStringLiteral("frame format like 8N1 or 7E1 expected"), 2);
    if (p.isSet(QStringLiteral("names"))) {
        const QStringList n = p.value(QStringLiteral("names")).split(QLatin1Char(','));
        if (n.size() != 2) return fail(QStringLiteral("--names PC,INSTR"), 2);
        tc.nameA = n[0], tc.nameB = n[1];
    }
    if (o.usb && o.pico.port == QLatin1String("auto")) {
        const auto picos = listPicos();
        if (picos.empty()) return fail(QStringLiteral("no USB sniffer (Pico) found"));
        o.pico.port = picos.front().path;
    }
    if (o.tap && !o.demo) {
        const TapGuess g = guessTap(listAdapters());
        tc.portA = p.isSet(QStringLiteral("a")) ? p.value(QStringLiteral("a")) : g.a;
        tc.portB = p.isSet(QStringLiteral("b")) ? p.value(QStringLiteral("b")) : g.b;
        if (tc.portA.isEmpty() || tc.portB.isEmpty()) return fail(g.sentence + QStringLiteral(" (or give --a and --b, or --demo)"));
        if (p.value(QStringLiteral("baud")) == QLatin1String("auto")) {
            if (!gJson) out(QStringLiteral("finding the baud rate (keep the link busy)…"));
            const BaudGuess bg = guessBaud({tc.portA, tc.portB}, 2.0);
            if (!gJson) out(bg.sentence);
            if (!bg.baud) return fail(bg.sentence);
            tc.line.baud = bg.baud;
        } else {
            tc.line.baud = p.value(QStringLiteral("baud")).toInt();
        }
    }
    // the camera: wait until it delivers (or fails) before the session starts
    CameraFeed feed;
    QString cam = p.value(QStringLiteral("camera"));
    if (cam == QLatin1String("auto")) {
        const auto list = Camera::list(o.demo);
        cam = list.empty() ? QString() : list.front().id;
    }
    if (!cam.isEmpty() && cam != QLatin1String("none")) {
        QEventLoop loop;
        QString camErr;
        QObject::connect(&feed, &CameraFeed::opened, &loop, &QEventLoop::quit);
        QObject::connect(&feed, &CameraFeed::failed, &loop, [&](const QString& e) { camErr = e, loop.quit(); });
        bool opened = false;
        QObject::connect(&feed, &CameraFeed::opened, &loop, [&] { opened = true; });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        feed.open(cam);
        loop.exec();
        if (!camErr.isEmpty()) return fail(QStringLiteral("camera: ") + camErr);
        if (!opened) return fail(QStringLiteral("camera: %1 sent no picture within 60 s").arg(cam));
    }
    Recorder rec;
    rec.setCamera(&feed);
    ControlServer server([&](const QJsonObject& r) { return rec.handleCommand(r); });
    QString err;
    if (!server.listen(&err)) return fail(err);
    double t0 = 0;
    const bool hex = p.isSet(QStringLiteral("hex"));
    bool headerDone = false;
    std::vector<QJsonObject> early;   // events written while starting: printed after the "started" line
    std::function<void(const QJsonObject&)> print = [&](const QJsonObject& e) {
        if (!headerDone) {
            early.push_back(e);
            return;
        }
        if (gJson) {
            out(QString::fromUtf8(QJsonDocument(e).toJson(QJsonDocument::Compact)));
            return;
        }
        const QString line = describeEventLine(e, t0);
        if (!line.isEmpty()) out(line);
        if (hex && e.value(QStringLiteral("kind")).toString() == QLatin1String("data"))
            out(QStringLiteral("            ") + hexView(QByteArray::fromHex(e.value(QStringLiteral("hex")).toString().toLatin1())));
    };
    QObject::connect(&rec, &Recorder::recorded, [&](const QJsonObject& e) { print(e); });
    QObject::connect(&rec, &Recorder::problem, [&](const QString& t) { std::fprintf(stderr, "problem: %s\n", t.toUtf8().constData()); });
    t0 = wallNow();
    if (!rec.start(o, &err)) return fail(err);
    const QJsonObject st = rec.status();
    if (gJson) outJson({{QStringLiteral("ok"), true}, {QStringLiteral("started"), true}, {QStringLiteral("project"), rec.projectFolder()},
                        {QStringLiteral("session"), rec.sessionFolder()}, {QStringLiteral("control"), controlName()}});
    else
        out(QStringLiteral("recording into %1\n%2\nmark: LibreTapCLI mark \"note\"   sync: LibreTapCLI sync   stop: LibreTapCLI stop (or Ctrl-C)")
                .arg(rec.projectFolder(),
                     o.demo ? QStringLiteral("demo instrument")
                            : QStringList{o.tap ? QStringLiteral("serial tap: PC side %1, instrument side %2, %3 baud %4").arg(tc.portA, tc.portB).arg(tc.line.baud).arg(tc.line.text()) : QString(),
                                          o.usb ? QStringLiteral("USB sniffer on %1 (one capture: do one action, then stop)").arg(o.pico.port) : QString(),
                                          o.ethernet ? QStringLiteral("network on %1").arg(o.eth.iface.isEmpty() ? QStringLiteral("(auto)") : o.eth.iface) : QString()}
                                  .filter(QRegularExpression(QStringLiteral("."))).join(QStringLiteral("; "))));
    headerDone = true;
    for (const QJsonObject& e : early) print(e);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QEventLoop loop;
    QObject::connect(&rec, &Recorder::stopped, &loop, &QEventLoop::quit);
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&] {
        if (gStop) rec.stop();
    });
    watch.start(100);
    if (p.isSet(QStringLiteral("seconds"))) QTimer::singleShot(int(p.value(QStringLiteral("seconds")).toDouble() * 1000), &rec, &Recorder::stop);
    loop.exec();
    // let the control client that asked us to stop read its answer
    QEventLoop drain;
    QTimer::singleShot(200, &drain, &QEventLoop::quit);
    drain.exec();
    feed.close();
    const QJsonObject done = rec.status();
    if (gJson) outJson({{QStringLiteral("ok"), true}, {QStringLiteral("stopped"), true}, {QStringLiteral("project"), rec.projectFolder()},
                        {QStringLiteral("session"), rec.sessionFolder()}, {QStringLiteral("stats"), done.value(QStringLiteral("stats"))}});
    else {
        const QJsonObject s = done.value(QStringLiteral("stats")).toObject();
        out(QStringLiteral("stopped: %1 bytes from the PC, %2 from the instrument, %3 marks, %4 stills; saved in %5")
                .arg(s.value(QStringLiteral("bytes_a")).toInt()).arg(s.value(QStringLiteral("bytes_b")).toInt())
                .arg(s.value(QStringLiteral("marks")).toInt()).arg(s.value(QStringLiteral("stills")).toInt()).arg(rec.projectFolder()));
    }
    return 0;
}

int cmdDecodeUsb(const QStringList& args) {
    QString path;
    bool low = false, ftdi = false;
    for (const QString& a : args) {
        if (a == QLatin1String("--low")) low = true;
        else if (a == QLatin1String("--ftdi")) ftdi = true;
        else path = a;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("cannot read %1").arg(path), 2);
    const QString text = QString::fromLatin1(f.readAll());
    UsbDecoder d(low || text.contains(QLatin1String("Capture speed       : Low")), ftdi);
    d.feed(text);
    if (gJson) {
        QJsonArray evs;
        for (const UsbEvent& e : d.events) {
            QJsonObject o{{QStringLiteral("t_ms"), e.tMs}, {QStringLiteral("what"), e.what}};
            if (e.addr >= 0) o.insert(QStringLiteral("addr"), e.addr);
            if (e.ep >= 0) o.insert(QStringLiteral("ep"), e.ep);
            if (!e.dir.isEmpty()) o.insert(QStringLiteral("dir"), e.dir);
            if (!e.data.isEmpty()) o.insert(QStringLiteral("hex"), QString::fromLatin1(e.data.toHex()));
            evs.append(o);
        }
        outJson({{QStringLiteral("ok"), true}, {QStringLiteral("events"), evs}, {QStringLiteral("summary"), QJsonArray::fromStringList(d.summary())}});
        return 0;
    }
    for (const UsbEvent& e : d.events) {
        QString s = QStringLiteral("%1 ms  dev %2 ep%3 %4  %5").arg(e.tMs, 11, 'f', 3).arg(e.addr >= 0 ? QString::number(e.addr) : QStringLiteral("-"), -3)
                        .arg(e.ep >= 0 ? QString::number(e.ep) : QStringLiteral("-"), -2).arg(e.dir, -3).arg(e.what);
        if (!e.data.isEmpty() && !e.control) s += QStringLiteral("  ") + textView(e.data);
        out(s);
        if (!e.data.isEmpty() && e.control) out(QStringLiteral("%1%2").arg(QString(17, QLatin1Char(' ')), hexView(e.data)));
    }
    for (const QString& l : d.summary()) out(l);
    return 0;
}

int cmdPcap(const QStringList& args) {
    QString path, ip;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--ip") && i + 1 < args.size()) ip = args[++i];
        else path = args[i];
    }
    std::vector<Packet> packets;
    QString err;
    if (!PcapReader::readFile(path, &packets, &err)) return fail(err, 2);
    NetFlows f(ip);
    for (const Packet& p : packets) f.add(p);
    f.flush();
    const auto evs = f.take();
    const double t0 = packets.empty() ? 0 : packets.front().t;
    if (gJson) {
        QJsonArray arr;
        for (const auto& e : evs) {
            QJsonObject o = e.fields;
            o.insert(QStringLiteral("t"), e.t);
            o.insert(QStringLiteral("kind"), e.kind);
            arr.append(o);
        }
        outJson({{QStringLiteral("ok"), true}, {QStringLiteral("events"), arr}, {QStringLiteral("stats"), f.stats()}});
        return 0;
    }
    for (const auto& e : evs) {
        if (e.kind == QLatin1String("data"))
            out(QStringLiteral("%1  %2  %3  %4").arg(clockText(e.t - t0), 11).arg(e.fields.value(QStringLiteral("dir")).toString() == QLatin1String("a") ? QStringLiteral("PC>INSTR") : QStringLiteral("INSTR>PC"))
                    .arg(e.fields.value(QStringLiteral("proto")).toString().toUpper(), textView(QByteArray::fromHex(e.fields.value(QStringLiteral("hex")).toString().toLatin1()))));
        else
            out(QStringLiteral("%1  %2").arg(clockText(e.t - t0), 11).arg(e.fields.value(QStringLiteral("text")).toString()));
    }
    out(QString::fromUtf8(QJsonDocument(f.stats()).toJson(QJsonDocument::Compact)));
    return 0;
}

int cmdControl(const QString& cmd, const QStringList& args) {
    QJsonObject req{{QStringLiteral("cmd"), cmd}};
    if (cmd == QLatin1String("mark")) req.insert(QStringLiteral("note"), args.join(QLatin1Char(' ')));
    QJsonObject ans;
    QString err;
    if (!controlRequest(req, &ans, &err)) {
        if (cmd == QLatin1String("status")) {
            if (gJson) outJson({{QStringLiteral("ok"), true}, {QStringLiteral("running"), false}});
            else out(QStringLiteral("no LibreTap is running"));
            return 0;
        }
        return fail(err);
    }
    if (gJson) {
        outJson(ans);
    } else if (!ans.value(QStringLiteral("ok")).toBool()) {
        return fail(ans.value(QStringLiteral("error")).toString());
    } else if (cmd == QLatin1String("mark")) {
        out(QStringLiteral("mark %1 %2").arg(ans.value(QStringLiteral("mark")).toInt()).arg(ans.value(QStringLiteral("note")).toString()));
    } else if (cmd == QLatin1String("sync")) {
        out(QStringLiteral("sync %1").arg(ans.value(QStringLiteral("sync")).toInt()));
    } else {
        const QJsonObject s = ans.value(QStringLiteral("stats")).toObject();
        out(QStringLiteral("%1, %2 s; %3 bytes from the PC, %4 from the instrument, %5 marks, %6 syncs, %7 stills\n%8")
                .arg(ans.value(QStringLiteral("recording")).toBool() ? QStringLiteral("recording") : QStringLiteral("not recording"))
                .arg(ans.value(QStringLiteral("elapsed_s")).toDouble(), 0, 'f', 1)
                .arg(s.value(QStringLiteral("bytes_a")).toInt()).arg(s.value(QStringLiteral("bytes_b")).toInt())
                .arg(ans.value(QStringLiteral("marks")).toInt()).arg(ans.value(QStringLiteral("syncs")).toInt())
                .arg(s.value(QStringLiteral("stills")).toInt()).arg(ans.value(QStringLiteral("project")).toString()));
    }
    return ans.value(QStringLiteral("ok")).toBool() ? 0 : 1;
}

int cmdPorts(bool all) {
    QJsonObject c = connectionsJson();
    if (all) {
        QJsonArray every;
        for (const Adapter& a : listAdapters(true)) every.append(a.toJson());
        c.insert(QStringLiteral("all_ports"), every);
    }
    if (gJson) {
        c.insert(QStringLiteral("ok"), true);
        outJson(c);
        return 0;
    }
    for (const QJsonValue& s : c.value(QStringLiteral("sentences")).toArray()) out(s.toString());
    for (const QJsonValue& a : c.value(all ? QStringLiteral("all_ports") : QStringLiteral("adapters")).toArray())
        out(QStringLiteral("  serial  %1  %2").arg(a.toObject().value(QStringLiteral("path")).toString(), a.toObject().value(QStringLiteral("label")).toString()));
    for (const QJsonValue& a : c.value(QStringLiteral("cameras")).toArray())
        out(QStringLiteral("  camera  %1  %2").arg(a.toObject().value(QStringLiteral("id")).toString(), a.toObject().value(QStringLiteral("name")).toString()));
    for (const QJsonValue& a : c.value(QStringLiteral("usb_sniffers")).toArray())
        out(QStringLiteral("  usb     %1  USB sniffer (Pico)").arg(a.toObject().value(QStringLiteral("path")).toString()));
    for (const QJsonValue& a : c.value(QStringLiteral("interfaces")).toArray())
        out(QStringLiteral("  network %1").arg(a.toObject().value(QStringLiteral("label")).toString()));
    return 0;
}

int cmdGuess(const QStringList& args) {
    QStringList ports;
    double seconds = 2;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--seconds") && i + 1 < args.size()) seconds = args[++i].toDouble();
        else ports << args[i];
    }
    if (ports.isEmpty()) return fail(QStringLiteral("guess PORT [PORT]"), 2);
    const BaudGuess g = guessBaud(ports, seconds, [](int b) {
        if (!gJson) std::fprintf(stderr, "listening at %d baud…\n", b);
    });
    if (gJson) {
        QJsonObject o = g.toJson();
        o.insert(QStringLiteral("ok"), g.baud != 0);
        outJson(o);
    } else {
        for (const auto& r : g.rows)
            out(QStringLiteral("%1 baud: %2 bytes, %3% text  %4").arg(r.baud, 7).arg(r.bytes, 6).arg(int(r.text * 100), 3).arg(textView(r.sample)));
        out(g.sentence);
    }
    return g.baud ? 0 : 1;
}

QJsonObject sessionSummary(const ProjectSession& ps) {
    const Session& s = ps.session;
    return {{QStringLiteral("name"), s.name()},
            {QStringLiteral("folder"), s.folder},
            {QStringLiteral("recorder"), s.recorder()},
            {QStringLiteral("host"), s.meta.value(QStringLiteral("host"))},
            {QStringLiteral("started"), s.meta.value(QStringLiteral("started"))},
            {QStringLiteral("seconds"), s.end() - s.start()},
            {QStringLiteral("data"), s.count(QStringLiteral("data"))},
            {QStringLiteral("marks"), s.count(QStringLiteral("mark"))},
            {QStringLiteral("syncs"), s.count(QStringLiteral("sync"))},
            {QStringLiteral("frames"), s.count(QStringLiteral("frame"))},
            {QStringLiteral("offset"), ps.offset},
            {QStringLiteral("drift"), ps.drift},
            {QStringLiteral("how"), ps.how},
            {QStringLiteral("cut_off"), s.cutOff},
            {QStringLiteral("bad_lines"), s.badLines}};
}

int cmdSessions() {
    const QString ws = workspaceDir();
    QJsonArray arr;
    QDir d(ws);
    for (const QString& n : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        Project p;
        if (!Project::load(d.filePath(n), &p, nullptr)) continue;
        QJsonArray ss;
        for (const ProjectSession& s : p.sessions) ss.append(sessionSummary(s));
        arr.append(QJsonObject{{QStringLiteral("name"), n}, {QStringLiteral("folder"), d.filePath(n)}, {QStringLiteral("project"), p.isSaved()},
                               {QStringLiteral("seconds"), p.end() - p.start()}, {QStringLiteral("sessions"), ss}});
    }
    if (gJson) {
        outJson({{QStringLiteral("ok"), true}, {QStringLiteral("workspace"), ws}, {QStringLiteral("recordings"), arr}});
        return 0;
    }
    out(QStringLiteral("recordings in %1:").arg(ws));
    if (arr.isEmpty()) out(QStringLiteral("  (none yet)"));
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        out(QStringLiteral("  %1  %2").arg(o.value(QStringLiteral("name")).toString(), -28).arg(clockText(o.value(QStringLiteral("seconds")).toDouble(), false)));
        for (const QJsonValue& s : o.value(QStringLiteral("sessions")).toArray()) {
            const QJsonObject so = s.toObject();
            out(QStringLiteral("      %1  %2: %3 traffic, %4 marks, %5 pictures")
                    .arg(so.value(QStringLiteral("name")).toString(), so.value(QStringLiteral("recorder")).toString())
                    .arg(so.value(QStringLiteral("data")).toInt()).arg(so.value(QStringLiteral("marks")).toInt()).arg(so.value(QStringLiteral("frames")).toInt()));
        }
    }
    return 0;
}

int cmdShow(const QStringList& args) {
    QString target;
    bool hex = false, frames = false;
    for (const QString& a : args) {
        if (a == QLatin1String("--hex")) hex = true;
        else if (a == QLatin1String("--frames")) frames = true;
        else target = a;
    }
    const QString path = resolve(target.isEmpty() ? QStringLiteral("latest") : target);
    if (path.isEmpty()) return fail(QStringLiteral("no recording %1 (try LibreTapCLI sessions)").arg(target));
    Project p;
    QString err;
    if (!Project::load(path, &p, &err)) return fail(err);
    Timeline tl;
    tl.build(p);
    if (gJson) {
        QJsonArray ss, rows;
        for (const ProjectSession& s : p.sessions) ss.append(sessionSummary(s));
        for (const TItem& it : tl.rows) {
            const RowText r = describe(p, it);
            const Event& e = tl.event(p, it);
            QJsonObject o{{QStringLiteral("t"), it.t}, {QStringLiteral("rel"), it.t - tl.start}, {QStringLiteral("session"), it.s},
                          {QStringLiteral("kind"), e.kind}, {QStringLiteral("who"), r.who}, {QStringLiteral("text"), r.text}};
            if (!e.o.value(QStringLiteral("hex")).toString().isEmpty()) o.insert(QStringLiteral("hex"), e.o.value(QStringLiteral("hex")));
            rows.append(o);
        }
        QJsonArray pics;
        for (const Picture& pic : tl.pictures) {
            QJsonArray fr;
            if (frames)
                for (const TItem& f : pic.frames)
                    fr.append(QJsonObject{{QStringLiteral("t"), f.t}, {QStringLiteral("rel"), f.t - tl.start},
                                          {QStringLiteral("file"), QDir(p.sessions[f.s].session.folder).filePath(tl.event(p, f).str("file"))},
                                          {QStringLiteral("full"), tl.event(p, f).o.value(QStringLiteral("full")).toBool(true)}});
            QJsonObject po{{QStringLiteral("label"), pic.label}, {QStringLiteral("kind"), pic.kind}, {QStringLiteral("count"), int(pic.frames.size())}};
            if (frames) po.insert(QStringLiteral("frames"), fr);
            pics.append(po);
        }
        outJson({{QStringLiteral("ok"), true}, {QStringLiteral("folder"), path}, {QStringLiteral("start"), tl.start},
                 {QStringLiteral("sessions"), ss}, {QStringLiteral("pictures"), pics}, {QStringLiteral("rows"), rows}});
        return 0;
    }
    out(QStringLiteral("%1: %2 session(s), %3, starts %4").arg(p.name).arg(p.sessions.size()).arg(clockText(tl.end - tl.start)).arg(isoLocal(tl.start)));
    for (const ProjectSession& s : p.sessions) {
        QStringList src;
        for (const QJsonValue& v : s.session.sources()) src << v.toObject().value(QStringLiteral("label")).toString();
        out(QStringLiteral("  %1 (%2): %3; clock %4%5")
                .arg(s.session.name(), s.session.recorder(), src.join(QStringLiteral(", ")), s.how,
                     s.how == QLatin1String("reference") ? QString() : QStringLiteral(" offset %1 s drift %2 ppm").arg(s.offset, 0, 'f', 3).arg(s.drift * 1e6, 0, 'f', 1)));
        if (s.session.cutOff) out(QStringLiteral("  (its last line was cut off and is ignored)"));
    }
    for (const TItem& it : tl.rows) {
        const RowText r = describe(p, it);
        const Event& e = tl.event(p, it);
        const double rel = it.t - tl.start;
        if (e.kind == QLatin1String("mark") || e.kind == QLatin1String("sync"))
            out(QStringLiteral("--- %1 at %2  %3").arg(r.who.toLower(), clockText(rel), e.kind == QLatin1String("mark") ? r.text : QString()));
        else
            out(QStringLiteral("%1  %2  %3").arg(clockText(rel), 11).arg(r.who, -24).arg(r.text));
        if (hex && !r.hex.isEmpty()) out(QStringLiteral("%1%2").arg(QString(39, QLatin1Char(' ')), r.hex));
    }
    for (const Picture& pic : tl.pictures) {
        out(QStringLiteral("pictures: %1, %2 frames").arg(pic.label).arg(pic.frames.size()));
        if (frames)
            for (const TItem& f : pic.frames)
                out(QStringLiteral("%1  %2").arg(clockText(f.t - tl.start), 11).arg(QDir(p.sessions[f.s].session.folder).filePath(tl.event(p, f).str("file"))));
    }
    return 0;
}

bool loadSession(const QString& x, Session* s, QString* err) {
    const QString path = resolve(x);
    if (path.isEmpty()) {
        *err = QStringLiteral("no session %1").arg(x);
        return false;
    }
    return Session::load(path, s, err);
}

void printMatch(const MatchResult& m) {
    if (gJson) {
        outJson(m.toJson());
        return;
    }
    out(m.message);
    if (m.matches)
        out(QStringLiteral("offset %1 s, drift %2 ppm, %3 of %4 pieces, residual %5 ms, confidence %6%, read lag %7 ms")
                .arg(m.offset, 0, 'f', 4).arg(m.drift * 1e6, 0, 'f', 2).arg(m.matches).arg(m.probes)
                .arg(m.residual * 1000, 0, 'f', 2).arg(int(m.confidence * 100)).arg(m.readLag * 1000, 0, 'f', 1));
}

int cmdMatch(const QStringList& args) {
    QStringList names;
    bool save = false;
    for (const QString& a : args) {
        if (a == QLatin1String("--save")) save = true;
        else names << a;
    }
    if (names.size() != 2) return fail(QStringLiteral("match A B: two sessions"), 2);
    QString err;
    if (save) {   // both in one project: store the result there
        Project p;
        if (!Project::load(resolve(names[1]), &p, &err)) return fail(err);
        int ia = -1, ib = -1;
        for (int i = 0; i < int(p.sessions.size()); ++i) {
            if (p.sessions[i].session.folder == QDir(resolve(names[0])).absolutePath()) ia = i;
            if (p.sessions[i].session.folder == QDir(resolve(names[1])).absolutePath()) ib = i;
        }
        if (ia < 0 || ib < 0 || !p.isSaved()) return fail(QStringLiteral("--save needs two sessions of the same recording"));
        const MatchResult m = matchInProject(p, ia, ib);
        if (m.ok && !p.save(&err)) return fail(err);
        printMatch(m);
        return m.ok ? 0 : 1;
    }
    Session a, b;
    if (!loadSession(names[0], &a, &err) || !loadSession(names[1], &b, &err)) return fail(err);
    const MatchResult m = matchSessions(a, b);
    printMatch(m);
    return m.ok ? 0 : 1;
}

int cmdImport(const QStringList& args) {
    if (args.size() != 2) return fail(QStringLiteral("import RECORDING SESSION-FOLDER"), 2);
    Project p;
    QString err;
    if (!Project::load(resolve(args[0]), &p, &err)) return fail(err);
    if (!p.isSaved()) return fail(QStringLiteral("%1 is not a recording folder").arg(args[0]));
    int idx = -1;
    if (!p.addSession(args[1], &err, &idx)) return fail(err);
    const MatchResult m = idx > 0 ? matchInProject(p, 0, idx) : MatchResult{};
    if (!p.save(&err)) return fail(err);
    if (gJson) {
        QJsonObject o = m.toJson();
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("matched"), m.ok);
        o.insert(QStringLiteral("session"), p.absolutePath(p.sessions[idx]));
        outJson(o);
    } else {
        out(QStringLiteral("added %1").arg(p.absolutePath(p.sessions[idx])));
        if (idx > 0) printMatch(m);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QStringList raw;
    for (int i = 1; i < argc; ++i) raw << QString::fromLocal8Bit(argv[i]);
    gJson = raw.removeAll(QStringLiteral("--json")) > 0;
    const QString cmd = raw.isEmpty() ? QString() : raw.takeFirst();
    // recording draws (the demo camera) and encodes JPEG: a GUI application without a display
    std::unique_ptr<QCoreApplication> app;
    if (cmd == QLatin1String("record")) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
        app = std::make_unique<QGuiApplication>(argc, argv);
    } else {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("LibreTap"));
    QCoreApplication::setApplicationName(QStringLiteral("LibreTap"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LIBRETAP_VERSION));
    if (cmd.isEmpty() || cmd == QLatin1String("help") || cmd == QLatin1String("--help") || cmd == QLatin1String("-h")) {
        std::fputs(kUsage, stdout);
        return cmd.isEmpty() ? 2 : 0;
    }
    if (cmd == QLatin1String("--version") || cmd == QLatin1String("version")) {
        out(recorderName());
        return 0;
    }
    if (cmd == QLatin1String("record")) return cmdRecord(raw);
    if (cmd == QLatin1String("status") || cmd == QLatin1String("mark") || cmd == QLatin1String("sync") || cmd == QLatin1String("stop") ||
        cmd == QLatin1String("start"))
        return cmdControl(cmd, raw);
    if (cmd == QLatin1String("ports")) return cmdPorts(raw.contains(QStringLiteral("--all")));
    if (cmd == QLatin1String("cameras")) {
        QJsonArray arr;
        for (const CameraInfo& c : Camera::list(true)) arr.append(QJsonObject{{QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name}});
        if (gJson) outJson({{QStringLiteral("ok"), true}, {QStringLiteral("cameras"), arr}});
        else
            for (const QJsonValue& v : arr) out(QStringLiteral("%1  %2").arg(v.toObject().value(QStringLiteral("id")).toString(), v.toObject().value(QStringLiteral("name")).toString()));
        return 0;
    }
    if (cmd == QLatin1String("guess")) return cmdGuess(raw);
    if (cmd == QLatin1String("sessions")) return cmdSessions();
    if (cmd == QLatin1String("show")) return cmdShow(raw);
    if (cmd == QLatin1String("match")) return cmdMatch(raw);
    if (cmd == QLatin1String("import")) return cmdImport(raw);
    if (cmd == QLatin1String("decode-usb")) return cmdDecodeUsb(raw);
    if (cmd == QLatin1String("pcap")) return cmdPcap(raw);
    return fail(QStringLiteral("unknown command %1; LibreTapCLI help").arg(cmd), 2);
}
