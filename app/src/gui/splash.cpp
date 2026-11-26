#include "splash.h"

#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QScreen>
#include <QStyleHints>

namespace lt::gui {

namespace {
const QColor kAccent("#2bb0a6");     // the signal: teal
const QColor kWarm("#f2a541");       // the stethoscope: brass

// The stethoscope: chest piece at c on the wire, tubing rising to two ear tips.
void stethoscope(QPainter& p, QPointF c, double s, const QColor& tube) {
    QPen pen(tube, 4.5 * s);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPainterPath t;
    t.moveTo(c + QPointF(0, -9 * s));
    t.cubicTo(c + QPointF(0, -40 * s), c + QPointF(-4 * s, -52 * s), c + QPointF(0, -64 * s));
    p.drawPath(t);
    const QPointF y = c + QPointF(0, -64 * s);
    QPainterPath arms;
    arms.moveTo(y);
    arms.cubicTo(y + QPointF(-22 * s, -6 * s), y + QPointF(-24 * s, -30 * s), y + QPointF(-14 * s, -44 * s));
    arms.moveTo(y);
    arms.cubicTo(y + QPointF(22 * s, -6 * s), y + QPointF(24 * s, -30 * s), y + QPointF(14 * s, -44 * s));
    p.drawPath(arms);
    p.setPen(Qt::NoPen);
    p.setBrush(kWarm);
    p.drawEllipse(y + QPointF(-14 * s, -44 * s), 4.5 * s, 4.5 * s);
    p.drawEllipse(y + QPointF(14 * s, -44 * s), 4.5 * s, 4.5 * s);
    p.drawEllipse(c, 13 * s, 13 * s);
    p.setBrush(QColor(255, 255, 255, 200));
    p.drawEllipse(c, 6 * s, 6 * s);
}

// A square-wave signal along a wire from x0 to x1 at y.
void wire(QPainter& p, double x0, double x1, double y, double amp, double s, bool pulses) {
    QPainterPath w;
    w.moveTo(x0, y);
    if (pulses) {
        double x = x0 + 10 * s;
        w.lineTo(x, y);
        const int bits[] = {1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0};
        int prev = 0, k = 0;
        const double step = 9 * s;
        while (x + step < x1 - 10 * s) {
            const int b = bits[k++ % 14];
            if (b != prev) w.lineTo(x, y - b * amp);
            x += step;
            w.lineTo(x, y - b * amp);
            prev = b;
        }
        w.lineTo(x, y);
    }
    w.lineTo(x1, y);
    QPen pen(kAccent, 3 * s);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(w);
}
}  // namespace

bool darkScheme() {
    if (const auto* h = QGuiApplication::styleHints(); h && h->colorScheme() != Qt::ColorScheme::Unknown)
        return h->colorScheme() == Qt::ColorScheme::Dark;
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QImage drawIcon(int size) {
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const double s = size / 128.0;
    QLinearGradient bg(0, 0, 0, size);
    bg.setColorAt(0, QColor("#22324a"));
    bg.setColorAt(1, QColor("#121a27"));
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(4 * s, 4 * s, 120 * s, 120 * s), 26 * s, 26 * s);
    wire(p, 14 * s, 114 * s, 94 * s, 14 * s, s, true);
    stethoscope(p, QPointF(64 * s, 94 * s), 0.72 * s, QColor("#e8edf5"));
    return img;
}

QIcon appIcon() {
    QIcon icon;
    for (int sz : {16, 24, 32, 48, 64, 128, 256}) icon.addPixmap(QPixmap::fromImage(drawIcon(sz)));
    return icon;
}

QImage drawSplash(qreal dpr, bool dark) {
    const int w = kSplashW, h = kSplashH;
    QImage img(QSize(int(w * dpr), int(h * dpr)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QColor bg0 = dark ? QColor("#1d2738") : QColor("#fbf7ef"), bg1 = dark ? QColor("#0f151f") : QColor("#efe7d8");
    const QColor ink = dark ? QColor("#ffffff") : QColor("#1f2a3a"), sub = dark ? QColor("#b8c2d6") : QColor("#4a5568");
    const QColor faint = dark ? QColor("#7d879b") : QColor("#8a8f99");
    QLinearGradient g(0, 0, 0, h);
    g.setColorAt(0, bg0);
    g.setColorAt(1, bg1);
    p.setPen(dark ? Qt::NoPen : QPen(QColor("#d9cfbd"), 1));
    p.setBrush(g);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), 18, 18);

    p.save();
    p.translate(0, -22);
    // the old PC: beige monitor with a green-text CRT on a desktop case
    const QColor beige("#d8cfb8"), beigeDark("#b9ae94");
    p.setPen(Qt::NoPen);
    p.setBrush(beigeDark);
    p.drawRoundedRect(QRectF(28, 214, 120, 22), 3, 3);                        // case
    p.setBrush(QColor("#4a4a4a"));
    p.drawRect(QRectF(40, 222, 34, 3));                                      // floppy slot
    p.setBrush(beige);
    p.drawRoundedRect(QRectF(38, 118, 100, 88), 8, 8);                       // monitor
    p.drawRect(QRectF(72, 204, 32, 10));                                     // neck
    p.setBrush(QColor("#11261a"));
    p.drawRoundedRect(QRectF(48, 127, 80, 62), 10, 10);                      // CRT
    p.setPen(QPen(QColor("#5dff8f"), 2));
    for (int k = 0; k < 4; ++k) p.drawLine(QPointF(56, 138 + k * 11), QPointF(56 + (k % 2 ? 40 : 58), 138 + k * 11));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#7ad97a"));
    p.drawEllipse(QPointF(128, 198), 2.2, 2.2);                              // power LED

    // the instrument: a box with a little display and knobs
    p.setBrush(dark ? QColor("#3b4a61") : QColor("#5b6b82"));
    p.drawRoundedRect(QRectF(250, 168, 96, 68), 6, 6);
    p.setBrush(QColor("#0d1b14"));
    p.drawRoundedRect(QRectF(260, 178, 46, 20), 3, 3);
    p.setPen(QPen(QColor("#ffb347"), 1.6));
    p.drawLine(QPointF(265, 188), QPointF(300, 188));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#c9d3e0"));
    p.drawEllipse(QPointF(324, 188), 7, 7);
    p.drawEllipse(QPointF(272, 220), 5, 5);
    p.drawEllipse(QPointF(290, 220), 5, 5);
    p.setBrush(QColor("#ff6b5a"));
    p.drawEllipse(QPointF(332, 222), 3, 3);

    // the cable between them, signal on it, and the stethoscope listening
    wire(p, 140, 258, 225, 9, 1, true);
    stethoscope(p, QPointF(199, 225), 0.95, dark ? QColor("#e8edf5") : QColor("#2a3547"));
    p.restore();

    QFont f = QGuiApplication::font();
    f.setPixelSize(44);
    f.setBold(true);
    p.setFont(f);
    p.setPen(ink);
    p.drawText(QRectF(376, 64, 250, 56), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("LibreTap"));
    f.setPixelSize(19);
    f.setBold(false);
    f.setItalic(true);
    p.setFont(f);
    p.setPen(dark ? kAccent.lighter(120) : kAccent.darker(130));
    p.drawText(QRectF(378, 124, 250, 54), Qt::AlignLeft, QStringLiteral("Your instrument outlived\nits computer"));
    f.setItalic(false);
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(sub);
    p.drawText(QRectF(378, 190, 240, 60), Qt::AlignLeft | Qt::TextWordWrap,
               QStringLiteral("Record what the old software tells the instrument, next to what its screen shows."));
    f.setPixelSize(12);
    p.setFont(f);
    p.setPen(faint);
    p.drawText(QRectF(378, 262, 245, 20), Qt::AlignLeft, QStringLiteral("Version %1 · MIT licence").arg(QStringLiteral(LIBRETAP_VERSION)));
    p.end();
    return img;
}

static qreal screenDpr() {
    const QScreen* s = QGuiApplication::primaryScreen();
    return s ? s->devicePixelRatio() : 1.0;
}

Splash::Splash() : QSplashScreen(QPixmap::fromImage(drawSplash(screenDpr(), darkScheme()))) {
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Click to continue"));
}

void Splash::mousePressEvent(QMouseEvent* e) { QSplashScreen::mousePressEvent(e); }

}  // namespace lt::gui
