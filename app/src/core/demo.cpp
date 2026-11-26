#include "demo.h"

#include <QDateTime>
#include <QPainter>

#include <atomic>
#include <cmath>

#include "util.h"

namespace lt {

namespace {
// The pretend instrument's state, shared with the pretend screen (the camera runs in its own thread).
std::atomic<int> gIntegrationMs{500};
std::atomic<int> gGain{1};
std::atomic<int> gReading{0};          // last reading × 10000
std::atomic<int> gStep{0};

QByteArray reading() {
    const double t = wallNow();
    const double v = 0.5 + 0.2 * std::sin(t / 7.0) * gIntegrationMs.load() / 1000.0 * gGain.load();
    gReading = int(std::lround(v * 10000));
    return QByteArray("M ") + QByteArray::number(v, 'f', 4) + " V\r\n";
}
}  // namespace

DemoTapSource::DemoTapSource(QObject* parent)
    : SerialTapSource(TapConfig{QStringLiteral("demo-a"), QStringLiteral("demo-b"), QStringLiteral("PC"), QStringLiteral("Instrument"), {}, 20},
                      parent) {
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &DemoTapSource::step);
}

QJsonObject DemoTapSource::describe() const {
    QJsonObject o = SerialTapSource::describe();
    o.insert(QStringLiteral("demo"), true);
    return o;
}

bool DemoTapSource::openLines(QString*) {
    n_ = 0;
    timer_.start();
    return true;
}

void DemoTapSource::closeLines() { timer_.stop(); }

void DemoTapSource::step() {
    ++n_;
    gStep = n_;
    // every 12th poll the "operator" changes a setting; the program sends it, then polls again
    if (n_ % 12 == 0) {
        static const int times[] = {100, 250, 500, 1000, 2000};
        const int ms = times[(n_ / 12) % 5];
        const QByteArray cmd = "SET INT " + QByteArray::number(ms) + "\r";
        bytesIn(0, cmd);
        QTimer::singleShot(12, this, [this, ms] {
            gIntegrationMs = ms;
            bytesIn(1, "OK\r\n");
        });
        return;
    }
    if (n_ % 31 == 0) {
        const int g = gGain.load() == 1 ? 4 : 1;
        bytesIn(0, "SET GAIN " + QByteArray::number(g) + "\r");
        QTimer::singleShot(9, this, [this, g] {
            gGain = g;
            bytesIn(1, "OK\r\n");
        });
        return;
    }
    bytesIn(0, "MEAS?\r");
    // the answer arrives in two pieces, as USB adapters often deliver it
    QTimer::singleShot(15, this, [this] {
        const QByteArray r = reading();
        bytesIn(1, r.left(4));
        QTimer::singleShot(3, this, [this, r] { bytesIn(1, r.mid(4)); });
    });
}

QImage demoScreen(int w, int h, double t) {
    QImage img(w, h, QImage::Format_RGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const double s = w / 640.0;
    p.scale(s, s);
    const int W = 640, H = int(h / s);
    // a photographed CRT: dark surround, slightly blue-grey desktop
    p.fillRect(QRect(0, 0, W, H), QColor(24, 26, 30));
    const QRect screen(30, 20, W - 60, H - 40);
    p.fillRect(screen, QColor(0, 128, 128));   // the teal Windows 9x desktop
    const QRect win(screen.x() + 30, screen.y() + 24, screen.width() - 60, screen.height() - 60);
    p.fillRect(win, QColor(192, 192, 192));
    p.fillRect(QRect(win.x() + 3, win.y() + 3, win.width() - 6, 18), QColor(0, 0, 128));
    QFont f(QStringLiteral("Sans"));
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRect(win.x() + 8, win.y() + 3, win.width(), 18), Qt::AlignVCenter, QStringLiteral("DEMO-METER Control 2.1"));
    f.setBold(false);
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(Qt::black);
    const int x = win.x() + 20;
    int y = win.y() + 46;
    auto row = [&](const QString& k, const QString& v) {
        p.drawText(QPoint(x, y), k);
        p.fillRect(QRect(x + 150, y - 14, 140, 20), Qt::white);
        p.drawText(QPoint(x + 156, y), v);
        y += 30;
    };
    row(QStringLiteral("Integration time"), QStringLiteral("%1 ms").arg(gIntegrationMs.load()));
    row(QStringLiteral("Gain"), QStringLiteral("x%1").arg(gGain.load()));
    row(QStringLiteral("Reading"), QStringLiteral("%1 V").arg(gReading.load() / 10000.0, 0, 'f', 4));
    row(QStringLiteral("Polls"), QString::number(gStep.load()));
    // a little live trace
    const QRect plot(x + 310, win.y() + 34, win.width() - 350, 110);
    p.fillRect(plot, Qt::black);
    p.setPen(QPen(QColor(0, 255, 0), 1.5));
    QPointF last;
    for (int i = 0; i <= 60; ++i) {
        const double tt = t - (60 - i) * 0.5;
        const double v = 0.5 + 0.2 * std::sin(tt / 7.0) * gIntegrationMs.load() / 1000.0 * gGain.load();
        const QPointF pt(plot.x() + i * plot.width() / 60.0, plot.bottom() - std::clamp(v, 0.0, 1.5) / 1.5 * plot.height());
        if (i) p.drawLine(last, pt);
        last = pt;
    }
    // taskbar clock
    p.fillRect(QRect(screen.x(), screen.bottom() - 22, screen.width(), 23), QColor(192, 192, 192));
    p.setPen(Qt::black);
    p.drawText(QRect(screen.right() - 80, screen.bottom() - 22, 74, 23), Qt::AlignCenter,
               QDateTime::fromMSecsSinceEpoch(qint64(t * 1000)).toString(QStringLiteral("HH:mm:ss")));
    // the camera's view: a slight vignette
    QRadialGradient vg(W / 2.0, H / 2.0, W * 0.7);
    vg.setColorAt(0.6, QColor(0, 0, 0, 0));
    vg.setColorAt(1, QColor(0, 0, 0, 120));
    p.fillRect(QRect(0, 0, W, H), vg);
    return img;
}

}  // namespace lt
