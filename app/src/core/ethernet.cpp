#include "ethernet.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#endif

#include "session.h"
#include "util.h"

namespace lt {

namespace {
bool isVirtual(const QString& n) {
    static const char* const prefixes[] = {"lo", "docker", "veth", "br-", "virbr", "vmnet", "vboxnet", "tun", "tap", "wg", "zt", "tailscale", "utun", "awdl", "llw", "bridge", "anpi", "ap"};
    for (const char* p : prefixes)
        if (n.startsWith(QLatin1String(p))) return true;
    return false;
}

#ifdef Q_OS_LINUX
qint64 rxPackets(const QString& n) {
    QFile f(QStringLiteral("/sys/class/net/%1/statistics/rx_packets").arg(n));
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toLongLong() : -1;
}
#endif
}  // namespace

std::vector<NetInterface> listInterfaces(bool all) {
    std::vector<NetInterface> out;
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        NetInterface n;
        n.name = i.name();
        n.wired = i.type() == QNetworkInterface::Ethernet;
        n.up = i.flags().testFlag(QNetworkInterface::IsRunning);
        n.virtualIf = isVirtual(n.name) || i.type() == QNetworkInterface::Loopback || i.type() == QNetworkInterface::Virtual;
        for (const QNetworkAddressEntry& a : i.addressEntries())
            if (a.ip().protocol() == QAbstractSocket::IPv4Protocol) n.addresses << a.ip().toString();
        if (n.virtualIf && !all) continue;
        const QString what = i.type() == QNetworkInterface::Wifi ? QStringLiteral("Wi-Fi")
                             : n.wired ? QStringLiteral("wired")
                             : i.type() == QNetworkInterface::Loopback ? QStringLiteral("this computer only (loopback)")
                                                                       : QStringLiteral("network");
        QString name = i.humanReadableName();
        if (name != n.name && !name.isEmpty()) name = QStringLiteral("%1 (%2)").arg(name, n.name);
        n.label = QStringLiteral("%1: %2, %3").arg(name, what, n.up ? (n.wired ? QStringLiteral("cable connected") : QStringLiteral("connected"))
                                                                 : (n.wired ? QStringLiteral("no cable") : QStringLiteral("off")));
        if (!n.addresses.isEmpty()) n.label += QStringLiteral(" (%1)").arg(n.addresses.join(QStringLiteral(", ")));
        out.push_back(n);
    }
    std::stable_sort(out.begin(), out.end(), [](const NetInterface& a, const NetInterface& b) {
        return std::make_tuple(!a.up, !a.wired, a.virtualIf) < std::make_tuple(!b.up, !b.wired, b.virtualIf);
    });
    return out;
}

QString pickInterface(double seconds) {
    const auto list = listInterfaces();
#ifdef Q_OS_LINUX
    std::map<QString, qint64> before;
    for (const NetInterface& n : list) before[n.name] = rxPackets(n.name);
    QThread::msleep(static_cast<unsigned long>(seconds * 1000));
    QString best;
    qint64 most = 0;
    for (const NetInterface& n : list) {
        const qint64 d = rxPackets(n.name) - before[n.name];
        if (n.wired && d > most) most = d, best = n.name;   // a mirror port is wired
    }
    if (!best.isEmpty()) return best;
#else
    (void)seconds;
#endif
    for (const NetInterface& n : list)
        if (n.wired && n.up) return n.name;
    return list.empty() ? QString() : list.front().name;
}

QString dumpcapProgram() {
    const QString env = qEnvironmentVariable("LIBRETAP_DUMPCAP");
    if (!env.isEmpty()) return QFileInfo(env).isExecutable() ? env : QString();
    QString p = QStandardPaths::findExecutable(QStringLiteral("dumpcap"));
    if (!p.isEmpty()) return p;
    // Present but not executable for this session is normal on Linux: dumpcap belongs to the wireshark group, and
    // a user just added to it gets the group only at the next login (captureAccess() then runs it through sg).
    for (const QString& c : {QStringLiteral("/usr/bin/dumpcap"), QStringLiteral("/usr/sbin/dumpcap"),
                             QStringLiteral("C:/Program Files/Wireshark/dumpcap.exe"),
                             QStringLiteral("/Applications/Wireshark.app/Contents/MacOS/dumpcap")})
        if (QFileInfo(c).isFile()) return c;
    return {};
}

QStringList captureSetupCommand() {
#if defined(Q_OS_LINUX)
    const QString pk = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pk.isEmpty()) return {};
    QStringList scripts{qEnvironmentVariable("LIBRETAP_SETUP_CAPTURE")};
    if (QCoreApplication::instance()) scripts << QCoreApplication::applicationDirPath() + QStringLiteral("/../libexec/libretap/setup-capture");
    scripts << QStringLiteral("/usr/libexec/libretap/setup-capture");
    for (const QString& sc : scripts)
        if (!sc.isEmpty() && QFileInfo(sc).isExecutable()) return {pk, QFileInfo(sc).canonicalFilePath()};
#endif
    return {};
}

CaptureAccess captureAccess() {
    CaptureAccess a;
    a.program = dumpcapProgram();
    if (a.program.isEmpty()) {
#if defined(Q_OS_WIN)
        a.sentence = QStringLiteral("Ethernet capture needs Wireshark (with Npcap): install it from wireshark.org, then start LibreTap again.");
#elif defined(Q_OS_MACOS)
        a.sentence = QStringLiteral("Ethernet capture needs Wireshark: install it from wireshark.org, including its \"ChmodBPF\" part, then start LibreTap again.");
#else
        a.sentence = QStringLiteral("Ethernet capture needs Wireshark: sudo apt install wireshark");
#endif
        return a;
    }
    if (!qEnvironmentVariableIsEmpty("LIBRETAP_DUMPCAP")) {   // tests: a stand-in
        a.ok = true;
        a.sentence = QStringLiteral("Ethernet capture ready.");
        return a;
    }
#if defined(Q_OS_LINUX)
    if (geteuid() == 0) {
        a.ok = true;
        a.sentence = QStringLiteral("Ethernet capture ready (running as root).");
        return a;
    }
    const QFileInfo fi(a.program);
    const bool forGroup = fi.group() == QLatin1String("wireshark");
    const bool forAll = fi.permission(QFileDevice::ExeOther);   // dumpcap for everyone (capabilities or none)
    group* g = getgrnam("wireshark");
    bool member = false, active = false;
    if (g) {
        const passwd* pw = getpwuid(getuid());
        for (char** m = g->gr_mem; pw && m && *m; ++m) member |= QString::fromLocal8Bit(*m) == QString::fromLocal8Bit(pw->pw_name);
        gid_t groups[256];
        const int n = getgroups(256, groups);
        for (int i = 0; i < n; ++i) active |= groups[i] == g->gr_gid;
        member |= active;
    }
    if (forGroup && member) {
        a.ok = true;
        a.viaSg = !active;
        a.sentence = QStringLiteral("Ethernet capture ready.");
        return a;
    }
    if (forGroup && !member) {
        a.sentence = QStringLiteral("You may not capture network traffic yet. Run once: sudo usermod -aG wireshark $USER");
        return a;
    }
    if (forAll) {   // no group restriction (capabilities for everyone, or none at all: dumpcap will tell)
        a.ok = true;
        a.sentence = QStringLiteral("Ethernet capture ready.");
        return a;
    }
    a.sentence = QStringLiteral("Wireshark's capture helper is for root only. Run once: sudo dpkg-reconfigure wireshark-common (answer Yes), "
                                "then sudo usermod -aG wireshark $USER");
    return a;
#elif defined(Q_OS_MACOS)
    bool bpf = false;
    for (int i = 0; i < 4 && !bpf; ++i) bpf = QFileInfo(QStringLiteral("/dev/bpf%1").arg(i)).isReadable();
    a.ok = bpf;
    a.sentence = bpf ? QStringLiteral("Ethernet capture ready.")
                     : QStringLiteral("macOS does not let LibreTap capture yet: install Wireshark's \"ChmodBPF\" (part of the Wireshark installer), then log in again.");
    return a;
#else
    a.ok = true;   // Windows: Npcap decides; dumpcap's own message is shown if it refuses
    a.sentence = QStringLiteral("Ethernet capture ready (Wireshark found).");
    return a;
#endif
}

// ---- the source

EthernetSource::EthernetSource(const EthernetConfig& c, QObject* parent) : Source(parent), c_(c) {
    poll_.setInterval(200);
    connect(&poll_, &QTimer::timeout, this, [this] {
        readFiles();
        writeOut();
    });
}

EthernetSource::~EthernetSource() {
    if (proc_) {
        proc_->kill();
        proc_->waitForFinished(2000);
    }
}

QJsonObject EthernetSource::describe() const {
    QJsonObject o{{QStringLiteral("id"), id()},
                  {QStringLiteral("kind"), kind()},
                  {QStringLiteral("label"), QStringLiteral("Ethernet on %1").arg(c_.iface)},
                  {QStringLiteral("interface"), c_.iface},
                  {QStringLiteral("names"), QJsonObject{{QStringLiteral("a"), QStringLiteral("PC")}, {QStringLiteral("b"), QStringLiteral("Instrument")}}},
                  {QStringLiteral("raw"), QStringLiteral("raw/eth_*.pcapng")},
                  {QStringLiteral("gap_ms"), c_.gapMs}};
    if (!c_.instrumentIp.isEmpty()) o.insert(QStringLiteral("instrument_ip"), c_.instrumentIp);
    return o;
}

bool EthernetSource::start(SessionWriter* writer, QString* error) {
    w_ = writer;
    files_.clear();
    flows_ = std::make_unique<NetFlows>(c_.instrumentIp, c_.gapMs / 1000.0);
    const CaptureAccess acc = captureAccess();
    if (!acc.ok) {
        if (error) *error = acc.sentence;
        return false;
    }
    if (c_.iface.isEmpty()) c_.iface = pickInterface();
    QStringList args{QStringLiteral("-i"), c_.iface, QStringLiteral("-q"), QStringLiteral("-w"), w_->path(QStringLiteral("raw/eth.pcapng")),
                     QStringLiteral("-b"), QStringLiteral("filesize:%1").arg(c_.ringFileMB * 1024), QStringLiteral("-b"),
                     QStringLiteral("files:%1").arg(c_.ringFiles)};
    if (!c_.instrumentIp.isEmpty()) args << QStringLiteral("-f") << QStringLiteral("host %1").arg(c_.instrumentIp);
    proc_ = std::make_unique<QProcess>();
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc_.get(), &QProcess::readyReadStandardError, this, [this] { stderr_ += QString::fromLocal8Bit(proc_->readAllStandardError()); });
    if (acc.viaSg) {   // the wireshark group is not active in this login yet: sg reads /etc/group
        QStringList quoted;
        for (const QString& s : QStringList{acc.program} + args) quoted << QLatin1Char('\'') + QString(s).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
        proc_->start(QStringLiteral("sg"), {QStringLiteral("wireshark"), QStringLiteral("-c"), QStringLiteral("exec ") + quoted.join(QLatin1Char(' '))});
    } else {
        proc_->start(acc.program, args);
    }
    if (!proc_->waitForStarted(5000)) {
        if (error) *error = QStringLiteral("cannot start %1").arg(acc.program);
        proc_.reset();
        return false;
    }
    // a refusal (permission, no such interface) comes within a moment
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 1200 && proc_->state() != QProcess::NotRunning) proc_->waitForFinished(100);
    if (proc_->state() == QProcess::NotRunning) {
        stderr_ += QString::fromLocal8Bit(proc_->readAllStandardError());
        QString why = stderr_.trimmed().section(QLatin1Char('\n'), 0, 1).simplified();
        if (why.contains(QLatin1String("ermission"))) why = captureAccess().sentence + QStringLiteral(" (") + why + QLatin1Char(')');
        if (error) *error = QStringLiteral("network capture on %1 stopped at once: %2").arg(c_.iface, why.isEmpty() ? QStringLiteral("no reason given") : why);
        proc_.reset();
        return false;
    }
    w_->event(wallNow(), QStringLiteral("status"), {{QStringLiteral("src"), id()}, {QStringLiteral("text"), QStringLiteral("network capture on %1 started").arg(c_.iface)}});
    poll_.start();
    return true;
}

void EthernetSource::readFiles() {
    if (!w_) return;
    QDir raw(w_->path(QStringLiteral("raw")));
    for (const QString& n : raw.entryList({QStringLiteral("eth*.pcapng"), QStringLiteral("eth*.pcap")}, QDir::Files, QDir::Name)) {
        auto& slot = files_[n];
        if (!slot.second) slot.second = std::make_unique<PcapReader>();
        QFile f(raw.filePath(n));
        if (!f.open(QIODevice::ReadOnly) || f.size() <= slot.first) continue;
        f.seek(slot.first);
        const QByteArray more = f.read(f.size() - slot.first);
        slot.first += more.size();
        slot.second->feed(more);
        for (const Packet& p : slot.second->take()) flows_->add(p);
    }
}

void EthernetSource::writeOut() {
    if (!w_) return;
    flows_->poll(wallNow());
    for (const NetFlows::Out& o : flows_->take()) emit recorded(w_->event(o.t, o.kind, o.fields));
}

void EthernetSource::flush() {
    readFiles();
    writeOut();
}

void EthernetSource::stop() {
    poll_.stop();
    if (proc_) {
        proc_->terminate();   // dumpcap writes what it has and closes the file
        if (!proc_->waitForFinished(4000)) proc_->kill(), proc_->waitForFinished(2000);
        proc_.reset();
    }
    readFiles();
    if (flows_) flows_->flush();
    writeOut();
    w_ = nullptr;
}

QJsonObject EthernetSource::stats() const { return flows_ ? flows_->stats() : QJsonObject{}; }

bool importPcap(const QString& pcap, SessionWriter* w, const QString& ip, QString* error) {
    std::vector<Packet> packets;
    if (!PcapReader::readFile(pcap, &packets, error)) return false;
    NetFlows f(ip);
    for (const Packet& p : packets) f.add(p);
    f.flush();
    for (const NetFlows::Out& o : f.take()) w->event(o.t, o.kind, o.fields);
    return true;
}

}  // namespace lt
