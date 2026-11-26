#include "match.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <map>

namespace lt {

namespace {

struct Piece {
    double t;
    QByteArray data;
};

// Direction role of a data event: 0 = PC → instrument, 1 = instrument → PC, -1 = not link traffic.
int role(const Session& s, const Event& e) {
    if (e.kind != QLatin1String("data")) return -1;
    const QString dir = e.str("dir");
    if (dir == QLatin1String("a") || dir == QLatin1String("out")) return 0;
    if (dir == QLatin1String("b") || dir == QLatin1String("in")) return 1;
    (void)s;
    return -1;
}

double byteTime(const Session& s, const Event& e) {
    const int baud = s.source(e.src).value(QStringLiteral("baud")).toInt();
    return baud > 0 ? 10.0 / baud : 0.0;
}

std::vector<Piece> pieces(const Session& s, int r) {
    std::vector<Piece> out;
    for (const Event& e : s.events)
        if (role(s, e) == r) {
            QByteArray b = e.bytes();
            if (!b.isEmpty()) out.push_back({e.t, std::move(b)});
        }
    return out;
}

// One direction's bytes, concatenated, with the time of each byte.
struct Stream {
    QByteArray bytes;
    std::vector<double> times;
    QHash<quint32, std::vector<int>> index;   // 3-byte key → positions
};

Stream stream(const Session& s, int r) {
    Stream st;
    for (const Event& e : s.events) {
        if (role(s, e) != r) continue;
        const QByteArray b = e.bytes();
        const double bt = byteTime(s, e);
        for (int i = 0; i < b.size(); ++i) st.times.push_back(e.t + i * bt);
        st.bytes += b;
    }
    const auto* p = reinterpret_cast<const uchar*>(st.bytes.constData());
    for (int i = 0; i + 3 <= st.bytes.size(); ++i) st.index[quint32(p[i]) << 16 | quint32(p[i + 1]) << 8 | p[i + 2]].push_back(i);
    return st;
}

// Candidate time differences (ref - other) for each piece of `other`: where its bytes occur in the reference.
struct Probe {
    double t;
    std::vector<double> dts;
};

std::vector<Probe> probes(const Stream& ref, const std::vector<Piece>& other, int maxLen = 48, int maxOcc = 2000) {
    std::vector<Probe> out;
    for (const Piece& pc : other) {
        if (pc.data.size() < 3) continue;   // a lone ACK occurs everywhere
        const QByteArray needle = pc.data.left(maxLen);
        const auto* n = reinterpret_cast<const uchar*>(needle.constData());
        const auto it = ref.index.constFind(quint32(n[0]) << 16 | quint32(n[1]) << 8 | n[2]);
        if (it == ref.index.constEnd() || int(it->size()) > maxOcc) continue;
        Probe p{pc.t, {}};
        for (int pos : *it)
            if (pos + needle.size() <= ref.bytes.size() && std::equal(needle.begin(), needle.end(), ref.bytes.begin() + pos))
                p.dts.push_back(ref.times[pos] - pc.t);
        if (!p.dts.empty()) out.push_back(std::move(p));
    }
    return out;
}

struct Fit {
    double a = 0, b = 0;
    int n = 0;
    double rms = 0;
};

// Least squares y = a + b (x - x0); b only when the points span enough time to tell.
Fit lineFit(const std::vector<std::pair<double, double>>& xy, double x0, bool withSlope) {
    Fit f;
    f.n = int(xy.size());
    if (xy.empty()) return f;
    double sx = 0, sy = 0;
    for (const auto& [x, y] : xy) sx += x - x0, sy += y;
    const double mx = sx / f.n, my = sy / f.n;
    double sxx = 0, sxy = 0;
    for (const auto& [x, y] : xy) sxx += (x - x0 - mx) * (x - x0 - mx), sxy += (x - x0 - mx) * (y - my);
    f.b = withSlope && sxx > 0 ? sxy / sxx : 0;
    f.a = my - f.b * mx;
    double ss = 0;
    for (const auto& [x, y] : xy) ss += std::pow(y - (f.a + f.b * (x - x0)), 2);
    f.rms = std::sqrt(ss / f.n);
    return f;
}

QString ppm(double drift) { return QStringLiteral("%1 ppm").arg(drift * 1e6, 0, 'f', 1); }

}  // namespace

QJsonObject MatchResult::toJson() const {
    return {{QStringLiteral("ok"), ok},           {QStringLiteral("method"), method},   {QStringLiteral("offset"), offset},
            {QStringLiteral("drift"), drift},     {QStringLiteral("t0"), t0},           {QStringLiteral("matches"), matches},
            {QStringLiteral("probes"), probes},   {QStringLiteral("residual"), residual}, {QStringLiteral("confidence"), confidence},
            {QStringLiteral("read_lag"), readLag}, {QStringLiteral("message"), message}};
}

MatchResult matchTraffic(const Session& ref, const Session& other) {
    MatchResult r;
    r.method = QStringLiteral("traffic");
    r.t0 = other.start();
    const Stream refStream[2] = {stream(ref, 0), stream(ref, 1)};
    std::vector<Probe> pr[2] = {probes(refStream[0], pieces(other, 0)), probes(refStream[1], pieces(other, 1))};
    r.probes = int(pr[0].size() + pr[1].size());
    if (refStream[0].bytes.isEmpty() && refStream[1].bytes.isEmpty()) {
        r.message = QStringLiteral("%1 has no link traffic to match").arg(ref.name());
        return r;
    }
    if (r.probes == 0) {
        r.message = QStringLiteral("no traffic in %1 occurs in %2").arg(other.name(), ref.name());
        return r;
    }
    // writes alone when there are enough of them (reads lag)
    std::vector<Probe> use = pr[0];
    if (use.size() < 3) use.insert(use.end(), pr[1].begin(), pr[1].end());

    // coarse: a histogram of all candidates of both directions (the answers carry the unique content of a polling
    // program; their lag of tens of ms is small against the bins), 0.1 s bins, each piece one vote shared among
    // its candidates
    const double bin = 0.1;
    std::map<long long, double> hist;
    for (const auto& dirProbes : pr)
        for (const Probe& p : dirProbes)
            for (double dt : p.dts) hist[(long long)std::floor(dt / bin)] += 1.0 / p.dts.size();
    auto windowSum = [&](long long b) {
        double s = 0;
        for (long long k = b - 1; k <= b + 1; ++k)
            if (auto it = hist.find(k); it != hist.end()) s += it->second;
        return s;
    };
    long long best = 0;
    double bestSum = -1;
    for (const auto& [b, w] : hist) {
        (void)w;
        const double s = windowSum(b);
        if (s > bestSum) bestSum = s, best = b;
    }
    double second = 0;
    for (const auto& [b, w] : hist) {
        (void)w;
        if (std::llabs(b - best) > 3) second = std::max(second, windowSum(b));
    }
    r.confidence = bestSum / (bestSum + second);

    // refine: the closest candidate of each piece within a shrinking window around the current line
    Fit fit;
    fit.a = (best + 0.5) * bin;
    std::vector<std::pair<double, double>> xy;
    for (double window : {0.2, 0.1, 0.05, 0.03, 0.03}) {
        xy.clear();
        for (const Probe& p : use) {
            const double model = fit.a + fit.b * (p.t - r.t0);
            double bestDt = 0, bestErr = 1e300;
            for (double dt : p.dts)
                if (std::abs(dt - model) < bestErr) bestErr = std::abs(dt - model), bestDt = dt;
            if (bestErr < window) xy.emplace_back(p.t, bestDt);
        }
        if (xy.size() < 2) break;
        const double span = std::max_element(xy.begin(), xy.end())->first - std::min_element(xy.begin(), xy.end())->first;
        fit = lineFit(xy, r.t0, xy.size() >= 5 && span >= 30);
    }
    r.matches = int(xy.size());
    r.offset = fit.a;
    r.drift = fit.b;
    r.residual = fit.rms;
    // the reads' lag behind the fit (informative)
    std::vector<double> lags;
    for (const Probe& p : pr[1]) {
        const double model = fit.a + fit.b * (p.t - r.t0);
        double bestErr = 1e300, lag = 0;
        for (double dt : p.dts)
            if (std::abs(dt - model) < bestErr) bestErr = std::abs(dt - model), lag = model - dt;
        if (bestErr < 1.0) lags.push_back(lag);
    }
    if (!lags.empty()) {
        std::nth_element(lags.begin(), lags.begin() + lags.size() / 2, lags.end());
        r.readLag = lags[lags.size() / 2];
    }
    r.ok = r.matches >= 3 && r.confidence > 0.6;
    if (r.ok)
        r.message = QStringLiteral("Clocks matched by traffic: %1 is %2 s %3, drift %4 (%5 matching pieces, ±%6 ms).")
                        .arg(other.name())
                        .arg(std::abs(r.offset), 0, 'f', 3)
                        .arg(r.offset >= 0 ? QStringLiteral("behind") : QStringLiteral("ahead"))
                        .arg(ppm(r.drift))
                        .arg(r.matches)
                        .arg(r.residual * 1000, 0, 'f', 1);
    else if (r.matches >= 3)
        r.message = QStringLiteral("The traffic matches in more than one place (the program repeats itself; confidence %1 %). "
                                   "Press Sync on both recorders next time.").arg(int(r.confidence * 100));
    else
        r.message = QStringLiteral("Too little traffic in common to match the clocks (%1 pieces).").arg(r.matches);
    return r;
}

MatchResult matchSync(const Session& ref, const Session& other) {
    MatchResult r;
    r.method = QStringLiteral("sync");
    r.t0 = other.start();
    std::map<int, double> a;
    for (const Event& e : ref.events)
        if (e.kind == QLatin1String("sync")) a[e.o.value(QStringLiteral("n")).toInt()] = e.t;
    std::vector<std::pair<double, double>> xy;
    for (const Event& e : other.events)
        if (e.kind == QLatin1String("sync"))
            if (auto it = a.find(e.o.value(QStringLiteral("n")).toInt()); it != a.end()) xy.emplace_back(e.t, it->second - e.t);
    r.probes = r.matches = int(xy.size());
    if (xy.empty()) {
        r.message = QStringLiteral("no Sync presses with the same number in both sessions");
        return r;
    }
    const double span = xy.size() > 1 ? std::max_element(xy.begin(), xy.end())->first - std::min_element(xy.begin(), xy.end())->first : 0;
    const Fit f = lineFit(xy, r.t0, xy.size() >= 2 && span >= 60);
    r.offset = f.a;
    r.drift = f.b;
    r.residual = f.rms;
    r.confidence = 1;
    r.ok = true;
    r.message = QStringLiteral("Clocks matched by %1 Sync press%2: %3 is %4 s %5 (about ±0.3 s).")
                    .arg(r.matches)
                    .arg(r.matches == 1 ? QString() : QStringLiteral("es"))
                    .arg(other.name())
                    .arg(std::abs(r.offset), 0, 'f', 3)
                    .arg(r.offset >= 0 ? QStringLiteral("behind") : QStringLiteral("ahead"));
    return r;
}

MatchResult matchSessions(const Session& ref, const Session& other) {
    MatchResult t = matchTraffic(ref, other);
    if (t.ok) return t;
    MatchResult s = matchSync(ref, other);
    if (s.ok) return s;
    t.message += QStringLiteral(" ") + s.message[0].toUpper() + s.message.mid(1) + QStringLiteral(".");
    return t;
}

MatchResult matchInProject(Project& p, int ref, int other) {
    MatchResult m = matchSessions(p.sessions[ref].session, p.sessions[other].session);
    if (!m.ok) return m;
    ProjectSession& o = p.sessions[other];
    const ProjectSession& rs = p.sessions[ref];
    // compose with the reference's own relation to project time
    o.t0 = m.t0;
    o.offset = rs.toProject(m.t0 + m.offset) - m.t0;
    o.drift = (1 + m.drift) * (1 + rs.drift) - 1;
    o.how = m.method;
    o.match = m.toJson();
    return m;
}

void matchAll(Project& p) {
    for (int i = 1; i < int(p.sessions.size()); ++i)
        if (p.sessions[i].how != QLatin1String("manual")) matchInProject(p, 0, i);
}

}  // namespace lt
