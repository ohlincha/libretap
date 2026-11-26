#pragma once
// LibreTap's look: Fusion with a modern (GNOME/libadwaita-like) palette, light or dark following the desktop
// (as LibreRaman's theme does), flat rounded controls, cards instead of sunken frames, quiet tables, one accent
// (the red Record button) and teal for selection and links. Only LibreTap's own chrome: recorded screen pictures
// are shown as they are.
#include <QColor>
#include <QString>

class QApplication;

namespace lt::gui::theme {

enum class Mode { Auto, Light, Dark };   // Auto follows the desktop; LIBRETAP_THEME=light|dark forces one

void apply(QApplication& app, Mode mode = Mode::Auto);
bool isDark();

QColor accent();          // teal: selection, links, focus
QColor record();          // the Record button
QColor pcColor();         // traffic from the PC
QColor instrumentColor(); // traffic from the instrument
QColor markColor();
QColor syncColor();
QColor ok();
QColor warn();
QColor card();            // card background
QColor border();          // hairlines
QColor dim();             // secondary text
QColor pictureBackground();   // behind camera/screen pictures

}  // namespace lt::gui::theme
