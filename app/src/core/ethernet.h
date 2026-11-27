#pragma once
// Ethernet: the instrument and its PC on a switch that copies their traffic to this computer's port (port mirroring,
// e.g. TP-Link TL-SG105E: Monitoring ▸ Port Mirror), captured by Wireshark's dumpcap into raw/eth_*.pcapng (a ring
// of files, so a long recording cannot fill the disk). LibreTap reads the files while they grow (pcap.h) and puts
// the conversations on the timeline like serial traffic.
//
// Capturing needs permission. Linux: dumpcap from wireshark-common with its capture capabilities and the user in
// the "wireshark" group (the .deb sets this up); when the group was just added and this login does not have it
// yet, dumpcap is started through `sg wireshark`, so no new login is needed. Windows: Wireshark with Npcap.
// macOS: Wireshark's ChmodBPF. captureAccess() explains in one sentence what is missing and the command to fix it.
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <map>
#include <memory>

#include "pcap.h"
#include "source.h"

namespace lt {

struct NetInterface {
    QString name;                  // what dumpcap takes: eno1, en0, \Device\NPF_{…}
    QString label;                 // "eno1: wired, cable connected (192.168.1.5)"
    bool wired = false, up = false, virtualIf = false;
    QStringList addresses;
};

// Network interfaces worth offering (virtual ones — docker, bridges, VPN — only with all).
std::vector<NetInterface> listInterfaces(bool all = false);
// The interface that sees the most traffic in `seconds` (Linux: packet counters), else the first wired one up.
QString pickInterface(double seconds = 0.6);

struct CaptureAccess {
    bool ok = false;
    QString program;               // dumpcap's path
    bool viaSg = false;            // start it through `sg wireshark -c` (group not active in this login yet)
    QString sentence;              // plain words: ready, or what is missing and the one command to fix it
};
CaptureAccess captureAccess();
QString dumpcapProgram();          // $LIBRETAP_DUMPCAP, PATH, Wireshark's usual places; empty if none
// Linux package: LibreTap can set capture up itself (installs Wireshark if missing, the wireshark group) through
// pkexec, which asks for the administrator password. Empty when that is not possible here.
QStringList captureSetupCommand();

struct EthernetConfig {
    QString iface;
    QString instrumentIp;          // optional: only traffic to/from it (a capture filter too)
    int ringFileMB = 50, ringFiles = 20;   // at most 1 GB on disk
    double gapMs = 20;
};

class EthernetSource : public Source {
    Q_OBJECT
public:
    explicit EthernetSource(const EthernetConfig& c, QObject* parent = nullptr);
    ~EthernetSource() override;
    QString id() const override { return QStringLiteral("eth"); }
    QString kind() const override { return QStringLiteral("ethernet"); }
    QJsonObject describe() const override;
    bool start(SessionWriter* writer, QString* error) override;
    void stop() override;
    void flush() override;
    QJsonObject stats() const override;

private:
    void readFiles();
    void writeOut();
    EthernetConfig c_;
    SessionWriter* w_ = nullptr;
    std::unique_ptr<QProcess> proc_;
    std::unique_ptr<NetFlows> flows_;
    std::map<QString, std::pair<qint64, std::unique_ptr<PcapReader>>> files_;   // name → (bytes read, reader)
    QTimer poll_;
    QString stderr_;
};

// Reads a capture file into a session's events (import a pcap made elsewhere).
bool importPcap(const QString& pcap, SessionWriter* w, const QString& instrumentIp, QString* error);

}  // namespace lt
