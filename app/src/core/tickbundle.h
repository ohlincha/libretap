// Tick ships inside LibreTap's installers (Windows: <app>/tick, macOS: LibreTap.app/Contents/Resources/tick,
// Linux: /usr/share/libretap/tick), so an operator can carry it to the old PC: copyTick() puts the portable files
// in a "Tick" folder on a USB stick. Nothing is installed on the old PC; the files run from the stick.
#pragma once

#include <QString>
#include <QStringList>

namespace lt {

// The folder holding the bundled Tick (Tick.exe, TickEyes.exe, tick for Linux, README.md, SAFETY.md), or empty
// when this LibreTap was built without it. $LIBRETAP_TICK_DIR overrides.
QString bundledTickDir();

// The files in the bundled Tick folder (names only).
QStringList bundledTickFiles();

// Copies the bundled Tick into <destination>/Tick (made if needed; existing files are replaced). The Linux `tick`
// stays executable. Returns false with a reason; *copied gets the files written (full paths).
bool copyTick(const QString& destination, QStringList* copied, QString* error);

}  // namespace lt
