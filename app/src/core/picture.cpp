#include "picture.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lt {

namespace {

// The region as a grey image scaled to a fixed width, so measures do not depend on the camera's resolution.
QImage regionGrey(const QImage& image, const QRectF& region, int width = 480) {
    if (image.isNull()) return {};
    QRect r = image.rect();
    if (!region.isEmpty()) {
        r = QRect(int(region.x() * image.width()), int(region.y() * image.height()), int(region.width() * image.width()),
                  int(region.height() * image.height()))
                .intersected(image.rect());
        if (r.width() < 16 || r.height() < 16) r = image.rect();
    }
    QImage g = image.copy(r).convertToFormat(QImage::Format_Grayscale8);
    if (g.width() > width) g = g.scaledToWidth(width, Qt::SmoothTransformation);
    return g;
}

double percentile(const std::vector<int>& hist, long total, double q) {
    long need = long(q * total), acc = 0;
    for (int v = 0; v < 256; ++v) {
        acc += hist[size_t(v)];
        if (acc > need) return v;
    }
    return 255;
}

}  // namespace

Readability readability(const QImage& image, const QRectF& region) {
    Readability r;
    const QImage g = regionGrey(image, region);
    if (g.isNull() || g.width() < 8 || g.height() < 8) {
        r.verdict = QStringLiteral("No picture");
        return r;
    }
    std::vector<int> hist(256, 0);
    long total = 0, clip = 0;
    for (int y = 0; y < g.height(); ++y) {
        const uchar* p = g.constScanLine(y);
        for (int x = 0; x < g.width(); ++x) {
            ++hist[p[x]];
            clip += p[x] >= 250;
        }
        total += g.width();
    }
    r.valid = true;
    r.median = percentile(hist, total, 0.5);
    r.low = percentile(hist, total, 0.02);
    r.high = percentile(hist, total, 0.98);
    r.clipped = double(clip) / double(total);
    r.contrast = r.high - r.low;
    // edge strength: mean absolute Laplacian over the strongest 5 % of pixels, relative to the contrast (so it
    // measures how crisp the edges are, not how bright the picture is)
    std::vector<int> lap;
    lap.reserve(size_t(g.width()) * size_t(g.height()));
    for (int y = 1; y + 1 < g.height(); ++y) {
        const uchar* a = g.constScanLine(y - 1);
        const uchar* b = g.constScanLine(y);
        const uchar* c = g.constScanLine(y + 1);
        for (int x = 1; x + 1 < g.width(); ++x) lap.push_back(std::abs(4 * b[x] - b[x - 1] - b[x + 1] - a[x] - c[x]));
    }
    if (!lap.empty()) {
        const size_t k = lap.size() - std::max<size_t>(1, lap.size() / 20);
        std::nth_element(lap.begin(), lap.begin() + long(k), lap.end());
        double s = 0;
        for (size_t i = k; i < lap.size(); ++i) s += lap[i];
        s /= double(lap.size() - k);
        r.sharpness = r.contrast > 1 ? 100.0 * s / (4.0 * r.contrast) : 0;
    }
    // verdict, worst problem first
    if (r.high < 70) r.verdict = QStringLiteral("Too dark");
    else if (r.clipped > 0.35 || (r.low > 170 && r.clipped > 0.05)) r.verdict = QStringLiteral("Too bright (text burnt out)");
    else if (r.contrast < 60) r.verdict = QStringLiteral("Low contrast");
    else if (r.sharpness < 22) r.verdict = QStringLiteral("Too blurred");
    else r.verdict = QStringLiteral("Text readable");
    const double sContrast = std::clamp((r.contrast - 40) / 120.0, 0.0, 1.0);
    const double sSharp = std::clamp((r.sharpness - 10) / 30.0, 0.0, 1.0);
    const double sClip = std::clamp(1.0 - r.clipped / 0.35, 0.0, 1.0);
    const double sLight = std::clamp((r.high - 40) / 120.0, 0.0, 1.0);
    r.score = int(std::lround(100 * sContrast * sSharp * sClip * sLight));
    if (!r.ok()) r.score = std::min(r.score, 49);
    else r.score = std::max(r.score, 50);
    return r;
}

QRectF detectScreen(const QImage& image) {
    if (image.isNull()) return {};
    QImage g = image.convertToFormat(QImage::Format_Grayscale8).scaled(160, std::max(1, 160 * image.height() / std::max(1, image.width())),
                                                                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int W = g.width(), H = g.height();
    if (W < 16 || H < 9) return {};
    std::vector<int> hist(256, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) ++hist[g.constScanLine(y)[x]];
    const long total = long(W) * H;
    const double lo = percentile(hist, total, 0.1), hi = percentile(hist, total, 0.97);
    if (hi - lo < 40) return {};   // no bright area stands out
    const int thr = int((lo + hi) / 2);
    // bright pixels, with small dark holes (text) closed by a 3×3 maximum
    std::vector<char> on(size_t(total), 0), m(size_t(total), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) on[size_t(y * W + x)] = g.constScanLine(y)[x] >= thr;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            char v = 0;
            for (int dy = -1; dy <= 1 && !v; ++dy)
                for (int dx = -1; dx <= 1 && !v; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < W && yy < H) v = on[size_t(yy * W + xx)];
                }
            m[size_t(y * W + x)] = v;
        }
    // the largest connected bright area
    std::vector<int> label(size_t(total), 0), stack;
    int best = 0, bestN = 0, x0b = 0, y0b = 0, x1b = 0, y1b = 0, next = 0;
    for (int i = 0; i < int(total); ++i) {
        if (!m[size_t(i)] || label[size_t(i)]) continue;
        ++next;
        int n = 0, x0 = W, y0 = H, x1 = -1, y1 = -1;
        stack.push_back(i);
        label[size_t(i)] = next;
        while (!stack.empty()) {
            const int k = stack.back();
            stack.pop_back();
            ++n;
            const int cx = k % W, cy = k / W;
            x0 = std::min(x0, cx), y0 = std::min(y0, cy), x1 = std::max(x1, cx), y1 = std::max(y1, cy);
            const int nb[4] = {k - 1, k + 1, k - W, k + W};
            for (int j = 0; j < 4; ++j) {
                const int q = nb[j];
                if (q < 0 || q >= int(total) || (j < 2 && q / W != cy)) continue;
                if (m[size_t(q)] && !label[size_t(q)]) {
                    label[size_t(q)] = next;
                    stack.push_back(q);
                }
            }
        }
        if (n > bestN) bestN = n, best = next, x0b = x0, y0b = y0, x1b = x1, y1b = y1;
    }
    if (!best) return {};
    const double boxArea = double(x1b - x0b + 1) * (y1b - y0b + 1);
    const double fill = bestN / boxArea, share = boxArea / double(total);
    if (fill < 0.6 || share < 0.03 || share > 0.97) return {};   // not a screen-like rectangle
    return QRectF(double(x0b) / W, double(y0b) / H, double(x1b - x0b + 1) / W, double(y1b - y0b + 1) / H);
}

OptimiseResult optimisePicture(const PictureIO& io, const QRectF& region, double maxSeconds) {
    OptimiseResult res;
    QElapsedTimer clock;
    clock.start();
    auto timeUp = [&] { return clock.elapsed() > maxSeconds * 1000 || (io.cancelled && io.cancelled()); };
    // a settled frame: the camera needs a few frames before a change shows
    auto settled = [&](int skip) {
        QImage img;
        for (int i = 0; i <= skip && !timeUp(); ++i) img = io.grab();
        return img;
    };
    res.before = readability(settled(0), region);
    QStringList done;
    const auto mode = io.control(QStringLiteral("exposure_auto"));
    const auto exposure = io.control(QStringLiteral("exposure"));
    if (mode && exposure) {
        const int manual = mode->kind == CameraControl::Bool ? 0 : 1;   // V4L2: 1 = manual
        io.set(QStringLiteral("exposure_auto"), manual);
        const auto gainC = io.control(QStringLiteral("gain"));
        int e = std::clamp(exposure->value, exposure->min, exposure->max), gain = gainC ? gainC->value : 0;
        const int e0 = e;
        io.set(QStringLiteral("exposure"), e);
        for (int step = 0; step < 12 && !timeUp(); ++step) {
            const Readability r = readability(settled(3), region);
            if (!r.valid) break;
            int ne = e, ng = gain;
            if (r.clipped > 0.02 || r.high >= 250) {
                ne = std::max(exposure->min, int(e * 0.55));
                if (ne == e && gainC && gain > gainC->min) ng = std::max(gainC->min, gain - (gainC->max - gainC->min) / 8);
            } else if (r.high < 205) {
                const double f = std::clamp(232.0 / std::max(1.0, r.high), 1.15, 3.0);
                ne = std::min(exposure->max, int(std::ceil(e * f)));
                if (ne == e && gainC) ng = std::min(gainC->max, gain + std::max(1, (gainC->max - gainC->min) / 8));
            } else {
                break;   // brightest background just below clipping
            }
            if (ne == e && ng == gain) break;   // at the limits
            if (ne != e) io.set(QStringLiteral("exposure"), ne);
            if (ng != gain) io.set(QStringLiteral("gain"), ng);
            e = ne, gain = ng;
        }
        done << QStringLiteral("exposure %1 → %2 ms (manual)").arg(e0 / 10.0, 0, 'f', 1).arg(e / 10.0, 0, 'f', 1);
        if (gainC && gain != gainC->value) done << QStringLiteral("gain %1 → %2").arg(gainC->value).arg(gain);
        res.changed = true;
    } else {
        done << QStringLiteral("exposure cannot be set on this camera");
    }
    // focus
    const auto af = io.control(QStringLiteral("focus_auto"));
    const auto focus = io.control(QStringLiteral("focus"));
    if (af) {
        if (af->value == 0) io.set(QStringLiteral("focus_auto"), 1), res.changed = true;
        settled(8);   // let it find focus
        done << QStringLiteral("autofocus on");
    } else if (focus && !timeUp()) {
        const int steps = 10;
        int bestV = focus->value;
        double bestS = -1;
        for (int i = 0; i <= steps && !timeUp(); ++i) {
            int v = focus->min + (focus->max - focus->min) * i / steps;
            if (focus->step > 1) v = focus->min + (v - focus->min) / focus->step * focus->step;
            io.set(QStringLiteral("focus"), v);
            const double s = readability(settled(2), region).sharpness;
            if (s > bestS) bestS = s, bestV = v;
        }
        io.set(QStringLiteral("focus"), bestV);
        done << QStringLiteral("focus set to %1 (sharpest of a sweep)").arg(bestV);
        res.changed = true;
    }
    res.after = readability(settled(3), region);
    if (timeUp() && io.cancelled && io.cancelled()) done << QStringLiteral("stopped early");
    res.report = done.join(QStringLiteral("; ")) + QStringLiteral(". Now: %1.").arg(res.after.verdict);
    return res;
}

}  // namespace lt
