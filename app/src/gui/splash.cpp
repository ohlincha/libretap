#include "splash.h"

#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QRadialGradient>
#include <QScreen>
#include <QStyleHints>

#include <cmath>

namespace lt::gui {

// Semi-realistic, muted: shaded shapes with soft shadows and highlights, desaturated colours. All QPainter.
namespace {

const QColor kTeal("#6fa6a0");

QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

void softShadow(QPainter& p, const QRectF& r, double alpha) {
    QRadialGradient g(r.center(), r.width() / 2);
    g.setColorAt(0, QColor(0, 0, 0, int(255 * alpha)));
    g.setColorAt(1, QColor(0, 0, 0, 0));
    p.save();
    p.translate(r.center());
    p.scale(1, r.height() / r.width());
    p.translate(-r.center());
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(r.center(), r.width() / 2, r.width() / 2);
    p.restore();
}

// A round thing seen from the front: tube, cable, ear tube. Shadowed edge, body, highlight.
void tube(QPainter& p, const QPainterPath& path, double w, const QColor& c, bool shine = true) {
    p.setBrush(Qt::NoBrush);
    QPen pen(c.darker(165), w + 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.drawPath(path);
    pen.setColor(c);
    pen.setWidthF(w);
    p.setPen(pen);
    p.drawPath(path);
    if (shine) {
        QColor h = c.lighter(150);
        h.setAlpha(170);
        pen.setColor(h);
        pen.setWidthF(std::max(0.8, w * 0.28));
        p.setPen(pen);
        p.drawPath(path.translated(-w * 0.18, -w * 0.22));
    }
}

QLinearGradient vgrad(double y0, double y1, const QColor& a, const QColor& b) {
    QLinearGradient g(0, y0, 0, y1);
    g.setColorAt(0, a);
    g.setColorAt(1, b);
    return g;
}

void screw(QPainter& p, QPointF c, double r) {
    QRadialGradient g(c - QPointF(r * 0.3, r * 0.3), r * 1.3);
    g.setColorAt(0, QColor("#e4e6e8"));
    g.setColorAt(1, QColor("#7d838a"));
    p.setPen(QPen(QColor(0, 0, 0, 70), 0.6));
    p.setBrush(g);
    p.drawEllipse(c, r, r);
    p.setPen(QPen(QColor(60, 64, 70, 160), 0.8));
    p.drawLine(c + QPointF(-r * 0.6, r * 0.2), c + QPointF(r * 0.6, -r * 0.2));
}

// The old PC: a beige desktop case and a CRT monitor with depth, a curved glass screen and green text.
void crt(QPainter& p) {
    const QColor beige("#d6cdb8"), beigeDark("#b3a88f"), beigeShade("#9c927a");
    softShadow(p, QRectF(14, 244, 196, 22), 0.45);
    // case: top face (depth), front
    QPainterPath top;
    top.moveTo(26, 214);
    top.lineTo(38, 206);
    top.lineTo(190, 206);
    top.lineTo(180, 214);
    top.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(beige.lighter(108));
    p.drawPath(top);
    QPainterPath side;
    side.moveTo(180, 214);
    side.lineTo(190, 206);
    side.lineTo(190, 240);
    side.lineTo(180, 250);
    side.closeSubpath();
    p.setBrush(beigeShade);
    p.drawPath(side);
    p.setBrush(vgrad(214, 250, beige, beigeDark));
    p.drawRoundedRect(QRectF(26, 214, 154, 36), 2.5, 2.5);
    // drive bays and the power button
    p.setBrush(vgrad(222, 230, beigeDark, beige));
    p.drawRoundedRect(QRectF(36, 221, 62, 9), 1.5, 1.5);
    p.setBrush(QColor(50, 48, 44));
    p.drawRect(QRectF(44, 225, 40, 1.8));
    p.setBrush(vgrad(234, 242, beigeDark, beige));
    p.drawRoundedRect(QRectF(36, 234, 62, 8), 1.5, 1.5);
    QRadialGradient btn(QPointF(158, 230), 7);
    btn.setColorAt(0, beige.lighter(110));
    btn.setColorAt(1, beigeShade);
    p.setBrush(btn);
    p.setPen(QPen(QColor(0, 0, 0, 50), 0.6));
    p.drawEllipse(QPointF(160, 232), 5.5, 5.5);
    p.setPen(Qt::NoPen);
    QRadialGradient led(QPointF(142, 232), 4);
    led.setColorAt(0, QColor("#b9e3b0"));
    led.setColorAt(0.4, QColor("#7fb37a"));
    led.setColorAt(1, QColor(127, 179, 122, 0));
    p.setBrush(led);
    p.drawEllipse(QPointF(142, 232), 4, 4);
    for (int k = 0; k < 5; ++k) {   // vents
        p.setBrush(QColor(0, 0, 0, 35));
        p.drawRoundedRect(QRectF(110 + k * 6, 222, 2.2, 20), 1, 1);
    }

    // monitor neck and stand
    p.setBrush(vgrad(196, 206, beigeShade, beigeDark));
    p.drawRoundedRect(QRectF(84, 192, 52, 14), 3, 3);
    // monitor: side depth, then the front with its bezel
    QPainterPath mside;
    mside.moveTo(170, 98);
    mside.lineTo(184, 106);
    mside.lineTo(184, 186);
    mside.lineTo(170, 196);
    mside.closeSubpath();
    p.setBrush(vgrad(98, 196, beigeDark, beigeShade));
    p.drawPath(mside);
    const QRectF front(48, 96, 124, 102);
    QLinearGradient fg(front.topLeft(), front.bottomRight());
    fg.setColorAt(0, beige.lighter(110));
    fg.setColorAt(1, beigeDark);
    p.setBrush(fg);
    p.drawRoundedRect(front, 9, 9);
    // bezel recess: inner shadow
    const QRectF bezel(60, 106, 100, 76);
    p.setBrush(vgrad(bezel.top(), bezel.bottom(), beigeShade, beige));
    p.drawRoundedRect(bezel, 8, 8);
    // the glass: dark, slightly curved (big radius, radial glow)
    const QRectF glass = bezel.adjusted(5, 5, -5, -5);
    QRadialGradient gl(glass.center(), glass.width() * 0.62);
    gl.setColorAt(0, QColor("#24352c"));
    gl.setColorAt(0.8, QColor("#15201a"));
    gl.setColorAt(1, QColor("#0d140f"));
    p.setBrush(gl);
    QPainterPath g;
    g.addRoundedRect(glass, 12, 10);
    p.drawPath(g);
    p.save();
    p.setClipPath(g);
    // text with a phosphor glow
    const QColor phos("#9fd8a6");
    const double lines[][2] = {{0, 54}, {0, 38}, {8, 46}, {0, 30}, {8, 50}};
    for (int k = 0; k < 5; ++k) {
        const QPointF a(glass.left() + 9 + lines[k][0], glass.top() + 11 + k * 10), b = a + QPointF(lines[k][1], 0);
        QColor glow = phos;
        glow.setAlpha(55);
        p.setPen(QPen(glow, 4.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
        p.setPen(QPen(phos, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
    }
    p.fillRect(QRectF(glass.left() + 9, glass.top() + 58, 5, 2), phos);   // the cursor
    for (double y = glass.top(); y < glass.bottom(); y += 2) p.fillRect(QRectF(glass.left(), y, glass.width(), 0.6), QColor(0, 0, 0, 40));
    // glass reflection: a soft diagonal sheen
    QLinearGradient sheen(glass.topLeft(), glass.center());
    sheen.setColorAt(0, QColor(255, 255, 255, 60));
    sheen.setColorAt(1, QColor(255, 255, 255, 0));
    QPainterPath refl;
    refl.moveTo(glass.topLeft());
    refl.lineTo(glass.left() + glass.width() * 0.62, glass.top());
    refl.lineTo(glass.left(), glass.top() + glass.height() * 0.7);
    refl.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(sheen);
    p.drawPath(refl);
    p.restore();
    // highlight along the top edge of the case and the monitor
    p.setPen(QPen(QColor(255, 255, 255, 90), 1));
    p.drawLine(QPointF(56, 97), QPointF(164, 97));
    // badge and power LED on the monitor
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 40));
    p.drawRoundedRect(QRectF(98, 186, 24, 4), 1.5, 1.5);
    p.setBrush(QColor("#86b48a"));
    p.drawEllipse(QPointF(152, 189), 1.8, 1.8);
}

// The instrument: a bench device with rack ears, a dim vacuum-fluorescent display, knobs and keys.
void instrument(QPainter& p) {
    const QRectF f(292, 160, 112, 82);
    softShadow(p, QRectF(280, 236, 140, 20), 0.45);
    QPainterPath top;
    top.moveTo(f.left(), f.top());
    top.lineTo(f.left() + 10, f.top() - 9);
    top.lineTo(f.right() + 10, f.top() - 9);
    top.lineTo(f.right(), f.top());
    top.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#7d868e"));
    p.drawPath(top);
    QPainterPath side;
    side.moveTo(f.right(), f.top());
    side.lineTo(f.right() + 10, f.top() - 9);
    side.lineTo(f.right() + 10, f.bottom() - 9);
    side.lineTo(f.right(), f.bottom());
    side.closeSubpath();
    p.setBrush(QColor("#4b535a"));
    p.drawPath(side);
    // brushed panel
    QLinearGradient panel(f.topLeft(), f.bottomLeft());
    panel.setColorAt(0, QColor("#8b949b"));
    panel.setColorAt(0.5, QColor("#737c84"));
    panel.setColorAt(1, QColor("#5f676e"));
    p.setBrush(panel);
    p.drawRoundedRect(f, 3, 3);
    for (double y = f.top() + 2; y < f.bottom(); y += 2.5) p.fillRect(QRectF(f.left() + 1, y, f.width() - 2, 0.5), QColor(255, 255, 255, 14));
    // rack ears with screws
    p.setBrush(QColor(0, 0, 0, 30));
    p.drawRect(QRectF(f.left() + 9, f.top() + 2, 1, f.height() - 4));
    p.drawRect(QRectF(f.right() - 10, f.top() + 2, 1, f.height() - 4));
    for (double y : {f.top() + 8, f.bottom() - 8}) {
        screw(p, QPointF(f.left() + 4.5, y), 2.3);
        screw(p, QPointF(f.right() - 4.5, y), 2.3);
    }
    // display: recessed, dim teal digits
    const QRectF d(f.left() + 16, f.top() + 10, 56, 22);
    p.setBrush(vgrad(d.top(), d.bottom(), QColor("#1f2422"), QColor("#2b3230")));
    p.setPen(QPen(QColor(0, 0, 0, 90), 1));
    p.drawRoundedRect(d, 2.5, 2.5);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(12);
    mono.setBold(true);
    p.setFont(mono);
    QColor vfd("#8fcfc3");
    QColor glow = vfd;
    glow.setAlpha(70);
    p.setPen(glow);
    p.drawText(d.translated(0.6, 0.6), Qt::AlignCenter, QStringLiteral("0.5123"));
    p.setPen(vfd);
    p.drawText(d, Qt::AlignCenter, QStringLiteral("0.5123"));
    // a big knob
    const QPointF kc(f.right() - 26, f.top() + 22);
    softShadow(p, QRectF(kc.x() - 12, kc.y() - 6, 26, 26), 0.35);
    QRadialGradient kg(kc - QPointF(4, 5), 15);
    kg.setColorAt(0, QColor("#d9dcdf"));
    kg.setColorAt(0.6, QColor("#9aa0a6"));
    kg.setColorAt(1, QColor("#5f656b"));
    p.setPen(QPen(QColor(0, 0, 0, 80), 0.8));
    p.setBrush(kg);
    p.drawEllipse(kc, 11, 11);
    p.setBrush(QColor("#b9bec3"));
    p.drawEllipse(kc, 6.5, 6.5);
    p.setPen(QPen(QColor("#3a3f44"), 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(kc, kc + QPointF(4.5, -4.5));
    // keys and an LED
    for (int k = 0; k < 3; ++k) {
        const QRectF b(f.left() + 16 + k * 20, f.top() + 46, 15, 9);
        softShadow(p, b.adjusted(-1, 4, 1, 8), 0.25);
        p.setPen(QPen(QColor(0, 0, 0, 70), 0.6));
        p.setBrush(vgrad(b.top(), b.bottom(), QColor("#d3d6d9"), QColor("#9ea4aa")));
        p.drawRoundedRect(b, 2, 2);
    }
    for (int k = 0; k < 2; ++k) {   // small knobs
        const QPointF c(f.left() + 24 + k * 22, f.bottom() - 12);
        QRadialGradient sg(c - QPointF(2, 2), 7);
        sg.setColorAt(0, QColor("#cfd3d6"));
        sg.setColorAt(1, QColor("#5d6369"));
        p.setBrush(sg);
        p.drawEllipse(c, 5, 5);
    }
    QRadialGradient led(QPointF(f.right() - 26, f.bottom() - 12), 5);
    led.setColorAt(0, QColor("#f0c9a8"));
    led.setColorAt(0.45, QColor("#c98f66"));
    led.setColorAt(1, QColor(201, 143, 102, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(led);
    p.drawEllipse(QPointF(f.right() - 26, f.bottom() - 12), 5, 5);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1));
    p.drawLine(QPointF(f.left() + 3, f.top() + 0.5), QPointF(f.right() - 3, f.top() + 0.5));
}

// A DB-9 plug with its hood, the cable leaving towards `dir` (+1 right, -1 left).
void db9(QPainter& p, QPointF c, int dir) {
    p.save();
    p.translate(c);
    p.scale(dir, 1);
    // hood (plastic), shell (metal) toward the device
    QRectF hood(0, -7, 20, 14);
    p.setPen(QPen(QColor(0, 0, 0, 90), 0.7));
    p.setBrush(vgrad(-7, 7, QColor("#6a6e72"), QColor("#3f4347")));
    p.drawRoundedRect(hood, 3, 3);
    p.setBrush(vgrad(-5, 5, QColor("#dfe2e5"), QColor("#8d9399")));
    QPainterPath shell;   // the D shape, seen from the side as a short trapezoid
    shell.moveTo(-6, -5);
    shell.lineTo(0, -6);
    shell.lineTo(0, 6);
    shell.lineTo(-6, 4);
    shell.closeSubpath();
    p.drawPath(shell);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 60));
    p.drawRoundedRect(QRectF(2, -6, 16, 3), 1.5, 1.5);
    screw(p, QPointF(4, -9.5), 2.1);
    screw(p, QPointF(4, 9.5), 2.1);
    p.restore();
}

// The stethoscope: chest piece on the cable, rubber tubing up to the Y, metal ear tubes, rubber tips.
void stethoscope(QPainter& p, QPointF c, double s, const QColor& rubber) {
    const QPointF y = c + QPointF(0, -78 * s);
    QPainterPath t;
    t.moveTo(c + QPointF(0, -10 * s));
    t.cubicTo(c + QPointF(10 * s, -34 * s), y + QPointF(-14 * s, 26 * s), y);
    tube(p, t, 4.6 * s, rubber);
    QPainterPath arms;
    arms.moveTo(y);
    arms.cubicTo(y + QPointF(-24 * s, -4 * s), y + QPointF(-26 * s, -34 * s), y + QPointF(-15 * s, -50 * s));
    arms.moveTo(y);
    arms.cubicTo(y + QPointF(24 * s, -4 * s), y + QPointF(26 * s, -34 * s), y + QPointF(15 * s, -50 * s));
    tube(p, arms, 2.6 * s, QColor("#b8bec4"));
    p.setPen(Qt::NoPen);
    for (int k : {-1, 1}) {
        const QPointF e = y + QPointF(k * 15 * s, -51 * s);
        QRadialGradient eg(e - QPointF(1.3 * s, 1.3 * s), 4.5 * s);
        eg.setColorAt(0, QColor("#7a7f85"));
        eg.setColorAt(1, QColor("#2c3034"));
        p.setBrush(eg);
        p.drawEllipse(e, 4 * s, 4 * s);
    }
    QRadialGradient yg(y - QPointF(1, 1), 4 * s);
    yg.setColorAt(0, QColor("#e9ecee"));
    yg.setColorAt(1, QColor("#80868c"));
    p.setBrush(yg);
    p.drawEllipse(y, 3.2 * s, 3.2 * s);
    // chest piece: a chrome disc with a darker rim and a diaphragm
    softShadow(p, QRectF(c.x() - 15 * s, c.y() + 6 * s, 32 * s, 14 * s), 0.4);
    QRadialGradient cg(c - QPointF(4 * s, 5 * s), 17 * s);
    cg.setColorAt(0, QColor("#f2f4f5"));
    cg.setColorAt(0.55, QColor("#aeb4ba"));
    cg.setColorAt(1, QColor("#62686e"));
    p.setPen(QPen(QColor(0, 0, 0, 90), 0.8));
    p.setBrush(cg);
    p.drawEllipse(c, 13 * s, 13 * s);
    QRadialGradient dg(c + QPointF(2 * s, 2 * s), 11 * s);
    dg.setColorAt(0, QColor("#d8dcdf"));
    dg.setColorAt(1, QColor("#8c9298"));
    p.setPen(QPen(QColor(0, 0, 0, 60), 0.6));
    p.setBrush(dg);
    p.drawEllipse(c, 9 * s, 9 * s);
    p.setPen(QPen(QColor(255, 255, 255, 170), 1.4 * s, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(c.x() - 10 * s, c.y() - 10 * s, 20 * s, 20 * s), 100 * 16, 70 * 16);
}
}  // namespace

bool darkScheme() {
    if (qEnvironmentVariable("LIBRETAP_THEME") == QLatin1String("dark")) return true;
    if (qEnvironmentVariable("LIBRETAP_THEME") == QLatin1String("light")) return false;
    if (const auto* h = QGuiApplication::styleHints(); h && h->colorScheme() != Qt::ColorScheme::Unknown)
        return h->colorScheme() == Qt::ColorScheme::Dark;
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

// The icon: a stethoscope listening on a cable, on a slate tile. Small sizes drop the fine detail.
QImage drawIcon(int size) {
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const bool small = size <= 32;
    const double k = size <= 20 ? 1.6 : 1.0;   // the tiniest: thicker strokes
    p.scale(size / 128.0, size / 128.0);
    QLinearGradient bg(0, 4, 0, 124);
    bg.setColorAt(0, QColor("#4a5763"));
    bg.setColorAt(1, QColor("#2c343c"));
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(4, 4, 120, 120), 28, 28);
    if (!small) {
        QLinearGradient sheen(0, 4, 0, 60);
        sheen.setColorAt(0, QColor(255, 255, 255, 34));
        sheen.setColorAt(1, QColor(255, 255, 255, 0));
        p.setBrush(sheen);
        p.drawRoundedRect(QRectF(5, 5, 118, 60), 27, 27);
    }
    // the cable across the bottom
    QPainterPath cable;
    cable.moveTo(4, 96);
    cable.cubicTo(40, 88, 88, 104, 124, 94);
    if (small) {
        p.setPen(QPen(QColor("#a39d92"), 9 * k, Qt::SolidLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(cable);
    } else {
        tube(p, cable, 8, QColor("#a39d92"));
    }
    // tubing and ear tubes
    const QPointF c(64, 94), y(64, 48);
    QPainterPath t;
    t.moveTo(c);
    t.cubicTo(QPointF(74, 76), QPointF(56, 62), y);
    QPainterPath arms;
    arms.moveTo(y);
    arms.cubicTo(QPointF(42, 46), QPointF(40, 24), QPointF(50, 14));
    arms.moveTo(y);
    arms.cubicTo(QPointF(86, 46), QPointF(88, 24), QPointF(78, 14));
    if (small) {
        p.setPen(QPen(QColor("#dfe3e6"), 7 * k, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(t);
        p.setPen(QPen(QColor("#dfe3e6"), 6 * k, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(arms);
    } else {
        tube(p, t, 6.5, QColor("#8fa3b0"));
        tube(p, arms, 4.2, QColor("#c9ced3"));
        for (int k : {-1, 1}) {
            QRadialGradient eg(QPointF(64 + k * 14 - 1.5, 13), 6);
            eg.setColorAt(0, QColor("#80868c"));
            eg.setColorAt(1, QColor("#2a2e32"));
            p.setPen(Qt::NoPen);
            p.setBrush(eg);
            p.drawEllipse(QPointF(64 + k * 14, 14), 5.5, 5.5);
        }
    }
    // chest piece
    QRadialGradient cg(c - QPointF(6, 7), 26);
    cg.setColorAt(0, QColor("#f4f6f7"));
    cg.setColorAt(0.55, QColor("#b3b9bf"));
    cg.setColorAt(1, QColor("#5d6369"));
    p.setPen(small ? Qt::NoPen : QPen(QColor(0, 0, 0, 90), 1.2));
    p.setBrush(small ? QBrush(QColor("#d3d8dc")) : QBrush(cg));
    p.drawEllipse(c, 19 * k, 19 * k);
    if (!small) {
        QRadialGradient dg(c + QPointF(3, 3), 15);
        dg.setColorAt(0, QColor("#dde1e4"));
        dg.setColorAt(1, QColor("#8a9096"));
        p.setBrush(dg);
        p.setPen(QPen(QColor(0, 0, 0, 60), 0.8));
        p.drawEllipse(c, 13, 13);
    }
    // a muted teal signal dot: it listens
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#6fa6a0"));
    p.drawEllipse(c, small ? 7 * k : 5, small ? 7 * k : 5);
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
    const QColor bg0 = dark ? QColor("#2b3036") : QColor("#f4f1eb"), bg1 = dark ? QColor("#1b1e22") : QColor("#e4dfd5");
    const QColor ink = dark ? QColor("#eef0f1") : QColor("#2b3036"), sub = dark ? QColor("#b3b9bf") : QColor("#5a6168");
    const QColor faint = dark ? QColor("#80878e") : QColor("#8d918f");
    QLinearGradient g(0, 0, 0, h);
    g.setColorAt(0, bg0);
    g.setColorAt(1, bg1);
    p.setPen(dark ? QPen(QColor(255, 255, 255, 18), 1) : QPen(QColor(0, 0, 0, 25), 1));
    p.setBrush(g);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), 18, 18);
    // a soft pool of light behind the scene, and the desk line
    QRadialGradient pool(QPointF(214, 190), 210);
    pool.setColorAt(0, dark ? QColor(255, 255, 255, 18) : QColor(255, 255, 255, 120));
    pool.setColorAt(1, QColor(255, 255, 255, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(pool);
    p.drawEllipse(QPointF(214, 190), 210, 210);

    p.save();
    p.translate(0, 4);
    crt(p);
    instrument(p);
    // the cable: from the PC case's side to the instrument's port, sagging a little, a plug at each end
    QPainterPath cable;
    cable.moveTo(210, 236);
    cable.cubicTo(228, 240, 226, 272, 248, 272);
    cable.cubicTo(270, 272, 262, 224, 272, 222);
    tube(p, cable, 5.4, dark ? QColor("#9b958a") : QColor("#a19b90"));
    db9(p, QPointF(190, 236), 1);    // plugged into the PC case's side
    db9(p, QPointF(292, 222), -1);   // and into the instrument
    stethoscope(p, QPointF(248, 268), 0.95, dark ? QColor("#6f7b86") : QColor("#56616b"));
    p.restore();

    QFont f = QGuiApplication::font();
    f.setPixelSize(44);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(ink);
    p.drawText(QRectF(436, 64, 230, 56), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("LibreTap"));
    f.setPixelSize(18);
    f.setWeight(QFont::Normal);
    f.setItalic(true);
    p.setFont(f);
    p.setPen(dark ? kTeal : QColor("#4c837e"));
    p.drawText(QRectF(438, 124, 230, 54), Qt::AlignLeft, QStringLiteral("Your instrument outlived\nits computer"));
    f.setItalic(false);
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(sub);
    p.drawText(QRectF(438, 186, 220, 60), Qt::AlignLeft | Qt::TextWordWrap,
               QStringLiteral("Record what the old software tells the instrument, next to what its screen shows."));
    f.setPixelSize(12);
    p.setFont(f);
    p.setPen(faint);
    p.drawText(QRectF(438, 262, 230, 20), Qt::AlignLeft, QStringLiteral("Version %1 · MIT licence").arg(QStringLiteral(LIBRETAP_VERSION)));
    (void)mix;
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
