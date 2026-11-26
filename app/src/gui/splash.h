#pragma once
// Splash screen: an old beige PC and an instrument joined by a cable, a stethoscope listening on the cable, the
// name and the tagline. Drawn with QPainter in the desktop's light or dark scheme; a click hides it.
#include <QIcon>
#include <QImage>
#include <QSplashScreen>

namespace lt::gui {

constexpr int kSplashW = 640, kSplashH = 330;

QImage drawSplash(qreal dpr = 1.0, bool dark = true);   // logical size kSplashW × kSplashH
QImage drawIcon(int size);                              // the program icon: a stethoscope on a signal wire
QIcon appIcon();
bool darkScheme();

class Splash : public QSplashScreen {
    Q_OBJECT
public:
    Splash();

protected:
    void mousePressEvent(QMouseEvent* e) override;
};

}  // namespace lt::gui
