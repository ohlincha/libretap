#include "util.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QSettings>

#include <chrono>
#include <cmath>

namespace lt {

double wallNow() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() * 1e-6;
}

QString textView(const QByteArray& b, int maxBytes) {
    QString out;
    const int n = maxBytes < 0 ? int(b.size()) : std::min(int(b.size()), maxBytes);
    out.reserve(n * 2);
    for (int i = 0; i < n; ++i) {
        const unsigned char x = static_cast<unsigned char>(b[i]);
        if (x == 13) out += QLatin1String("\\r");
        else if (x == 10) out += QLatin1String("\\n");
        else if (x == 9) out += QLatin1String("\\t");
        else if (x == 92) out += QLatin1String("\\\\");
        else if (x >= 32 && x < 127) out += QChar(x);
        else out += QStringLiteral("\\x%1").arg(x, 2, 16, QLatin1Char('0'));
    }
    if (n < b.size()) out += QStringLiteral("…");
    return out;
}

QString hexView(const QByteArray& b, int maxBytes) {
    const QByteArray part = maxBytes < 0 ? b : b.left(maxBytes);
    QString s = QString::fromLatin1(part.toHex(' '));
    if (part.size() < b.size()) s += QStringLiteral(" …");
    return s;
}

static QDateTime toDateTime(double t) {
    return QDateTime::fromMSecsSinceEpoch(qint64(std::llround(t * 1000.0)));
}

QString isoLocal(double t) { return toDateTime(t).toOffsetFromUtc(toDateTime(t).offsetFromUtc()).toString(Qt::ISODateWithMs); }

QString stampName(double t) { return toDateTime(t).toString(QStringLiteral("yyyyMMdd-HHmmss")); }

QString clockText(double s, bool millis) {
    const bool neg = s < 0;
    s = std::fabs(s);
    const qint64 ms = qint64(std::llround(s * 1000));
    const qint64 h = ms / 3600000, m = ms / 60000 % 60, sec = ms / 1000 % 60, f = ms % 1000;
    QString out = h ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(sec, 2, 10, QLatin1Char('0'))
                    : QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QLatin1Char('0'));
    if (millis) out += QStringLiteral(".%1").arg(f, 3, 10, QLatin1Char('0'));
    return neg ? QLatin1Char('-') + out : out;
}

QByteArray eventLine(double t, const QString& kind, const QJsonObject& fields) {
    QByteArray line = "{\"t\":" + QByteArray::number(t, 'f', 6) + ",\"kind\":" +
                      QJsonDocument(QJsonObject{{QStringLiteral("k"), kind}}).toJson(QJsonDocument::Compact).mid(5).chopped(1);
    QJsonObject rest = fields;
    rest.remove(QStringLiteral("t"));
    rest.remove(QStringLiteral("kind"));
    if (!rest.isEmpty()) line += ',' + QJsonDocument(rest).toJson(QJsonDocument::Compact).mid(1).chopped(1);
    return line + "}\n";
}

QString workspaceDir() {
    const QString env = qEnvironmentVariable("LIBRETAP_HOME");
    if (!env.isEmpty()) return env;
    const QString chosen = QSettings(QStringLiteral("LibreTap"), QStringLiteral("LibreTap")).value(QStringLiteral("workspace")).toString();
    if (!chosen.isEmpty()) return chosen;
    return QDir::home().filePath(QStringLiteral("LibreTap"));
}

QString recorderName() { return QStringLiteral("LibreTap " LIBRETAP_VERSION); }

}  // namespace lt
