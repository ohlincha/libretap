#include "protocol.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

#include "util.h"

namespace lt {

namespace {
int number(const QJsonValue& v) {
    if (v.isDouble()) return int(v.toDouble());
    const QString s = v.toString().trimmed();
    bool ok = false;
    const int n = s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? s.mid(2).toInt(&ok, 16) : s.toInt(&ok);
    return ok ? n : -1;
}

QString formatNumber(double v) {
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e12) return QString::number(qint64(std::llround(v)));
    return QString::number(v, 'g', 6);
}

// raw value of a byte field, or nothing
bool readBytes(const QByteArray& d, const ProtoField& f, double* raw, QString* str) {
    const QString t = f.type;
    if (t == QLatin1String("ascii") || t == QLatin1String("hex")) {
        if (f.offset < 0 || f.offset >= d.size()) return false;
        const QByteArray part = d.mid(f.offset, f.length > 0 ? f.length : -1);
        *str = t == QLatin1String("ascii") ? QString::fromLatin1(part) : hexView(part);
        return true;
    }
    int n = 1;
    if (t.contains(QLatin1String("16"))) n = 2;
    else if (t.contains(QLatin1String("32"))) n = 4;
    if (f.offset < 0 || f.offset + n > d.size()) return false;
    quint32 v = 0;
    const bool be = t.endsWith(QLatin1String("be"));
    for (int k = 0; k < n; ++k) {
        const quint32 b = uchar(d[f.offset + (be ? k : n - 1 - k)]);
        v = v << 8 | b;
    }
    double x = v;
    if (t.startsWith(QLatin1Char('i'))) {
        if (n == 1) x = qint8(v);
        else if (n == 2) x = qint16(v);
        else x = qint32(v);
    }
    *raw = x;
    return true;
}

QString render(const ProtoField& f, double raw, const QString& str, bool isString) {
    if (!f.values.isEmpty()) {
        const QString key = isString ? str : formatNumber(raw);
        if (f.values.contains(key)) return f.values.value(key).toString();
        const QString hexKey = QStringLiteral("0x%1").arg(qint64(raw), 2, 16, QLatin1Char('0'));
        if (!isString && f.values.contains(hexKey)) return f.values.value(hexKey).toString();
    }
    if (isString) return str;
    if (f.format == QLatin1String("hex")) return QStringLiteral("0x%1").arg(qint64(raw), 2, 16, QLatin1Char('0'));
    if (f.format == QLatin1String("bits")) return QStringLiteral("%1").arg(qint64(raw), f.type.contains(QLatin1String("16")) ? 16 : 8, 2, QLatin1Char('0'));
    const double v = raw * f.scale + f.add;
    return formatNumber(v) + (f.unit.isEmpty() ? QString() : QStringLiteral(" ") + f.unit);
}
}  // namespace

bool Protocol::load(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1").arg(path);
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("%1: %2 (at byte %3)").arg(path, pe.errorString()).arg(pe.offset);
        return false;
    }
    file = path;
    return parse(doc.object(), error);
}

bool Protocol::parse(const QJsonObject& o, QString* error) {
    if (o.value(QStringLiteral("format")).toString() != QLatin1String("libretap-protocol")) {
        if (error) *error = QStringLiteral("not a libretap-protocol file (\"format\" is missing)");
        return false;
    }
    name = o.value(QStringLiteral("name")).toString();
    notes = o.value(QStringLiteral("notes")).toString();
    messages.clear();
    int k = 0;
    for (const QJsonValue& v : o.value(QStringLiteral("messages")).toArray()) {
        ++k;
        const QJsonObject m = v.toObject();
        ProtoMessage pm;
        pm.name = m.value(QStringLiteral("name")).toString(QStringLiteral("message %1").arg(k));
        pm.say = m.value(QStringLiteral("say")).toString(pm.name);
        pm.dir = m.value(QStringLiteral("dir")).toString(QStringLiteral("any"));
        pm.device = m.value(QStringLiteral("device")).toString().toLower();
        pm.source = m.value(QStringLiteral("source")).toString();
        pm.ep = m.contains(QStringLiteral("ep")) ? number(m.value(QStringLiteral("ep"))) : -1;
        if (m.contains(QStringLiteral("text"))) {
            pm.hasText = true;
            pm.text = QRegularExpression(m.value(QStringLiteral("text")).toString(), QRegularExpression::DotMatchesEverythingOption);
            if (!pm.text.isValid()) {
                if (error) *error = QStringLiteral("message \"%1\": the text pattern is not valid: %2").arg(pm.name, pm.text.errorString());
                return false;
            }
        }
        if (m.contains(QStringLiteral("hex"))) {
            pm.hasHex = true;
            QString h = m.value(QStringLiteral("hex")).toString().simplified();
            if (h.endsWith(QChar(0x2026)) || h.endsWith(QLatin1Char('*'))) pm.open = true, h.chop(1);
            for (const QString& b : h.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const bool any = b == QLatin1String("??");
                pm.pattern += char(any ? 0 : b.toInt(nullptr, 16));
                pm.mask += char(any ? 0 : 1);
            }
        }
        if (m.contains(QStringLiteral("usb"))) {
            pm.hasUsb = true;
            const QJsonObject u = m.value(QStringLiteral("usb")).toObject();
            for (auto it = u.begin(); it != u.end(); ++it) pm.usb.emplace_back(it.key(), number(it.value()));
        }
        for (const QJsonValue& fv : m.value(QStringLiteral("fields")).toArray()) {
            const QJsonObject fo = fv.toObject();
            ProtoField f;
            f.name = fo.value(QStringLiteral("name")).toString();
            f.type = fo.value(QStringLiteral("type")).toString(QStringLiteral("int"));
            f.from = fo.value(QStringLiteral("from")).toString();
            f.unit = fo.value(QStringLiteral("unit")).toString();
            f.format = fo.value(QStringLiteral("format")).toString();
            f.group = fo.contains(QStringLiteral("group")) ? fo.value(QStringLiteral("group")).toInt() : -1;
            f.offset = fo.contains(QStringLiteral("offset")) ? fo.value(QStringLiteral("offset")).toInt() : -1;
            f.length = fo.value(QStringLiteral("length")).toInt(0);
            f.scale = fo.value(QStringLiteral("scale")).toDouble(1);
            f.add = fo.value(QStringLiteral("add")).toDouble(0);
            f.values = fo.value(QStringLiteral("values")).toObject();
            pm.fields.push_back(f);
        }
        messages.push_back(pm);
    }
    return true;
}

Decoded Protocol::decode(const Project& p, const TItem& it, const Timeline& tl) const {
    Decoded d;
    const Event& e = tl.event(p, it);
    const bool usb = e.kind == QLatin1String("usb");
    if (e.kind != QLatin1String("data") && !usb) return d;
    const Session& s = p.sessions[size_t(it.s)].session;
    const QByteArray setup = QByteArray::fromHex(e.str("setup").toLatin1());
    if (usb && setup.size() != 8) return d;
    const QByteArray data = e.bytes();
    d.total = int(data.size()) + (usb ? 8 : 0);
    const QString dir = e.str("dir");
    const bool toInst = usb || dir == QLatin1String("a") || dir == QLatin1String("out");
    for (size_t k = 0; k < messages.size(); ++k) {
        const ProtoMessage& m = messages[k];
        if (m.dir == QLatin1String("to-instrument") && !toInst) continue;
        if (m.dir == QLatin1String("from-instrument") && toInst) continue;
        if (!m.device.isEmpty() && e.str("dev").toLower() != m.device) continue;
        if (!m.source.isEmpty() && s.sourceKind(e.src) != m.source) continue;
        if (m.ep >= 0 && e.o.value(QStringLiteral("ep")).toInt(-1) != m.ep) continue;
        if (m.hasUsb != usb) continue;
        QRegularExpressionMatch tm;
        if (m.hasText) {
            tm = m.text.match(QString::fromLatin1(data));
            if (!tm.hasMatch()) continue;
        }
        if (m.hasHex) {
            if (data.size() < m.pattern.size() || (!m.open && data.size() != m.pattern.size())) continue;
            bool ok = true;
            for (int i = 0; i < m.pattern.size() && ok; ++i) ok = !m.mask[i] || data[i] == m.pattern[i];
            if (!ok) continue;
        }
        if (m.hasUsb) {
            bool ok = true;
            for (const auto& [f, v] : m.usb) {
                int got = -1;
                if (f == QLatin1String("bmRequestType")) got = uchar(setup[0]);
                else if (f == QLatin1String("bRequest")) got = uchar(setup[1]);
                else if (f == QLatin1String("wValue")) got = uchar(setup[2]) | uchar(setup[3]) << 8;
                else if (f == QLatin1String("wIndex")) got = uchar(setup[4]) | uchar(setup[5]) << 8;
                else if (f == QLatin1String("wLength")) got = uchar(setup[6]) | uchar(setup[7]) << 8;
                ok &= got == v;
            }
            if (!ok) continue;
        }
        // matched: the fields
        QString say = m.say;
        int fieldBytes = 0;
        for (const ProtoField& f : m.fields) {
            double raw = 0;
            QString str;
            bool isString = false, got = false;
            if (f.group >= 0 && m.hasText) {
                str = tm.captured(f.group);
                got = !str.isNull();
                if (f.type == QLatin1String("text")) isString = true;
                else if (f.type == QLatin1String("hex")) raw = str.toLongLong(nullptr, 16);
                else raw = str.toDouble();
            } else if (!f.from.isEmpty() && usb) {
                got = true;
                if (f.from == QLatin1String("wValue")) raw = uchar(setup[2]) | uchar(setup[3]) << 8;
                else if (f.from == QLatin1String("wIndex")) raw = uchar(setup[4]) | uchar(setup[5]) << 8;
                else if (f.from == QLatin1String("wLength")) raw = uchar(setup[6]) | uchar(setup[7]) << 8;
                else if (f.from == QLatin1String("bRequest")) raw = uchar(setup[1]);
                else got = false;
            } else if (f.offset >= 0) {
                got = readBytes(data, f, &raw, &str);
                isString = f.type == QLatin1String("ascii") || f.type == QLatin1String("hex");
                if (got) fieldBytes += f.length > 0 ? f.length : f.type.contains(QLatin1String("32")) ? 4 : f.type.contains(QLatin1String("16")) ? 2 : 1;
            }
            say.replace(QLatin1Char('{') + f.name + QLatin1Char('}'), got ? render(f, raw, str, isString) : QStringLiteral("?"));
        }
        d.message = int(k);
        d.sentence = say;
        d.explained = (m.hasText || m.hasHex) ? d.total : (usb ? 8 + std::min<int>(fieldBytes, int(data.size())) : std::min(fieldBytes, d.total));
        if (usb && m.fields.empty()) d.explained = 8;
        if (usb && data.isEmpty()) d.explained = d.total;
        return d;
    }
    return d;
}

QJsonObject Coverage::toJson() const {
    QJsonArray un;
    std::vector<std::pair<QString, std::pair<int, qint64>>> v(unexplained.begin(), unexplained.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    for (const auto& [f, c] : v) un.append(QJsonObject{{QStringLiteral("family"), f}, {QStringLiteral("messages"), c.first}, {QStringLiteral("bytes"), double(c.second)}});
    return {{QStringLiteral("percent_bytes"), percent()}, {QStringLiteral("bytes_explained"), double(explained)}, {QStringLiteral("bytes"), double(total)},
            {QStringLiteral("messages"), messages}, {QStringLiteral("messages_explained"), matched}, {QStringLiteral("unexplained"), un}};
}

}  // namespace lt
